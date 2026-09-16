"""Recompute bounded attribution and TCP observations; preserve failed runs."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def quantile(values, p):
    values = sorted(values)
    if not values:
        return None
    index = (len(values) - 1) * p
    lo = int(index)
    return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (index - lo)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    attribution, latency, failures = [], [], []
    identities = {}
    for path in sorted(args.directory.rglob("*.json")):
        if path.name == "analysis-followup.json":
            continue
        d = read(path)
        if not isinstance(d, dict):
            continue
        if d.get("kind") == "exit-attribution":
            if d.get("status") != "ok":
                failures.append(str(path.relative_to(args.directory)))
                continue
            identity = identities.setdefault("attribution", d["identity"])
            assert d["identity"] == identity, "attribution binary or boot identity changed"
            assert d["exitCode"] == 0
            wanted = 0 if d["condition"] == "off" else d["expectedResidentProcessors"]
            for side in ("before", "after"):
                state = d[side]
                assert state["bootUtc"] == identity["bootUtc"], "attribution boot changed"
                assert state["hvm"]["exitCode"] == 0 and state["hvm"]["parsed"]["residentProcessorCount"] == wanted
                assert not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(state["hvm"]["parsed"]["stateNames"])
            rows = [json.loads(s) for s in d["stdout"].splitlines() if s.startswith("{")]
            assert rows[0]["affinityResult"] == 0 and rows[0]["pinCpu"] == 0
            result = next(r for r in rows if "nsPerOperation" in r)
            assert result["status"] == "ok" and all(math.isfinite(result[k]) and result[k] > 0 for k in ("nsPerOperation", "seconds", "operations"))
            attribution.append(dict(runId=d["runId"], round=d["round"], warmup=d["warmup"],
                                    condition=d["condition"], workload=d["workload"],
                                    nsPerOperation=result["nsPerOperation"], seconds=result["seconds"]))
        elif d.get("kind") == "application-latency":
            if d.get("status") != "ok":
                failures.append(str(path.relative_to(args.directory)))
                continue
            identity = identities.setdefault("latency", d["identity"])
            assert d["identity"] == identity, "latency binary identity changed"
            rows = [json.loads(s) for s in path.with_suffix(".jsonl").read_text().splitlines()]
            header = rows[0]
            control = next(r for r in rows if r["kind"] == "control")
            samples = [r for r in rows if r["kind"] == "request"]
            final = rows[-1]
            assert final["kind"] == "final" and final["error"] == final["workerError"] == 0
            assert samples and control["waitResult"] == control["exitCode"] == 0
            previous = 0
            for seq, row in enumerate(samples, 1):
                assert row["valid"] and row["seq"] == seq and row["endQpc"] >= row["beginQpc"]
                assert row["previousEndQpc"] == previous
                assert row["beginQpc"] >= previous
                previous = row["endQpc"]
            frequency = header["qpcFrequency"]
            assert frequency > 0 and control["endQpc"] >= control["beginQpc"]
            rtts = [(r["endQpc"] - r["beginQpc"]) * 1e6 / frequency for r in samples]
            gaps = [(r["endQpc"] - r["previousEndQpc"]) * 1e6 / frequency for r in samples[1:]]
            overlapping = [r for r in samples[1:] if r["previousEndQpc"] <= control["endQpc"]
                           and r["endQpc"] >= control["beginQpc"]]
            assert overlapping, "No observations overlap the control call"
            row = dict(runId=d["runId"], round=d["round"], warmup=d["warmup"], condition=d["condition"],
                       requests=len(samples), rttP50Us=quantile(rtts, .5), rttP95Us=quantile(rtts, .95),
                       rttP99Us=quantile(rtts, .99), rttMaxUs=max(rtts), completionGapMaxUs=max(gaps),
                       commandOverlapGapMaxUs=max((r["endQpc"] - r["previousEndQpc"]) * 1e6 / frequency for r in overlapping),
                       commandDurationMs=(control["endQpc"] - control["beginQpc"]) * 1000 / frequency)
            metric = d["after"]["metrics"]
            assert metric["transitionCoherent"] and d["before"]["bootUtc"] == d["after"]["bootUtc"]
            assert d["before"]["processes"] == d["after"]["processes"]
            if d["condition"] in ("insert", "remove"):
                assert int(metric["commandBeginQpc"]) >= control["beginQpc"]
                assert int(metric["commandEndQpc"]) <= control["endQpc"]
                mf = int(metric["qpcFrequency"])
                assert mf == frequency, "different QPC frequency domains"
                row["driverCommandUs"] = (int(metric["commandEndQpc"]) - int(metric["commandBeginQpc"])) * 1e6 / mf
                g = metric["globalQpc"]
                row["rendezvousUs"] = (int(g["rendezvousEnd"]) - int(g["rendezvousBegin"])) * 1e6 / mf
            else:
                row["driverCommandUs"] = row["rendezvousUs"] = None
            latency.append(row)
    summary = {"attribution": [], "latency": [], "failedRecords": failures,
               "scope": "One machine/boot; paced closed-loop TCP. Gaps include Sleep(1), logging, scheduler and socket costs. Control-call overlap is not exact driver pause."}
    for rows, key in [(attribution, "attribution"), (latency, "latency")]:
        if not rows:
            continue
        with (args.directory / f"{key}-runs.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        groups = sorted({(r["condition"], r.get("workload", "")) for r in rows if not r["warmup"]})
        for condition, workload in groups:
            selected = [r for r in rows if not r["warmup"] and r["condition"] == condition and r.get("workload", "") == workload]
            columns = ["nsPerOperation"] if key == "attribution" else ["rttP50Us", "rttP99Us", "rttMaxUs", "commandOverlapGapMaxUs", "commandDurationMs"]
            result = dict(condition=condition, workload=workload, n=len(selected))
            for column in columns:
                values = [r[column] for r in selected]
                result[column] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
            summary[key].append(result)
    (args.directory / "analysis-followup.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
