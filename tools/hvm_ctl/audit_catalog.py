"""Build gate for the C command catalog consumed by the Qt form and CLI."""
import json
from pathlib import Path
import re


def main():
    root = Path(__file__).resolve().parents[2]
    client = root / "Ksword5.1/Ksword5.1/ArkDriverClient"
    source = (client / "HvmCommandCatalog.c").read_text(encoding="utf-8-sig")
    catalog = source.split("g_commands[] = {", 1)[1].split("\n};", 1)[0]
    literal = r'"(?:\\.|[^"\\])*"'
    records = re.findall(r"^\s*\{\s*(" + literal + r"),\s*(" + literal + r"),\s*(" +
                         literal + r"),\s*(" + literal + r"),\s*(Hvm\w+),", catalog, re.M)
    assert records, "No command definitions"
    names = [json.loads(r[0]) for r in records]
    assert len(set(names)) == len(names), "Duplicate command name"
    strings = {json.loads(s) for r in records for s in r[1:4]}
    strings.update(json.loads(s) for s in re.findall(r"\{\s*(" + literal + r"),\s*Hvm\w+,", catalog))
    for language in ("zh-CN", "en-US"):
        pack = json.loads((client.parent / "languages" / f"{language}.json").read_text(encoding="utf-8-sig"))
        missing = sorted(s for s in strings if not pack["source_translations"].get(s))
        assert not missing, (language, missing)
    engine = (client / "HvmCommandEngine.c").read_text(encoding="utf-8-sig")
    handlers = set(re.findall(r"case (Hvm\w+):", engine))
    handlers.update(re.findall(r"spec->handler == (Hvm\w+)", engine))
    assert not ({r[4] for r in records} - handlers), "Command has no dispatch handler"
    print(f"HVM_CATALOG_AUDIT=PASS commands={len(records)} translations={len(strings)}")


if __name__ == "__main__":
    main()
