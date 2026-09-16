"""Check the actual CLI parser and GUI form report without mutating a driver."""
import argparse
import json
from pathlib import Path
import subprocess


def invoke(exe, *args):
    return subprocess.run([str(exe), *args], capture_output=True, encoding="utf-8", timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, default=Path("tools/hvm_ctl/hvm_ctl.exe"))
    parser.add_argument("--gui-report", type=Path)
    args = parser.parse_args()
    result = invoke(args.cli, "--json", "commands")
    assert result.returncode == 0, result.stderr
    catalog = json.loads(result.stdout)["commands"]
    by_name = {c["name"]: c for c in catalog}
    assert len(by_name) == len(catalog), "Duplicate command name"
    for locale in ("zh-CN", "en-US"):
        pack = json.loads(Path(f"Ksword5.1/Ksword5.1/languages/{locale}.json").read_text(encoding="utf-8-sig"))["source_translations"]
        for command in catalog:
            for source in [command["title"], command["group"], command["description"],
                           *(a["name"] for a in command["arguments"])]:
                assert source in pack and pack[source], (locale, source)

    # Contract regressions: --json must not shift CPU/MSR arguments; hex values
    # must not become decimal, and 64-bit addresses must not be truncated.
    cases = [
        (["gdt-dump", "1"], [1]),
        (["msr-log", "1b"], [0x1B]),
        (["msr-log"], [0x10]),
        (["ept-leaf", "123456789ABC"], [0x123456789ABC]),
        (["events", "18446744073709551615"], [2**64 - 1, 64]),
        (["events", "123", "7"], [123, 7]),
        (["resident-vmreadbench"], [512]),
        (["soak"], [1000]),
        (["nested-page-map", "1234501e", "7000000", "d1"], [0x1234501E, 0x7000000, 0xD1, 0]),
        (["nested-page-map", "1234501e", "7000000", "d1", "1234"], [0x1234501E, 0x7000000, 0xD1, 1234]),
        (["nested-page-map-test", "1234501e", "7000000", "d1", "4"], [0x1234501E, 0x7000000, 0xD1, 4]),
        (["inject-test", "42", "10000", "20000"], [42, 0x10000, 0x20000, 0x4B535744]),
        (["inject-dll", "42", "10000", "0", "C:/HVM test/中文 测试.dll"], [42, 0x10000, 0, 0]),
    ]
    for command, expected in cases:
        parsed = []
        for prefix in (["--validate"], ["--json", "--validate"]):
            result = invoke(args.cli, *prefix, *command)
            assert result.returncode == 0, (command, result.stderr)
            value = json.loads(result.stdout)
            assert [int(v, 16) for v in value["values"]] == expected, value
            if command[0] == "inject-dll":
                assert value["arguments"][3] == command[4], value
            if "controlRequest" in value:
                wire = value["controlRequest"]
                assert wire["command"] == value["operation"] and wire["flags"] == value["flags"]
                assert wire["expectedGeneration"] == 0
                assert wire["soakMilliseconds"] == (expected[0] if command[0] == "soak" else 0)
                assert wire["vmreadBenchIterations"] == (expected[0] if command[0] == "resident-vmreadbench" else 0)
            parsed.append(value)
        assert parsed[0] == parsed[1], command

    invalid = [
        ["gdt-dump", "-1"], ["gdt-dump", "4294967295"], ["gdt-dump", "1junk"], ["status", "1"],
        ["msr-log", "100000000"], ["msr-log", "-1"], ["msr-log", " 1b"],
        ["events", "18446744073709551616"], ["soak", "4294967296"],
        ["nested-page-map", "1234501e", "7000001", "d1"],
        ["nested-page-map", "1234501e", "7000000", "100"],
        ["nested-page-map", "10000000000000000", "7000000", "00"],
        ["nested-page-map", "0x", "7000000", "00"],
        ["nested-page-map", "1234501e", "7000000"],
        ["nested-page-map-test", "1234501e", "7000000", "d1", "0"],
        ["nested-page-map-test", "1234501e", "7000000", "d1", "5"],
        ["inject-dll", "42", "10000", "0"],
        ["proc-freeze", "42"], ["proc-terminate", "4294967296", "10000"],
        ["unknown-command"],
    ]
    for command in invalid:
        result = invoke(args.cli, "--json", "--validate", *command)
        assert result.returncode == 2, (command, result.returncode, result.stdout, result.stderr)
        assert "device-open-failed" not in result.stdout, command

    if args.gui_report:
        report = json.loads(args.gui_report.read_text(encoding="utf-8"))
        assert report["passed"] is True, report
        assert report["embedded"] is True and report["hostClass"] == "MainWindow", report
        rows = report["rows"]
        assert len(rows) == len(catalog), (len(rows), len(catalog))
        assert {r["name"] for r in rows} == set(by_name)
        for row in rows:
            assert row["ok"] is True, row
            result = invoke(args.cli, "--json", "--validate", row["name"], *row["arguments"])
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout) == row["result"], row["name"]
        print(f"GUI_FORM_COMMAND_PARITY=PASS ({len(rows)} commands)")
    print(f"CATALOG_TRANSLATIONS=PASS ({len(catalog)} commands, 2 languages)")
    print(f"ARGUMENT_CONTRACTS=PASS ({len(cases) * 2} valid, {len(invalid)} refused)")


if __name__ == "__main__":
    main()
