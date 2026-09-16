"""Regenerate build-validation.json from the artifacts actually produced.

Why this exists: build-validation.json used to be maintained by hand. It drifted
silently -- it kept asserting a page count, a PDF hash and an abstract length
that belonged to an earlier build, and its visual-review sentence went on
vouching for a PDF that no longer existed.

Two rules keep that from recurring:

* Everything measurable is measured here, every build.
* Everything that needs a human to look at something is listed in HUMAN_FIELDS.
  Those values are carried forward only while the manuscript content is
  unchanged. As soon as it differs, each one is reset to a sentinel saying it has
  not been done for this build. A stale human assertion is worse than an absent
  one, because it reads as evidence.

The carry-forward key is sourceArchiveSha256, not pdfSha256. xdvipdfmx stamps
/CreationDate with the wall clock, so two builds of identical sources produce
different PDF bytes; keying on that would reset every human field on every build
and the sentinel would never lift. package_source.py pins the ZIP entry
timestamps, so the archive hash changes only when the sources do -- which is also
the thing a reviewer actually looked at.

Run it after Build-Preprint.ps1 has produced the PDF and the source package;
the build script invokes it as its last step.
"""

import argparse
import hashlib
import json
import pathlib
import re
import zipfile

from pypdf import PdfReader

HERE = pathlib.Path(__file__).resolve().parent

# field -> sentinel written when the PDF changed and the assertion must be redone
HUMAN_FIELDS = {
    "visualReview": "not performed for this build",
    "arxivServerProcessing": "not run",
    "authorScientificReview": "pending",
    "runtimeExperiments": "not rerun for this build",
    "environmentNote": "not recorded for this build",
}


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def count_boxes(log_text, kind):
    return len(re.findall(rf"^{kind}\b", log_text, flags=re.MULTILINE))


def font_embedding(reader):
    """Return (embedded count, sorted names lacking an embedded file)."""
    embedded, missing = set(), set()
    for page in reader.pages:
        resources = page.get("/Resources")
        fonts = resources.get_object().get("/Font") if resources else None
        if not fonts:
            continue
        for _, ref in fonts.items():
            font = ref.get_object()
            descendants = font.get("/DescendantFonts")
            targets = [d.get_object() for d in descendants] if descendants else [font]
            for target in targets:
                name = str(target.get("/BaseFont", "?"))
                descriptor = target.get("/FontDescriptor")
                descriptor = descriptor.get_object() if descriptor else None
                keys = ("/FontFile", "/FontFile2", "/FontFile3")
                if descriptor and any(k in descriptor for k in keys):
                    embedded.add(name)
                else:
                    missing.add(name)
    return len(embedded), sorted(missing)


def abstract_present(reader, abstract):
    """The abstract is complete if page 1 carries it apart from typography.

    Extracted text differs from the source in ligatures (fi, ff) and in line-break
    hyphenation, so an exact substring test reports a false negative. Compare word
    sequences instead and require that the abstract survive as one aligned run with
    only such single-word substitutions.
    """
    import difflib

    words = re.sub(r"\s+", " ", abstract).strip().split(" ")
    page = re.sub(r"\s+", " ", reader.pages[0].extract_text() or "").strip().split(" ")
    matcher = difflib.SequenceMatcher(None, words, page, autojunk=False)
    differing = 0
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        # Leading title/author block and the trailing section body are expected.
        if i1 == i2 and (i1 == 0 or i1 == len(words)):
            continue
        if tag == "replace" and (i2 - i1) == 1:
            differing += 1
            continue
        return False, differing
    return True, differing


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=HERE.parents[2] / "output/pdf")
    parser.add_argument("--pdf-name", default="ksword-live-interposition-v1.pdf")
    parser.add_argument("--archive-name", default="ksword-arxiv-source-v1.zip")
    parser.add_argument("--log-name", default="main.log")
    arguments = parser.parse_args()

    pdf_path = arguments.output_dir / arguments.pdf_name
    archive_path = arguments.output_dir / arguments.archive_name
    log_path = arguments.output_dir / arguments.log_name
    for path in (pdf_path, archive_path, log_path):
        if not path.is_file():
            raise SystemExit(f"missing build artifact: {path}")

    reader = PdfReader(str(pdf_path))
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    abstract = (HERE / "abstract.txt").read_text(encoding="ascii").strip()
    metadata = json.loads((HERE / "metadata.json").read_text(encoding="utf-8"))

    tex_sources = [HERE / "main.tex"] + sorted((HERE / "sections").glob("*.tex"))
    tex = "\n".join(p.read_text(encoding="utf-8") for p in tex_sources)

    embedded_count, unembedded = font_embedding(reader)
    complete, ligature_substitutions = abstract_present(reader, abstract)
    expected_author = " ".join(
        v for v in (metadata["authors"][0]["givenName"],
                    metadata["authors"][0]["familyName"]) if v)

    with zipfile.ZipFile(archive_path) as bundle:
        source_files = len(bundle.namelist())

    pdf_sha = sha256_of(pdf_path)
    archive_sha = sha256_of(archive_path)
    record = {
        "pdfPages": len(reader.pages),
        "figures": len(re.findall(r"\\begin\{figure\}", tex)),
        "tables": (len(re.findall(r"\\begin\{table\}", tex))
                   + len(re.findall(r"\\captionof\{table\}", tex))),
        "pdfBytes": pdf_path.stat().st_size,
        "pdfSha256": pdf_sha,
        "sourceFiles": source_files,
        "sourceArchiveBytes": archive_path.stat().st_size,
        "sourceArchiveSha256": archive_sha,
        "resolvedReferences": len(set(re.findall(r"\\bibitem\{([^}]+)\}", tex))
                                  or re.findall(r"\\bibitem\{([^}]+)\}",
                                                (HERE / "main.bbl").read_text(encoding="utf-8"))),
        "allFontsEmbedded": not unembedded,
        "embeddedFontCount": embedded_count,
        "unembeddedFonts": unembedded,
        "texOverfullBoxes": count_boxes(log_text, "Overfull"),
        "texUnderfullBoxes": count_boxes(log_text, "Underfull"),
        "texUnresolvedReferences": len(re.findall(r"LaTeX Warning: (Reference|Citation)",
                                                  log_text)),
        "texLaTeXWarnings": len(re.findall(r"LaTeX Warning:", log_text)),
        "metadataAuthorVerified": str(reader.metadata.get("/Author")) == expected_author,
        "abstractCompleteInPdf": complete,
        "abstractLigatureSubstitutions": ligature_substitutions,
        "abstractCharacters": len(abstract),
        "compiler": "Tectonic 0.17.0 / XeTeX",
    }

    # comments.txt is the arXiv comments field and is written by hand. It states
    # the same three counts measured above, so it drifts exactly the way this
    # record used to. Fail the build rather than ship a disagreeing pair.
    comments = (HERE / "comments.txt").read_text(encoding="ascii").strip()
    expected = (f"{record['pdfPages']} pages, {record['figures']} figures, "
                f"{record['tables']} tables")
    if not comments.startswith(expected):
        raise SystemExit(
            f"comments.txt disagrees with the built PDF.\n"
            f"  measured: {expected}\n"
            f"  comments.txt: {comments}")

    previous_path = HERE / "build-validation.json"
    previous = {}
    if previous_path.is_file():
        previous = json.loads(previous_path.read_text(encoding="utf-8"))
    carried = previous.get("sourceArchiveSha256") == archive_sha
    for field, sentinel in HUMAN_FIELDS.items():
        record[field] = previous.get(field, sentinel) if carried else sentinel
    record["humanAssertionsCarriedForward"] = carried
    record["humanAssertionsKey"] = "sourceArchiveSha256"
    record["pdfSha256Note"] = ("xdvipdfmx stamps /CreationDate with the wall clock, "
                               "so pdfSha256 identifies this exact artifact and "
                               "differs between builds of identical sources.")

    previous_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    state = "carried forward" if carried else "reset (sources changed)"
    print(f"build-validation.json: {record['pdfPages']} pages, "
          f"{record['embeddedFontCount']} embedded fonts, "
          f"human assertions {state}")


if __name__ == "__main__":
    main()
