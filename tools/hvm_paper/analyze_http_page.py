"""Require application output, process/boot/PFN continuity, and clean reclamation."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory
    # PowerShell StreamReader offsets count the preserved CR/LF text.
    with (root / "serial.txt").open(encoding="utf-8-sig", newline="") as stream:
        serial = stream.read()
    original = bytearray(b" " * 4096)
    prefix = b'{"product":"research-fixture","price":1250,"padding":"'
    original[:len(prefix)] = prefix
    original[4093:] = b'"}\n'
    hashes = {"before": hashlib.md5(original).hexdigest(),
              "mapped": hashlib.md5(bytes(4096)).hexdigest(),
              "restored": hashlib.md5(original).hexdigest()}
    rows = []
    provenance = root / "measured-policy-sources.json"
    holder_liveness = "kill-0-per-request"
    if provenance.exists():
        holder_liveness = json.loads(provenance.read_text(encoding="utf-8-sig"))["holderLiveness"]
    independent_holder_liveness = holder_liveness == "kill-0-per-request"
    for path in sorted(root.glob("http-page-*.json")):
        r = json.loads(path.read_text(encoding="utf-8-sig"))
        row = {"runId": r["runId"], "result": "incomplete", "gpa": r["gpa"]}
        try:
            assert r["status"] == "recorded_requires_analysis", r.get("error", r["status"])
            assert [s["stage"] for s in r["steps"]] == ["before", "mapped", "restored"], "missing/reordered stages"
            assert r["before"]["bootUtc"] == r["after"]["bootUtc"], "Windows reboot"
            assert r["before"]["vmx"] == r["after"]["vmx"], "VMX identity drift"
            assert r["before"]["vmxConfigSha256"] == r["after"]["vmxConfigSha256"], "VM configuration drift"
            assert r["guestIdentity"].endswith(" 0-1"), "not two guest CPUs"
            live_identity = r.get("schemaVersion", 1) >= 2
            boot = r["guestIdentity"].split()[1]
            identities, counters, uptimes, holder_ages = set(), {}, [], []
            previous_end = 0
            for step in r["steps"]:
                assert previous_end <= step["serialOffset"] < step["serialEnd"] <= len(serial), "invalid/overlapping serial offsets"
                previous_end = step["serialEnd"]
                tail = serial[step["serialOffset"]:step["serialEnd"]]
                prefix = "paper-http-sample-v2" if live_identity else "paper-http-sample"
                pattern = prefix + r" (\d+) ([\d.]+) (\d+) (\d+) ([a-f0-9]{32}) (\d+) (\d+)"
                if live_identity:
                    pattern += r" (\d+) ([a-f0-9-]{36})"
                pattern += r'\r?\n(\{"kind":"application-page"[^\r\n]+\})\r?\n'
                samples = list(re.finditer(pattern, tail))
                assert len(samples) >= 10, "missing HTTP responses"
                for sample in samples:
                    _, uptime, rc, size, digest, pid, ticks = sample.groups()[:7]
                    assert rc == "0" and size == "4096", "HTTP transport/length failure"
                    assert digest == hashes[step["stage"]], "wrong served page contents"
                    assert int(ticks) > 0, "HTTP server process no longer exists"
                    identities.add((pid, ticks))
                    uptime = float(uptime)
                    assert not uptimes or uptime > uptimes[-1], "guest uptime stopped or regressed"
                    uptimes.append(uptime)
                    if live_identity:
                        alive, sample_boot = sample.groups()[7:9]
                        assert alive == "1" and sample_boot == boot, "holder exited or guest rebooted"
                    holder = json.loads(sample.groups()[-1])
                    assert holder["kind"] == "application-page" and holder["samePfn"] and holder["gpa"] == r["gpa"] and holder["pid"] == r["holderPid"], "page migration or holder drift"
                    age = uptime - holder["uptime"]
                    assert -0.1 <= age <= 5, "stale PFN observation"
                    holder_ages.append(age)
                holders = [json.loads(m) for m in re.findall(r'(?m)^\{"kind":"application-page"[^\r\n]+', tail)]
                assert holders and all(h["samePfn"] and h["gpa"] == r["gpa"] and h["pid"] == r["holderPid"] for h in holders), "page migration or holder drift"
                assert len({h["uptime"] for h in holders}) >= 2, "PFN observer did not advance"
                counters[step["stage"]] = len(samples)
            assert len(identities) == 1, "HTTP server was restarted"
            for side in ("before", "mapped", "after"):
                state = r[side]
                assert state["bootUtc"] == r["before"]["bootUtc"], "Windows reboot"
                assert state["vmx"] == r["before"]["vmx"], "VMX identity drift"
                assert state["vmxConfigSha256"] == r["before"]["vmxConfigSha256"], "VM configuration drift"
                assert state["hvm"]["exitCode"] == state["page"]["exitCode"] == state["metrics"]["exitCode"] == 0, "state query failed"
                assert state["hvm"]["parsed"]["residentProcessorCount"] == r["expectedResidentProcessors"], "residency changed"
                assert not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(state["hvm"]["parsed"]["stateNames"]), "resident fault"
            assert r["map"]["exitCode"] == r["remove"]["exitCode"] == 0, "page operation failed"
            assert r["map"]["parsed"]["ownerProcessId"] == r["before"]["vmx"][0]["pid"]
            mapped = r["mapped"]["page"]["parsed"]
            assert mapped["active"] == 1 and int(mapped["composedCount"]) > 0, "no observed replacement composition"
            original_pa, shadow_pa = int(mapped["originalPhysicalPage"], 16), int(mapped["shadowPhysicalPage"], 16)
            assert original_pa and shadow_pa and original_pa != shadow_pa, "missing/distinct backing evidence"
            page = r["after"]["page"]["parsed"]
            assert page["active"] == page["retired"] == 0, "mapping not reclaimed"
            metrics = r["after"]["metrics"]["parsed"]
            assert metrics["ruleAllocations"] == metrics["ruleFrees"] and metrics["replacementAllocations"] == metrics["replacementFrees"], "outstanding page-control allocation"
            row.update(result="pass" if live_identity else "effect_only_identity_snapshot",
                       responses=counters, httpdIdentity=list(identities)[0], expectedMD5=hashes,
                       guestUptimeBegin=uptimes[0], guestUptimeEnd=uptimes[-1], maxPfnSampleAgeSeconds=max(holder_ages),
                       identityRereadPerRequest=live_identity,
                       independentHolderLiveness=independent_holder_liveness and live_identity,
                       holderLivenessEvidence=holder_liveness if live_identity else "startup-only identity",
                       originalBacking=mapped["originalPhysicalPage"], replacementBacking=mapped["shadowPhysicalPage"],
                       observedCompositions=mapped["composedCount"])
        except (AssertionError, KeyError, TypeError, ValueError, IndexError) as error:
            row["error"] = str(error)
        rows.append(row)
    if not rows:
        raise ValueError("missing application runs")
    derived = root / "derived"
    derived.mkdir(exist_ok=True)
    result = {"runs": rows, "scope": "BusyBox httpd serves a controlled resident file-cache page. "
              "Zero-filled replacement is a controlled data fault; restoration recovers its original HTTP body. "
              "Schema 1 has a server-identity startup snapshot only; schema 2 re-reads identity and boot ID per response. "
              "The follow-up policy collector's alive field was constant one: its holder evidence is advancing, fresh PFN samples, not an independent kill-0 check. "
              "Root pagemap/mlock instrumentation selects the GPA. This is not arbitrary application atomicity, "
              "a guest-agent-free claim, or a production fault-tolerance benchmark."}
    (derived / "http-page-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
