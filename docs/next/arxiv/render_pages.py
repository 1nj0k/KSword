"""Rasterize the built preprint so its pages can actually be looked at.

Why this exists: build-validation.json carries a visualReview field, and the
only honest way to fill it in is for someone to look at every page. Counting
overfull boxes in the TeX log is not the same check -- a table can fit the text
block and still collide with its caption, a figure can render blank, and a
generated table can lose a column without TeX complaining at all.

Renders every page of the PDF to a PNG under an output directory. No system
tools are required: pypdfium2 carries its own renderer.

    python docs/next/arxiv/render_pages.py --out <dir> [--scale 1.5]
"""

from __future__ import annotations

import argparse
import pathlib

import pypdfium2

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_PDF = HERE.parent.parent.parent / "output" / "pdf" / "ksword-live-interposition-v1.pdf"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", type=pathlib.Path, default=DEFAULT_PDF)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    # 1.5 is about 108 dpi: small enough to page through, large enough that a
    # collision or a missing table column is visible rather than inferred.
    parser.add_argument("--scale", type=float, default=1.5)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    document = pypdfium2.PdfDocument(str(args.pdf))
    written = []
    for index in range(len(document)):
        image = document[index].render(scale=args.scale).to_pil()
        path = args.out / f"page-{index + 1:02d}.png"
        image.save(path)
        written.append(path)
    print(f"{len(written)} pages rendered from {args.pdf.name} into {args.out}")


if __name__ == "__main__":
    main()
