"""Render the original, evidence-scoped architecture figure at publication size.

Run from any directory with Python and Matplotlib. No experiment is executed.
The output contains no author identity. This is a diagram, not measured data.
"""

from pathlib import Path
import json
import xml.etree.ElementTree as ET

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch


ROOT = Path(__file__).resolve().parent
OUT = ROOT / "figures"
WIDTH = 178 / 25.4 * 72
HEIGHT = 562
DESCRIPTION = (
    "Two separately evaluated experiments. Windows 1 live interposition is "
    "tested without an intermediate VMM running. Descendant page replacement "
    "is tested with VMware and TinyCore already running beneath the monitor. "
    "Windows 0 and HVCI occupy the sibling outer Hyper-V root partition. "
    "One TinyCore GPA is redirected after EPT composition; observed bytes "
    "change A5, D1, B2, A5. Intermediate Hyper-V testing remains pending."
)


def render(gray=False):
    ink, muted, line = "#17283A", "#455564", "#A3B0BA"
    blue, teal, tint, gold = "#123958", "#087D85", "#EAF5F5", "#986109"
    if gray:
        ink, muted, line = "#161616", "#464646", "#989898"
        blue, teal, tint, gold = "#282828", "#454545", "#F0F0F0", "#454545"
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 10,
        "pdf.fonttype": 42, "ps.fonttype": 42, "svg.fonttype": "none",
        "svg.hashsalt": "eurosys2027-architecture-v1",
    })
    fig = plt.figure(figsize=(WIDTH / 72, HEIGHT / 72), facecolor="white")
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set(xlim=(0, WIDTH), ylim=(HEIGHT, 0))
    ax.axis("off")
    text_objects = []

    def text(x, y, s, size=10, color=ink, weight="normal", align="left"):
        obj = ax.text(x, y, s, fontsize=size, color=color, weight=weight,
                      ha=align, va="center", linespacing=1.3, zorder=5)
        text_objects.append(obj)
        return obj

    def box(x, y, w, h, fill="white", edge=line, lw=1, dash=False, radius=5):
        patch = FancyBboxPatch((x, y), w, h,
            boxstyle=f"round,pad=0,rounding_size={radius}",
            facecolor=fill, edgecolor=edge, linewidth=lw,
            linestyle=(0, (3.5, 2.5)) if dash else "solid", zorder=1)
        ax.add_patch(patch)
        return patch

    def arrow(x1, y1, x2, y2, color=ink, dash=False, rad=0, both=False):
        ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2),
            arrowstyle="<->" if both else "-|>", mutation_scale=11,
            linewidth=1.4, color=color, shrinkA=1, shrinkB=1,
            connectionstyle=f"arc3,rad={rad}",
            linestyle=(0, (3.5, 2.5)) if dash else "solid", zorder=3))

    def section(letter, y, title):
        box(8, y - 9, 19, 19, blue, blue, radius=4)
        text(17.5, y, letter, 11, "white", "bold", "center")
        text(35, y, title, 11.5, ink, "bold")

    # A: before and after are states of ONE Windows instance, not two VMs.
    section("a", 15, "Insert a monitor beneath the running Windows OS")
    box(8, 34, 488, 98, "#F5F7F9", "#DAE0E5")
    text(93, 47, "BEFORE", 10, muted, align="center")
    box(20, 59, 145, 43, "white", line)
    text(92.5, 80.5, "Windows 1\ncurrent boot", 10.5, align="center")
    text(222, 65, "insert / remove", 10, teal, align="center")
    arrow(179, 85, 269, 85, teal, both=True)
    box(282, 45, 202, 61, tint, teal, 1.6)
    box(293, 53, 180, 28, "white", teal)
    text(383, 67, "Windows 1 · same boot", 10.5, align="center")
    text(383, 94, "VMX monitor", 10.5, teal, "bold", "center")
    text(20, 119, "7 pairs · same boot/process identities · VMware absent", 10, muted)

    # B: the outer root and target are sibling Hyper-V partitions.
    section("b", 153, "Control a descendant while VMware is already running")
    box(8, 177, 124, 153, "#F5F7F9", line)
    text(70, 194, "Root partition", 10, muted, align="center")
    text(70, 225, "Windows 0", 12, ink, "bold", "center")
    box(21, 246, 98, 27, "white", line)
    text(70, 259.5, "HVCI active", 10.5, ink, "bold", "center")
    text(70, 304, "Outside monitor", 10, muted, align="center")

    box(146, 177, 350, 153, "#F5F7F9", line)
    text(321, 194, "Hyper-V guest partition", 10, muted, align="center")
    box(158, 210, 326, 109, tint, teal, 1.6)
    box(170, 221, 302, 69, "white", teal)
    text(183, 237, "Windows 1", 11, ink, "bold")
    text(183, 268, "2 vCPUs\nVBS off", 10, muted)
    box(260, 232, 201, 54, "#F2F5F8", line)
    text(272, 245, "VMware", 10.5, ink, "bold")
    box(286, 253, 166, 29, "white", blue, 1)
    text(369, 267.5, "TinyCore\n1 vCPU · diagnostic", 10, align="center")
    text(321, 305, "VMX monitor · 2 resident CPUs", 10.5, teal, "bold", "center")
    ax.plot([70, 70], [330, 345], color=ink, lw=1.2)
    ax.plot([321, 321], [330, 345], color=ink, lw=1.2)
    box(8, 345, 488, 27, blue, blue)
    text(252, 358.5, "Outer Hyper-V · Intel VT-x / EPT", 11, "white", "bold", "center")
    text(252, 388, "Page operations preserve Windows / VMware / TinyCore identities.",
         10, muted, align="center")

    # C: arrows terminate in the monitor/Windows 1 physical address domain.
    section("c", 413, "Redirect one descendant page, then restore")
    box(8, 448, 145, 47, "#F5F7F9", line)
    text(80.5, 471.5, "TinyCore GPA\n0x07000000", 10.5, align="center")
    arrow(156, 471.5, 188, 471.5, blue)
    box(192, 444, 137, 55, tint, teal, 1.5)
    text(260.5, 457, "Composed EPT02", 10.5, ink, "bold", "center")
    text(260.5, 472, "EPT12 → EPT01", 10, align="center")
    text(260.5, 487, "+ page policy", 10, teal, align="center")
    box(369, 436, 127, 31, "white", line, dash=True)
    text(432.5, 451.5, "Original backing", 10, align="center")
    box(369, 485, 127, 31, tint, teal, 1.5)
    text(432.5, 500.5, "Replacement", 10.5, teal, "bold", "center")
    arrow(331, 466, 367, 452, muted, dash=True)
    arrow(331, 479, 367, 500, teal)

    text(252, 535, "A5 original  →  D1 remap  →  B2 guest write  →  A5 restored",
         10, gold, "bold", "center")
    text(252, 554, "Hyper-V as an intermediate VMM: not yet evaluated.",
         10, muted, align="center")

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    extent = fig.bbox
    clipped = []
    for obj in text_objects:
        b = obj.get_window_extent(renderer)
        if b.x0 < extent.x0 or b.x1 > extent.x1 or b.y0 < extent.y0 or b.y1 > extent.y1:
            clipped.append(obj.get_text())
    if clipped:
        raise RuntimeError(f"Text outside figure: {clipped}")
    stem = "architecture-gray" if gray else "architecture"
    metadata = {"Title": "Live interposition and descendant-page control",
                "Subject": DESCRIPTION, "Author": "", "CreationDate": None,
                "ModDate": None}
    fig.savefig(OUT / f"{stem}.pdf", metadata=metadata)
    fig.savefig(OUT / f"{stem}.png", dpi=240)
    if not gray:
        svg = OUT / f"{stem}.svg"
        fig.savefig(svg, metadata={"Date": None, "Description": DESCRIPTION})
        content = svg.read_text(encoding="utf-8")
        end = content.index(">", content.index("<svg")) + 1
        content = content[:end] + "\n<title>Live interposition and descendant-page control</title>\n<desc>" + DESCRIPTION + "</desc>" + content[end:]
        svg.write_text(content, encoding="utf-8", newline="\n")
        ET.fromstring(content)  # Validate the exported standalone vector file.
    plt.close(fig)
    return {"variant": stem, "labels": len(text_objects), "minimumFontPt": 10,
            "widthMm": 178, "heightMm": round(HEIGHT / 72 * 25.4, 4),
            "textOutsideCanvas": clipped}


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    results = [render(), render(gray=True)]
    (OUT / "render-check.json").write_text(
        json.dumps({"scope": "figure geometry, not experiment validation",
                    "figures": results}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))
