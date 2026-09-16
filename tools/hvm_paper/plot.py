"""Export pilot performance plots. Does not collect or change raw data."""
import argparse
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
plt.rcParams['svg.hashsalt']='ksword-hvm-pilot-20260915'

ap=argparse.ArgumentParser()
ap.add_argument("directory",type=Path)
args=ap.parse_args()
directory=args.directory/"derived"
with (directory/"windows-overhead.csv").open(encoding="utf-8-sig") as f:
    rows=list(csv.DictReader(f))
labels={"integer-xorshift":"Integer loop", "cpuid-leaf0":"CPUID leaf 0",
        "memory-copy":"Memory copy", "memcpy-32MiB":"Memory copy",
        "memory-memcpy":"Memory copy", "pointer-chase-64MiB":"Pointer chase",
        "disk-direct-read-64MiB":"VHDX read", "disk-direct-write-64MiB":"VHDX write",
        "tcp-loopback-64MiB":"TCP loopback bulk", "tcp-loopback-rtt-1B":"TCP loopback RTT"}
regular=[r for r in rows if r["workload"]!="cpuid-leaf0"]
cpuid=[r for r in rows if r["workload"]=="cpuid-leaf0"]
fig,axes=plt.subplots(2,1,figsize=(9,6.3),gridspec_kw={"height_ratios":[5,1]})
for ax,items in zip(axes,[regular,cpuid]):
    for i,r in enumerate(items):
        median=float(r["elapsedOverheadPercent"])
        lo=float(r["bootstrap95LowPercent"]);hi=float(r["bootstrap95HighPercent"])
        ax.plot([lo,hi],[i,i],color="#147d92",linewidth=2)
        ax.plot(median,i,"o",color="#123f57",markersize=6)
        ax.annotate(f"{median:+.1f}%",(median,i),xytext=(7,6),textcoords="offset points",fontsize=8)
    ax.set_yticks(range(len(items)),[labels.get(r["workload"],r["workload"]) for r in items])
    ax.axvline(0,color="#888888",linewidth=.8)
    ax.grid(axis="x",alpha=.2);ax.set_axisbelow(True)
    ax.spines[["top","right"]].set_visible(False)
    ax.set_ylim(-.5,len(items)-.3)
    ax.set_xlabel("Elapsed-time change (%) — positive means slower")
fig.suptitle("KSword residency: matched Windows-only pilot",fontsize=14)
fig.text(.02,.012,"One CPU model and one Windows boot; on n=7, off n=14. Bars: percentile bootstrap 95% CI.\nSequential off/on/off blocks; background drift remains. CPUID uses a separate axis.",fontsize=8)
fig.tight_layout(rect=(0,.065,1,.96))
fig.savefig(directory/"windows-overhead.svg",metadata={"Date":None})
fig.savefig(directory/"windows-overhead.png",dpi=180)
plt.close(fig)
