"""Join real HTTP responses to the fixed-schema business oracle and controls."""
import argparse
import json
from pathlib import Path
import re

SAMPLE = re.compile(r"paper-http-sample-v2 (\d+) ([\d.]+) (\d+) (\d+) ([a-f0-9]{32}) (\d+) (\d+) (\d+) ([a-f0-9-]{36})$")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def parse(serial):
    samples, offset, current = [], 0, None
    for line in serial.splitlines(keepends=True):
        text = line.strip()
        match = SAMPLE.fullmatch(text)
        if match:
            s, uptime, rc, size, digest, pid, ticks, alive, boot = match.groups()
            current = dict(offset=offset, seq=int(s), uptime=float(uptime), rc=int(rc), bytes=int(size),
                           md5=digest, pid=int(pid), ticks=int(ticks), alive=int(alive), boot=boot)
            samples.append(current)
        elif current is not None and text.startswith('{"kind":'):
            value = json.loads(text)
            if value["kind"] == "application-page":
                current["holder"] = value
            elif value["kind"] == "policy-oracle":
                current["oracle"] = value
        offset += len(line)
    return samples


def check(samples, wanted, minimum):
    complete = [s for s in samples if "holder" in s and "oracle" in s]
    assert len(complete) >= minimum, f"only {len(complete)} complete application observations"
    identities = {(s["pid"], s["ticks"], s["boot"], s["holder"]["pid"], s["holder"]["gpa"]) for s in complete}
    assert len(identities) == 1, "application, guest or held-page identity changed"
    assert all(s["rc"] == 0 and s["bytes"] == 4096 and s["alive"] == 1 and s["holder"]["samePfn"] for s in complete)
    assert all(s["oracle"]["bytes"] == 4096 and s["oracle"]["validPolicy"] == wanted and
               s["oracle"]["decision"] == ("quote-1250" if wanted else "reject-corrupt-policy") for s in complete)
    digest = "53398dd1f3a4c282ace3b660acd92ff7" if wanted else "620f0b67a91f7f74151bc5be745b7110"
    assert all(s["md5"] == digest for s in complete), "unexpected response body"
    assert all(-.1 <= s["uptime"] - s["holder"]["uptime"] <= 5 for s in complete), "stale PFN sample"
    assert len({s["holder"]["uptime"] for s in complete}) >= 2, "PFN observer did not advance"
    assert all(-.1 <= s["oracle"]["uptime"] - s["uptime"] <= 5 for s in complete), "stale policy decision"
    assert all(a["uptime"] < b["uptime"] for a, b in zip(complete, complete[1:])), "nonmonotonic requests"
    return dict(count=len(complete), incompleteObservations=len(samples)-len(complete),
                identity=list(next(iter(identities))), decision=complete[0]["oracle"]["decision"])


def continuity(directory):
    before, after = (read(directory / f"inplace-{side}.json") for side in ("before", "after"))
    assert before["bootUtc"] == after["bootUtc"], "Windows reboot during comparator"
    identity = lambda state: sorted((p["name"], p["pid"], p["createdUtc"]) for p in state["processes"])
    assert identity(before) == identity(after), "Windows/VMM process identity changed"
    assert sum(p["name"] == "vmware-vmx.exe" for p in before["processes"]) == 1, "missing VMM"
    assert before["driverSha256"] == after["driverSha256"], "driver changed"
    for state in (before, after):
        assert state["hvm"]["residentProcessorCount"] == 4, "residency changed"
        assert not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(state["hvm"]["stateNames"])
        assert state["page"]["active"] == state["page"]["retired"] == 0, "EPT mapping remains"
    return dict(result="pass", bootUtc=before["bootUtc"], processes=before["processes"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    with (args.directory / "serial.txt").open(encoding="utf-8-sig", newline="") as f:
        serial = f.read()
    samples = parse(serial)
    result = dict(ept=[], inplace=[], scope="A fixed-schema HTTP consumer and guest-resident pinned page. This demonstrates equivalent corruption/recovery effects and different control requirements, not production fault tolerance or superior speed.")
    provenance = args.directory / "measured-policy-sources.json"
    result["holderLiveness"] = read(provenance)["holderLiveness"] if provenance.exists() else "unspecified"
    for path in sorted(args.directory.glob("http-page-*.json")):
        d = read(path)
        row = dict(runId=d["runId"], result="incomplete")
        try:
            assert d["status"] == "recorded_requires_analysis", d.get("error", d["status"])
            stages = {}
            for step in d["steps"]:
                selected = [s for s in samples if step["serialOffset"] <= s["offset"] < step["serialEnd"]]
                stages[step["stage"]] = check(selected, step["stage"] != "mapped", 10)
            assert len({tuple(s["identity"]) for s in stages.values()}) == 1
            row.update(result="pass", stages=stages)
        except (AssertionError, KeyError, ValueError) as e:
            row["error"] = str(e)
        result["ept"].append(row)
    starts = list(re.finditer(r"^policy-inplace-run-(\d+)\r?$", serial, re.M))
    try:
        result["inplaceContinuity"] = continuity(args.directory)
    except (AssertionError, KeyError, ValueError, OSError) as error:
        result["inplaceContinuity"] = dict(result="incomplete", error=str(error))
    for start in starts:
        run = start[1]
        row = dict(runId="inplace-" + run, result="incomplete")
        try:
            assert result["inplaceContinuity"]["result"] == "pass", "comparator continuity not verified"
            finish = re.search(rf"^policy-inplace-end-{run}\r?$", serial[start.end():], re.M)
            assert finish, "missing comparator end marker"
            end = start.end() + finish.start()
            segment = serial[start.end():end]
            markers = [re.search(rf"^policy-inplace-{stage}\r?$", segment, re.M) for stage in ("before", "write", "restore")]
            assert all(markers), "missing comparator stage"
            offsets = [start.end() + m.start() for m in markers] + [end]
            stages = {}
            for i, stage in enumerate(("before", "mapped", "restored")):
                selected = [s for s in samples if offsets[i] <= s["offset"] < offsets[i + 1]]
                # Exclude the first 0.5 s of observations after the marker.
                # Markers precede writes, so this is a settling window, not an
                # atomicity claim or a timestamp for the actual write itself.
                selected_count = len(selected)
                if stage != "before" and selected:
                    threshold = selected[0]["uptime"] + .5
                    selected = [s for s in selected if s["uptime"] >= threshold]
                stages[stage] = check(selected, stage != "mapped", 5)
                stages[stage]["settlingWindowExcluded"] = selected_count - len(selected)
            assert len({tuple(s["identity"]) for s in stages.values()}) == 1
            row.update(result="pass", stages=stages)
        except (AssertionError, KeyError, ValueError) as e:
            row["error"] = str(e)
        result["inplace"].append(row)
    output = args.directory / "derived"
    output.mkdir(exist_ok=True)
    (output / "policy-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
