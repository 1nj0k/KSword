"""Validate evidence identity, completeness and numerical preconditions.

This validates collection/analysis integrity, not the hypervisor's correctness.
Known unsuccessful trials are allowed and must remain visible in the summary.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

ap=argparse.ArgumentParser();ap.add_argument("directory",type=Path)
ap.add_argument("--windows-only", action="store_true", help="Validate a dedicated matched Windows A/B dataset without pilot-specific guest expectations")
a=ap.parse_args()
root=a.directory
read=lambda p:json.loads(p.read_text(encoding="utf-8-sig"))
issues=[]
index=read(root/"raw-index.json")
for item in index:
    path=root/item["path"]
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item["sha256"]:
        issues.append("raw hash mismatch: "+item["path"])
ids=[];binaries=set();boots=set();counts={}
for path in sorted(root.glob("*.json")):
    value=read(path)
    if not isinstance(value,dict):continue
    if "runId" in value:ids.append(value["runId"])
    if value.get("status")=="started":issues.append("unfinished run: "+path.name)
    if "workload" not in value or "before" not in value:continue
    key=(value["configuration"],value["workload"])
    counts[key]=counts.get(key,0)+1
    if "no-vmware" not in value["configuration"]:continue
    binaries.add(value["binarySha256"])
    for side in ("before","after"):
        boots.add(value[side]["bootUtc"])
        if any(p["name"]=="vmware-vmx" for p in value[side]["processes"]):
            issues.append("VMware present in matched block: "+path.name)
    for line in value["stdout"].splitlines():
        if not line.strip():continue
        metric=json.loads(line)
        if metric.get("kind")=="metadata":
            if metric["affinityResult"]:issues.append("affinity not applied: "+path.name)
        elif "seconds" in metric:
            if not math.isfinite(metric["seconds"]) or metric["seconds"]<=0:
                issues.append("invalid duration: "+path.name)
if len(ids)!=len(set(ids)):issues.append("duplicate run IDs")
if len(binaries)!=1:issues.append("mixed matched-benchmark binaries")
if len(boots)!=1:issues.append("matched blocks span multiple Windows boots")
for key,n in counts.items():
    if n!=8:issues.append(f"expected one warmup and seven measured executions for {key}, got {n}")
summary=read(root/"derived/summary.json")
if summary["windows"]["invalidMetricRows"]:issues.append("invalid Windows metric rows")
comparisons=summary["windows"]["matchedComparisons"]
if len(comparisons)!=8:issues.append("missing matched metrics")
for r in comparisons:
    computed=(r["onMedianSeconds"]/r["offMedianSeconds"]-1)*100
    if abs(computed-r["elapsedOverheadPercent"])>1e-9:issues.append("overhead arithmetic mismatch")
    if r["offN"]!=14 or r["onN"]!=7:issues.append("wrong comparison denominator")
transitions=summary["transitions"]["results"]
if not a.windows_only and not any("command_timeout_state_reached" in k for k in transitions):
    issues.append("known timeout missing from reliability summary")
if not a.windows_only and summary["linux"]["runResults"].get("sha256sum-unavailable:environment_error")!=7:
    issues.append("known unavailable-tool attempts missing")
if not a.windows_only:
    with (root/"derived/nested-page-runs.csv").open(encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["result"]=="pass" and (r["readClosure"]!="True" or r["activeAfter"]!="0" or r["retiredAfter"]!="0"):
                issues.append("nested PASS without closure: "+r["runId"])
result={"schemaVersion":1,"scope":"pilot dataset integrity, not runtime correctness",
        "status":"pass" if not issues else "failed","rawFilesChecked":len(index),
        "uniqueRunIds":len(set(ids)),"matchedBootIds":sorted(boots),
        "matchedBinaryHashes":sorted(binaries),"issues":issues}
(root/"derived/validation.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
print(json.dumps(result));raise SystemExit(bool(issues))
