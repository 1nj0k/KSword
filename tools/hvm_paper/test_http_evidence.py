"""Reject plausible-looking HTTP logs with stale or changed identity evidence."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class HttpEvidenceTests(unittest.TestCase):
    def fixture(self):
        boot = "3072455b-b9e3-4b83-a8cd-72483189b785"
        state = {"bootUtc": "2026-09-15T23:02:45Z", "vmx": [{"pid": 42, "createdUtc": "same"}],
                 "vmxConfigSha256": "same",
                 "hvm": {"exitCode": 0, "parsed": {"residentProcessorCount": 4, "stateNames": []}},
                 "page": {"exitCode": 0, "parsed": {"active": 0, "retired": 0}},
                 "metrics": {"exitCode": 0, "parsed": {"ruleAllocations": 3, "ruleFrees": 3,
                                                      "replacementAllocations": 3, "replacementFrees": 3}}}
        r = {"schemaVersion": 2, "status": "recorded_requires_analysis", "runId": "test", "gpa": "0x2000",
             "holderPid": 33, "guestIdentity": f"paper-http-identity {boot} 0-1", "expectedResidentProcessors": 4,
             "before": copy.deepcopy(state), "mapped": copy.deepcopy(state), "after": copy.deepcopy(state),
             "map": {"exitCode": 0, "parsed": {"ownerProcessId": 42}}, "remove": {"exitCode": 0}, "steps": []}
        r["mapped"]["page"]["parsed"].update(active=1, composedCount=2,
                                            originalPhysicalPage="0x1000", shadowPhysicalPage="0x3000")
        text = ""
        sequence = 0
        for stage in ("before", "mapped", "restored"):
            start = len(text)
            digest = "620f0b67a91f7f74151bc5be745b7110" if stage == "mapped" else "53398dd1f3a4c282ace3b660acd92ff7"
            for _ in range(10):
                uptime = sequence + 100
                text += f"paper-http-sample-v2 {sequence} {uptime} 0 4096 {digest} 22 55 1 {boot}\r\n"
                text += json.dumps({"kind": "application-page", "pid": 33, "gpa": "0x2000",
                                    "samePfn": True, "uptime": uptime-1}, separators=(",", ":")) + "\r\n"
                sequence += 1
            r["steps"].append({"stage": stage, "serialOffset": start, "serialEnd": len(text)})
        return r, text

    def analyze(self, record, serial, provenance=None):
        with tempfile.TemporaryDirectory(prefix="http-evidence-") as directory:
            root = Path(directory)
            (root / "serial.txt").write_bytes(serial.encode())
            (root / "http-page-test.json").write_text(json.dumps(record))
            if provenance:
                (root / "measured-policy-sources.json").write_text(json.dumps(provenance))
            subprocess.run([sys.executable, str(Path(__file__).with_name("analyze_http_page.py")), str(root)],
                           check=True, capture_output=True)
            return json.loads((root / "derived/http-page-summary.json").read_text())["runs"][0]

    def test_complete_and_fresh(self):
        self.assertEqual(self.analyze(*self.fixture())["result"], "pass")

    def test_constant_alive_is_not_independent_liveness(self):
        row = self.analyze(*self.fixture(), {"holderLiveness": "constant-one; fresh PFN samples only"})
        self.assertEqual(row["result"], "pass")
        self.assertFalse(row["independentHolderLiveness"])

    def test_changed_creation_time(self):
        r, serial = self.fixture()
        row = self.analyze(r, serial.replace(" 22 55 1 ", " 22 56 1 ", 1))
        self.assertEqual(row["error"], "HTTP server was restarted")

    def test_dead_holder(self):
        r, serial = self.fixture()
        row = self.analyze(r, serial.replace(" 22 55 1 ", " 22 55 0 ", 1))
        self.assertEqual(row["error"], "holder exited or guest rebooted")

    def test_stale_pfn(self):
        r, serial = self.fixture()
        row = self.analyze(r, serial.replace('"uptime":99', '"uptime":10', 1))
        self.assertEqual(row["error"], "stale PFN observation")

    def test_guest_boot_change(self):
        r, serial = self.fixture()
        row = self.analyze(r, serial.replace("3072455b-", "4072455b-", 1))
        self.assertEqual(row["error"], "holder exited or guest rebooted")

    def test_missing_stage(self):
        r, serial = self.fixture()
        r["steps"].pop()
        self.assertEqual(self.analyze(r, serial)["error"], "missing/reordered stages")


if __name__ == "__main__":
    unittest.main()
