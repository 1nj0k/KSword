"""Recheck teardown identity, owner revocation and reclamation from raw fields."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    root = parser.parse_args().root
    rows = []
    for path in sorted(root.rglob("vmware-teardown-*.json")):
        if "derived" in path.relative_to(root).parts:
            continue
        record = json.loads(path.read_text(encoding="utf-8-sig"))
        row = {"source": path.relative_to(root).as_posix(), "result": "incomplete"}
        try:
            assert record["status"] == "passed", record.get("error", record["status"])
            before, after = record["before"], record["after"]
            expected = record.get("expectedResidentProcessors", 2)
            mapping = record.get("expectedActiveMapping", 0)
            assert before["hvm"]["residentProcessorCount"] == after["hvm"]["residentProcessorCount"] == expected, "residency changed"
            assert before["bootUtc"] == after["bootUtc"], "Windows reboot"
            assert before["driverSha256"] == after["driverSha256"], "driver identity drift"
            assert len(before["vmx"]) == 1 and not after["vmx"], "VMM did not exit"
            assert before["page"]["active"] == mapping, "mapping precondition changed"
            assert record["stop"]["completed"] and record["stop"]["exitCode"] == 0, "vmrun failed"
            assert not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(after["hvm"]["stateNames"]), "resident fault"
            assert not record["hyperVResetEvents"], "Hyper-V reset event observed"
            if mapping:
                page = after["page"]
                assert page["active"] == 0 and page["retired"] == page["ownerExited"] == 1, "owner lease not revoked"
                assert page["ownerProcessId"] == before["vmx"][0]["pid"], "owner identity mismatch"
                reclaimed = record["reclamation"]
                assert reclaimed["exitCode"] == 0 and reclaimed["remove"]["active"] == reclaimed["remove"]["retired"] == 0, "reclamation failed"
                metrics = reclaimed["metrics"]
                assert metrics["ruleAllocations"] == metrics["ruleFrees"] and metrics["replacementAllocations"] == metrics["replacementFrees"], "outstanding page-control allocations"
            row.update(result="pass", residentCpus=expected, activeMappingAtExit=mapping,
                       driverSha256=after["driverSha256"], bootUtc=after["bootUtc"])
        except (AssertionError, KeyError, TypeError, ValueError) as error:
            row["error"] = str(error)
        rows.append(row)
    if not rows:
        raise ValueError("missing teardown records")
    derived = root / "derived"
    derived.mkdir(exist_ok=True)
    result = {"runs": rows, "scope": "VMM process exit and explicit all-CPU reclamation. "
              "Does not test guest reboot/snapshot reuse within a surviving VMM process or stopping residency with a live VMM."}
    (derived / "lifecycle.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
