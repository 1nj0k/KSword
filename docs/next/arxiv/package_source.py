"""Pack only the manuscript's actual local TeX dependencies for arXiv."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    inputs = {}

    def visit(name):
        path = HERE / name
        if not path.suffix:
            path = path.with_suffix(".tex")
        path = path.resolve()
        relative = path.relative_to(HERE).as_posix()
        if relative in inputs:
            return
        if path.suffix not in (".tex", ".bbl"):
            raise ValueError(f"Unexpected manuscript dependency: {relative}")
        raw = path.read_bytes()
        source = raw.decode("ascii")
        if any(s in source for s in [r"\write18", r"\includegraphics", r"\today"]):
            raise ValueError(f"Review external/dynamic dependency in {relative}")
        inputs[relative] = raw
        for child in re.findall(r"\\input\{([^}]+)\}", source):
            visit(child)

    visit("main.tex")
    all_source = "\n".join(b.decode("ascii") for b in inputs.values())
    cited = {key for group in re.findall(r"\\cite\{([^}]+)\}", all_source) for key in group.split(",")}
    bibliography = set(re.findall(r"\\bibitem\{([^}]+)\}", all_source))
    labels_list = re.findall(r"\\label\{([^}]+)\}", all_source)
    labels = set(labels_list)
    refs = set(re.findall(r"\\ref\{([^}]+)\}", all_source))
    assert cited <= bibliography, sorted(cited - bibliography)
    assert refs <= labels, sorted(refs - labels)
    assert len(labels) == len(labels_list), "Duplicate labels"
    assert (HERE / "abstract.txt").read_text().strip() == (HERE / "submission-abstract.md").read_text().strip()
    archive = destination / "ksword-arxiv-source-v1.zip"
    manifest = {"entryPoint": "main.tex", "files": [], "citations": len(cited),
                "labels": len(labels), "missingDependencies": [],
                "scope": "Local dependency and ZIP validation; not arXiv server processing."}
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, raw in sorted(inputs.items()):
            entry = zipfile.ZipInfo(name, date_time=(2026, 9, 16, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            bundle.writestr(entry, raw)
            manifest["files"].append({"path": name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    with zipfile.ZipFile(archive) as bundle:
        assert bundle.testzip() is None
        assert sorted(bundle.namelist()) == sorted(inputs)
    manifest["archiveSha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    manifest["archiveBytes"] = archive.stat().st_size
    (destination / "arxiv-source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Source package: {len(inputs)} files, {archive.stat().st_size} bytes, {len(cited)} resolved citations")


if __name__ == "__main__":
    main()
