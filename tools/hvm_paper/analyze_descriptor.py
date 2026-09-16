"""Recompute descriptor and VMware regression results from individual raw runs."""

import argparse
import json
import re
from collections import Counter
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def vmware_identity(snapshot):
    return [
        (p["pid"], p["createdUtc"])
        for p in snapshot["processes"]
        if p["name"] == "vmware-vmx.exe"
    ]


def analyze(directory):
    expected = {
        "launch-test-guest": (2, 2),
        "resident-nested-hidehv": (0, None),
        "nested-probe-all": (4, 3),
        "nested-selfvirt-all": (4, 3),
        "stop": (4, 1),
    }
    runs, boots, hashes = [], set(), set()
    tables, limits, cpus = Counter(), Counter(), Counter()
    for path in sorted((directory / "verified").glob("descriptor-*.json")):
        data = read(path)
        command = data["command"]
        count, stage = expected[command]
        require(data["status"] == "passed", f"{path.name}: collector failed")
        require(data["result"]["completed"] and data["result"]["exitCode"] == 0,
                f"{path.name}: CLI did not complete successfully")
        require(data["before"]["bootUtc"] == data["after"]["bootUtc"],
                f"{path.name}: Windows restarted")
        require(data["before"]["driverSha256"] == data["after"]["driverSha256"],
                f"{path.name}: driver file identity changed")
        require(data["before"]["qpcFrequency"] > 0, f"{path.name}: no QPC frequency")
        rows = data["descriptorReadbacks"]
        require(len(rows) == count, f"{path.name}: wrong number of table observations")
        identities = set()
        for row in rows:
            tag = row["ruleId"]
            require(tag in (0x47445452, 0x49445452), f"{path.name}: unknown table tag")
            name = "GDTR" if tag == 0x47445452 else "IDTR"
            packed = int(row["qualification"], 16)
            require(row["exitReason"] == stage, f"{path.name}: wrong continuation stage")
            require(row["status"] == "0x00000000" and row["timestampQpc"] > 0,
                    f"{path.name}: failed or untimestamped hardware readback")
            require(row["sequence"] > data["before"]["hvm"]["publishedEventCount"],
                    f"{path.name}: stale event")
            require(row["guestPhysicalAddress"] == row["guestLinearAddress"] and
                    (packed & 65535) == ((packed >> 16) & 65535),
                    f"{path.name}: hardware base or limit mismatch")
            identity = (row["processorGroup"], row["processor"])
            identities.add((*identity, tag))
            tables[name] += 1
            cpus[f"{identity[0]}:{identity[1]}:{name}"] += 1
            limits[f"{name}:0x{row['access']:04X}->0x{packed & 65535:04X}"] += 1
        if count == 4:
            require(len(identities) == 4 and len({x[:2] for x in identities}) == 2,
                    f"{path.name}: missing processor attribution")
        boots.add(data["before"]["bootUtc"])
        hashes.add(data["before"]["driverSha256"])
        runs.append({"file": str(path.relative_to(directory)), "command": command,
                     "readbacks": len(rows), "result": "passed"})
    require(runs, "No verified descriptor runs were found")

    teardown = []
    for path in sorted((directory / "teardown").glob("vmware-teardown-*.json")):
        data = read(path)
        passed = (data["status"] == "passed" and
                  data["stop"]["completed"] and data["stop"]["exitCode"] == 0 and
                  data["before"]["bootUtc"] == data["after"]["bootUtc"] and
                  len(data["before"]["vmx"]) == 1 and len(data["after"]["vmx"]) == 0 and
                  data["before"]["hvm"]["residentProcessorCount"] == 2 and
                  data["after"]["hvm"]["residentProcessorCount"] == 2 and
                  not data["hyperVResetEvents"])
        teardown.append({"file": str(path.relative_to(directory)),
                         "result": "passed" if passed else "failed",
                         "bootUtc": data["before"]["bootUtc"]})

    isolation = []
    for path in sorted(directory.rglob("write-isolation-*.json")):
        data = read(path)
        require(data["status"] == "recorded_requires_analysis", f"{path.name}: collection failed")
        before, after = data["before"], data["after"]
        serial = after["guestSerial"].replace("\r", "")
        sections, values = {}, []
        for label in ("before", "mapped", "written", "restored"):
            section = serial.split("paper-isolation-" + label + "\n", 1)[1]
            section = section.split("paper-isolation-", 1)[0]
            sections[label] = section
            values.append(re.findall(r"^\s*([0-9a-f]{8})\s*$", section, re.M)[0])
        boot_ids = [re.findall(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}",
                              sections[label])[0] for label in ("before", "restored")]
        page = after["hvm"]["nested-page-query"]["parsed"]
        passed = (values == ["a5a5a5a5", "d1d1d1d1", "b2b2b2b2", "a5a5a5a5"] and
                  boot_ids[0] == boot_ids[1] and
                  before["os"]["bootUtc"] == after["os"]["bootUtc"] and
                  vmware_identity(before) == vmware_identity(after) and
                  len(vmware_identity(before)) == 1 and
                  page["active"] == 0 and page["retired"] == 0 and
                  page["residentProcessors"] == 2)
        isolation.append({"file": str(path.relative_to(directory)), "values": values,
                          "guestBootId": boot_ids[0], "result": "passed" if passed else "failed"})

    gdt_queries = []
    for path in sorted((directory / "verified").glob("gdt-query-*.json")):
        data = read(path)
        if data["status"] == "passed":
            table = json.loads(data["result"]["raw"])
            require(data["result"]["exitCode"] == 0 and table["source"] == "R0" and
                    table["cpu"] == data["cpu"] and
                    table["bytes"] == int(table["limit"], 16) + 1 and
                    len(table["data"]) == table["bytes"] * 2 and
                    re.fullmatch(r"[0-9A-F]+", table["data"]),
                    f"{path.name}: incomplete R0 GDT snapshot")
        else:
            require(data["status"] == "clean-reject" and data["result"]["exitCode"] != 0,
                    f"{path.name}: expected invalid CPU rejection")
        gdt_queries.append({"file": str(path.relative_to(directory)), "cpu": data["cpu"],
                            "result": data["status"],
                            "ctlSha256": data["result"]["state"]["ctlSha256"]})

    observations = []
    for path in sorted((directory / "normal-init").glob("stability-*.jsonl")):
        samples = [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line]
        identities, observed_boots = set(), set()
        healthy = True
        for sample in samples:
            if sample["status"] != "ok":
                healthy = False
                continue
            state = sample["windows1"]
            observed_boots.add(state["bootUtc"])
            vmware_count = 0
            for process in state["processes"]:
                if process["name"] == "vmware-vmx":
                    vmware_count += 1
                    identities.add((process["pid"], process["createdUtc"]))
            hvm = json.loads(state["hvmStatusRaw"])
            healthy &= (vmware_count == 1 and state["hvmStatusExit"] == 0 and
                        state["pageStatusExit"] == 0 and hvm["residentProcessorCount"] == 2 and
                        not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(hvm["stateNames"]))
        observations.append({"file": str(path.relative_to(directory)), "samples": len(samples),
                             "lastElapsedSeconds": samples[-1]["hostElapsedSeconds"],
                             "observerAndHvmHealthy": healthy,
                             "oneWindowsBoot": len(observed_boots) == 1,
                             "oneVmwareProcessIdentity": len(identities) == 1,
                             "condition": "Observation includes normal guest boot and shell instrumentation."})

    normal_init = None
    capture = directory / "normal-init" / "shell-capture.json"
    if capture.exists():
        data = read(capture)
        tail = data["guestSerial"].replace("\r", "").split("normal-init-shell\n", 1)[1]
        lines = [line.strip() for line in tail.splitlines() if line.strip()]
        require(lines[3] == "init" and "normal-init-ready" in lines and
                not any(option in lines[2].split() for option in ("nosmp", "hpet=disable", "rdinit=/bin/sh")),
                "Normal init marker or intended command line missing")
        normal_init = {"file": str(capture.relative_to(directory)), "guestBootId": lines[0],
                       "uptime": lines[1], "commandLine": lines[2], "pid1": lines[3],
                       "clockevent": lines[4], "result": "normal-init-shell-reached",
                       "scope": "One VMware vCPU; text-mode boot with console/video and reserved-page options."}

    return {"schemaVersion": 1, "descriptorRuns": runs,
            "descriptorCommands": dict(Counter(r["command"] for r in runs)),
            "tableReadbacks": dict(tables), "cpuReadbacks": dict(cpus),
            "limits": dict(limits), "windowsBoots": sorted(boots),
            "driverSha256": sorted(hashes), "vmwareTeardown": teardown,
            "writeIsolation": isolation, "gdtQueries": gdt_queries,
            "normalInit": normal_init, "observations": observations,
            "preliminaryRunsExcluded": len(list(directory.glob("descriptor-*.json"))),
            "limitsOfEvidence": ["Preliminary records lack complete CPU/time metadata.",
                                 "No claim of multicore guest boot, long-term stability, or total allocation accounting.",
                                 "Teardown success is not a causal attribution to this patch alone."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    (args.directory / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"descriptorRuns": len(result["descriptorRuns"]),
                      "readbacks": sum(result["tableReadbacks"].values()),
                      "vmwareTeardown": [r["result"] for r in result["vmwareTeardown"]],
                      "writeIsolation": [r["result"] for r in result["writeIsolation"]]}))
