"""Extract durable per-run records and compare matched nested guest workloads."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics


def extract(path, destination):
    serial = path.read_text(encoding="utf-8-sig").replace("\r", "")
    destination.mkdir(parents=True, exist_ok=True)
    runs = []
    # The first baseline collector used 'paper-before' for its envelope only.
    for match in re.finditer(
        r"paper-(?:linux|before)-run-begin ([\w-]+)\n(.*?)"
        r"paper-(?:linux|before)-run-end \1(?:\n|$)", serial, re.S
    ):
        lines = match[2].splitlines()
        records = [json.loads(line) for line in lines if line.startswith("{")]
        starts = [r for r in records if r.get("kind") == "run-start"]
        ends = [r for r in records if r.get("kind") == "run-end"]
        if len(starts) != 1 or len(ends) != 1:
            raise ValueError(f"ambiguous record: {match[1]}")
        start, end = starts[0], ends[0]
        if start["runId"] != end["runId"] or start["runId"] != match[1]:
            raise ValueError("envelope identity mismatch")
        metrics = [r for r in records if r.get("schemaVersion") == 1 and "workload" in r]
        metadata = [r for r in records if r.get("kind") == "metadata"]
        identity = start.get("binaryMD5", "")
        # Preserve the failed earlier collector separately; it lacks its promised hash.
        eligible = (len(identity) == 32 and start["iteration"] > 0 and len(metadata) == 1
                    and metadata[0].get("affinityResult") == 0 and metadata[0].get("pinCpu") == 0)
        row = {**start, "exitCode": end["exitCode"], "endGuestUptime": end["guestUptime"],
               "eligible": eligible, "metrics": metrics, "raw": match[2],
               "serialSha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        (destination / (match[1] + ".json")).write_text(json.dumps(row, indent=2) + "\n")
        runs.append(row)
    if not runs:
        raise ValueError(f"no complete runs in {path}")
    return runs


def summarize(runs):
    groups = {}
    for run in runs:
        if not run["eligible"]:
            continue
        key = run["workload"]
        groups.setdefault(key, []).append(run)
    result = {}
    for key, rows in groups.items():
        seconds = [m["seconds"] for r in rows if r["exitCode"] == 0
                   for m in r["metrics"] if m["status"] == "ok"]
        result[key] = {"runs": len(rows), "successes": len(seconds),
                       "failures": sum(r["exitCode"] != 0 for r in rows),
                       "medianSeconds": statistics.median(seconds) if seconds else None,
                       "seconds": seconds, "binaryMD5": sorted({r["binaryMD5"] for r in rows}),
                       "bootIds": sorted({r["bootId"] for r in rows})}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    derived = args.root / "derived"
    derived.mkdir(exist_ok=True)
    summaries = {}
    for condition in ("before", "after"):
        folder = args.root / (condition + "-nested")
        summaries[condition] = summarize(extract(folder / "serial.txt", derived / (condition + "-linux-runs")))
    rows = []
    for workload in sorted(set(summaries["before"]) & set(summaries["after"])):
        old, new = summaries["before"][workload], summaries["after"][workload]
        if old["binaryMD5"] != new["binaryMD5"] or len(old["binaryMD5"]) != 1:
            raise ValueError(f"benchmark binary drift: {workload}")
        a, b = old["medianSeconds"], new["medianSeconds"]
        rows.append({"workload": workload, "beforeSeconds": a, "afterSeconds": b,
                     "elapsedReductionPercent": 100 * (1 - b / a) if a and b else None,
                     "beforeSuccesses": old["successes"], "beforeFailures": old["failures"],
                     "afterSuccesses": new["successes"], "afterFailures": new["failures"]})
    with (derived / "nested-improvements.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = {"method": "Same static binary, 4 Windows vCPUs / 2 TinyCore vCPUs, pinned CPU 0. "
              "One warmup and three measured iterations. A 20-second timeout is a censored failure, "
              "never zero latency. Before/after are different driver builds and guest boots; "
              "this is an implementation comparison, not overhead relative to an absent hypervisor.",
              "summaries": summaries, "comparison": rows}
    (derived / "nested-bench.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
