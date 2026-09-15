"""Derive tables from preserved observations; missing evidence never becomes PASS.

Python standard library only. Raw inputs are never rewritten. Re-running produces
the same statistics (including bootstrap seed). This is a single-machine pilot.
"""
import argparse
import csv
import hashlib
import json
import random
import re
import statistics as st
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def table(path, rows):
    if not rows:
        return
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        out = csv.DictWriter(f, fieldnames=keys)
        out.writeheader()
        out.writerows(rows)


def seconds(utc):
    return datetime.fromisoformat(utc.replace("Z", "+00:00")).timestamp()


def distribution(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return {"n": len(values), "median": st.median(values), "mean": st.mean(values),
            "min": min(values), "max": max(values),
            "sampleSd": st.stdev(values) if len(values) > 1 else None}


def identities(state):
    return sorted((p["name"].replace(".exe", ""), p["pid"], p["createdUtc"])
                  for p in state["processes"])


def continuity(before, after):
    return before["bootUtc"] == after["bootUtc"] and identities(before) == identities(after)


def hvm(state):
    return json.loads(state.get("hvmRaw", state.get("hvmStatusRaw", "{}")))


def windows(root, derived):
    rows, groups, errors = [], defaultdict(list), []
    for path in sorted(root.glob("windows1-*.json")):
        r = read(path)
        if "workload" not in r:
            continue
        if r["status"] != "ok":
            errors.append({"runId": r["runId"], "status": r["status"], "file": path.name})
            continue
        lines = [json.loads(s) for s in r["stdout"].splitlines() if s.strip()]
        meta = next(x for x in lines if x.get("kind") == "metadata")
        same = continuity(r["before"], r["after"])
        before, after = hvm(r["before"]), hvm(r["after"])
        expected_on = r["configuration"] in ("ksword-on", "on-no-vmware")
        condition_valid = all(s.get("residentProcessorCount") == (2 if expected_on else 0)
                              for s in (before, after))
        if "no-vmware" in r["configuration"]:
            condition_valid &= all(not any(p["name"] == "vmware-vmx" for p in side["processes"])
                                   for side in (r["before"], r["after"]))
        for v in lines:
            if "workload" not in v:
                continue
            row = {"runId": r["runId"], "configuration": r["configuration"],
                   "workload": v["workload"], "iteration": r["iteration"], "warmup": r["warmup"],
                   "seconds": v.get("seconds"), "nsPerOperation": v.get("nsPerOperation"),
                   "MiBPerSecond": v.get("MiBPerSecond"), "affinityResult": meta["affinityResult"],
                   "continuity": same, "conditionValid": condition_valid,
                   "binarySha256": r["binarySha256"], "scratchRemains": r["scratchRemains"],
                   "reportedVmExitDelta": after.get("vmExitCount", 0)-before.get("vmExitCount", 0),
                   "source": path.name}
            row["valid"] = bool(same and condition_valid and meta["affinityResult"] == 0
                                and not r["scratchRemains"] and v["status"] == "ok")
            rows.append(row)
            if not r["warmup"] and row["valid"]:
                groups[(r["configuration"], v["workload"])].append(v["seconds"])
    table(derived / "windows-runs.csv", rows)
    aggregates = [{"configuration": config, "workload": work, **distribution(values)}
                  for (config, work), values in sorted(groups.items())]
    table(derived / "windows-summary.csv", aggregates)
    comparisons = []
    for work in sorted({k[1] for k in groups}):
        pre = groups.get(("off-pre-no-vmware", work), [])
        post = groups.get(("off-post-no-vmware", work), [])
        on = groups.get(("on-no-vmware", work), [])
        if not (pre and post and on):
            continue
        off = pre + post
        rng = random.Random(20260915)
        boot = sorted(100*(st.median(rng.choices(on, k=len(on))) /
                           st.median(rng.choices(off, k=len(off)))-1) for _ in range(10000))
        comparisons.append({"workload": work, "offN": len(off), "onN": len(on),
                            "offMedianSeconds": st.median(off), "onMedianSeconds": st.median(on),
                            "elapsedOverheadPercent": 100*(st.median(on)/st.median(off)-1),
                            "bootstrap95LowPercent": boot[249], "bootstrap95HighPercent": boot[9749],
                            "offPostVsPrePercent": 100*(st.median(post)/st.median(pre)-1)})
    table(derived / "windows-overhead.csv", comparisons)
    return {"metricRows": len(rows), "invalidMetricRows": sum(not r["valid"] for r in rows),
            "failedRuns": errors, "summary": aggregates, "matchedComparisons": comparisons,
            "method": "Off/pre, on, off/post sequential blocks. 7 measured repetitions per block plus one warmup. Off samples pooled (n=14). 10000 percentile bootstrap resamples of medians, seed 20260915. Conditional descriptive CI; not independent machine/boot replication. Positive percent means longer elapsed time. No comparison to VMware-present block."}


def nested(root, derived):
    rows = []
    for path in sorted(root.glob("nested-page-*.json")):
        r = read(path)
        if r["status"] != "recorded_requires_analysis":
            rows.append({"runId": r["runId"], "case": r["case"], "result": "incomplete", "source": path.name})
            continue
        before, after = r["before"], r["after"]
        b, a = before["page"]["parsed"], after["page"]["parsed"]
        same = continuity(before, after)
        empty = a["active"] == 0 and a["retired"] == 0
        healthy = all(s["status"]["parsed"]["residentProcessorCount"] == 2 and
                      not {"FAULTED", "ROLLBACK_REQUIRED"}.intersection(s["status"]["parsed"]["stateNames"])
                      for s in (before, after))
        row = {"runId": r["runId"], "case": r["case"], "condition": r["condition"],
               "gpa": r["gpa"], "ept12Root": r["ept12Root"], "continuity": same,
               "activeAfter": a["active"], "retiredAfter": a["retired"], "source": path.name}
        if r["case"] == "remap-restore":
            values = re.findall(r"paper-page\s+[\d.]+ [\d.]+\s+([0-9a-f]{8})", r["serial"])
            expected = r["fill"].lower()*4
            closed = expected in values and values[-1:] == ["a5a5a5a5"]
            p = r["mapped"]["page"]["parsed"]
            row.update(fill=r["fill"], values=">".join(values), readClosure=closed,
                       originalBacking=p["originalPhysicalPage"], replacementBacking=p["shadowPhysicalPage"],
                       composedCount=p["composedCount"], mapCommandMs=r["map"]["commandElapsedMs"],
                       restoreCommandMs=r["remove"]["commandElapsedMs"],
                       generationBefore=b["generation"], generationMapped=p["generation"], generationAfter=a["generation"])
            valid = (same and healthy and empty and closed and r["map"]["exitCode"] == 0
                     and r["remove"]["exitCode"] == 0 and p["active"] == 1 and p["composedCount"] > 0)
            row["result"] = "pass" if valid else "failed_or_incomplete_evidence"
        else:
            action = r["action"]
            clean = (same and healthy and empty and action["exitCode"] != 0
                     and b["generation"] == a["generation"])
            row.update(result="clean_reject" if clean else "failed_or_incomplete_evidence",
                       rejectionLayer="CLI parser" if r["case"] == "unaligned-gpa" else "driver control",
                       exitCode=action["exitCode"], lastStatus=(action.get("parsed") or {}).get("lastStatus"))
        rows.append(row)
    table(derived / "nested-page-runs.csv", rows)
    write(derived / "nested-page-runs.json", rows)
    return {"byConditionAndResult": dict(Counter(f'{r.get("condition")}:{r["case"]}:{r["result"]}' for r in rows)),
            "mapCommandMs": distribution([r["mapCommandMs"] for r in rows if "mapCommandMs" in r]),
            "restoreCommandMs": distribution([r["restoreCommandMs"] for r in rows if "restoreCommandMs" in r]),
            "allocationScope": "Only the single nested mapping slot and retired slot are observed. Total driver outstanding allocations, leak freedom and global stale translations are not measured."}


def isolation(root, derived):
    results = []
    for path in sorted(root.glob("write-isolation-*.json")):
        r = read(path)
        if "after" not in r:
            results.append({"source": path.name, "result": "incomplete"})
            continue
        serial = r["after"]["guestSerial"].replace("\r", "")
        tail = serial[serial.rfind("paper-isolation-before"):]
        values = re.findall(r"^ ([0-9a-f]{8})$", tail, re.M)
        boots = re.findall(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", tail)
        before, after = r["before"], r["after"]
        w_same = before["os"]["bootUtc"] == after["os"]["bootUtc"] and identities(before) == identities(after)
        hashes_same = all(before["vmware"].get(k) == after["vmware"].get(k)
                          for k in ("sha256", "configSha256"))
        closed = values == ["a5a5a5a5", "d1d1d1d1", "b2b2b2b2", "a5a5a5a5"]
        result = {"source": path.name, "values": values, "guestBootIds": boots,
                  "windowsAndProcessContinuity": w_same, "vmwareDiskHashesUnchanged": hashes_same,
                  "guestUptimeSamples": [float(x) for x in re.findall(r"^([\d.]+) [\d.]+$", tail, re.M)],
                  "result": "pass" if closed and w_same and hashes_same and len(boots) == 2 and len(set(boots)) == 1 else "failed_or_incomplete_evidence"}
        results.append(result)
    write(derived / "write-isolation.json", results)
    return results


def linux(root, derived):
    serial_path = root / "tinycore-serial.txt"
    if not serial_path.exists():
        return {"status": "missing_raw_serial"}
    serial = serial_path.read_text(encoding="utf-8-sig").replace("\r", "")
    output = derived / "linux-runs"
    output.mkdir(exist_ok=True)
    runs = []
    for prefix, name, expected in (("paper-linux-cpu", "sha256sum-unavailable", 7),
                                    ("paper-md5", "md5-64MiB-ramfile", 8),
                                    ("paper-cdread", "buffered-cdrom-read-32MiB", 8)):
        matches = list(re.finditer(re.escape(prefix)+r"-(\d+)\n(.*?)(?="+re.escape(prefix)+r"-(?:\d+|end))", serial, re.S))
        for m in matches:
            iteration = int(m[1]); raw = m[0]
            uptimes = [float(t) for t in re.findall(r"^([\d.]+) [\d.]+$", raw, re.M)]
            r = {"schemaVersion": 1, "runId": prefix+"-"+m[1], "workload": name,
                 "iteration": iteration, "warmup": iteration == 0, "source": serial_path.name,
                 "raw": raw, "guestUptimeSamples": uptimes,
                 "status": "environment_error" if "not found" in raw else "ok" if "rc=0" in raw else "incomplete",
                 "hostTimestamp": None, "timestampNote": "Raw serial has guest uptime only; no synchronized host timestamp per command."}
            r["uptimeBracketSeconds"] = uptimes[-1]-uptimes[0] if len(uptimes) == 2 else None
            dd = re.search(r"33554432 bytes .*? copied, ([\d.]+) seconds", raw)
            if dd:
                r["ddReportedSeconds"] = float(dd[1]);r["MiBPerSecond"] = 32/float(dd[1])
            if name == "md5-64MiB-ramfile":
                r["digest"] = (re.search(r"^([0-9a-f]{32})  ", raw, re.M) or [None, None])[1]
                r["status"] = "ok" if r["status"] == "ok" and r["digest"] == "7f614da9329cd3aebf59b91aadc30bf0" else "failed"
            write(output / (r["runId"]+".json"), r);runs.append(r)
        if len(matches) != expected:
            runs.append({"workload": name, "status": "incomplete_series", "found": len(matches), "expected": expected})
    pings = []
    for m in re.finditer(r"PING (\d+\.\d+\.\d+\.\d+) .*?(?=paper-(?:ping|loopback|dns-ping)-end)", serial, re.S):
        counts = re.search(r"(\d+) packets transmitted, (\d+) packets received", m[0])
        if not counts:
            continue
        r = {"schemaVersion": 1, "runId": "ping-"+m[1], "destination": m[1],
             "transmitted": int(counts[1]), "received": int(counts[2]),
             "rttMs": [float(x) for x in re.findall(r"time=([\d.]+) ms", m[0])],
             "status": "ok" if counts[1] == counts[2] else "packet_loss", "raw": m[0],
             "source": serial_path.name, "repetitionsNote": "Packets in one invocation, not independent runs."}
        write(output / (r["runId"]+".json"), r);pings.append(r)
    table(derived / "linux-runs.csv", [{k:v for k,v in r.items() if k not in ("raw", "guestUptimeSamples")} for r in runs])
    return {"runResults": dict(Counter(f'{r["workload"]}:{r["status"]}' for r in runs)),
            "md5UptimeBracketSeconds": distribution([r["uptimeBracketSeconds"] for r in runs if r.get("workload") == "md5-64MiB-ramfile" and not r["warmup"] and r["status"] == "ok"]),
            "cdromDdSeconds": distribution([r["ddReportedSeconds"] for r in runs if r.get("workload") == "buffered-cdrom-read-32MiB" and not r["warmup"] and r["status"] == "ok"]),
            "pings": [{k:v for k,v in r.items() if k != "raw"} for r in pings],
            "limits": "No matched no-KSword guest baseline. MD5 bracket includes shell/serial overhead at 10 ms uptime resolution. CDROM reads are buffered; warmup separate. No disk write test on RAM rootfs. No pure memory latency/bandwidth or network throughput result."}


def stability(root, derived):
    results, rows = [], []
    for path in sorted(root.glob("stability-*.jsonl")):
        samples = [json.loads(s) for s in path.read_text(encoding="utf-8-sig").splitlines() if s.strip()]
        if samples and samples[0].get('schemaVersion', 1) >= 2:
            from stability_v2 import summarize_samples
            result, new_rows = summarize_samples(samples, path.name)
            results.append(result); rows.extend(new_rows)
            continue
        good = [s for s in samples if s["status"] == "ok"]
        if len(good) < 2:
            results.append({"source": path.name, "status": "insufficient_samples"});continue
        first, last = good[0]["windows1"], good[-1]["windows1"]
        duration = seconds(last["capturedUtc"])-seconds(first["capturedUtc"])
        counters, pages = [hvm(s["windows1"]) for s in good], [json.loads(s["windows1"]["pageStatusRaw"]) for s in good]
        all_identity = all(continuity(first, s["windows1"]) for s in good)
        cpu_identity = [[(p["index"], p["group"], p["number"]) for p in c["processors"]] for c in counters]
        delta = counters[-1]["vmExitCount"]-counters[0]["vmExitCount"]
        reason_deltas = {k: counters[-1]["exitReasonCount"].get(k, 0)-counters[0]["exitReasonCount"].get(k, 0)
                         for k in sorted(set(counters[-1]["exitReasonCount"]) | set(counters[0]["exitReasonCount"]))}
        heartbeats = sorted(set((float(u), float(idle), boot) for s in good for u,idle,boot in
                            re.findall(r"paper-hb ([\d.]+) ([\d.]+) ([0-9a-f-]{36})", s["windows1"]["serialTail"])))
        for i,s in enumerate(good):
            w=s["windows1"];c=counters[i]
            prev=good[i-1]["windows1"] if i else None
            interval=seconds(w["capturedUtc"])-seconds(prev["capturedUtc"]) if prev else None
            rows.append({"source":path.name,"sequence":s["sequence"],"capturedUtc":w["capturedUtc"],
                         "residentCpus":c["residentProcessorCount"],"reportedVmExits":c["vmExitCount"],
                         "reportedExitsPerSecond":(c["vmExitCount"]-counters[i-1]["vmExitCount"])/interval if interval else None,
                         "poolNonpagedBytes":w["memory"]["PoolNonpagedBytes"],"poolPagedBytes":w["memory"]["PoolPagedBytes"],
                         "vmwarePrivateBytes":next(p["privateBytes"] for p in w["processes"] if p["name"]=="vmware-vmx"),
                         "activeMappings":pages[i]["active"],"retiredPages":pages[i]["retired"],"captureDurationMs":s["captureDurationMs"]})
        results.append({"source":path.name,"sampleCount":len(samples),"observerFailures":len(samples)-len(good),
                        "firstUtc":first["capturedUtc"],"lastUtc":last["capturedUtc"],"sampleSpanSeconds":duration,
                        "windowsProcessIdentityStable":all_identity,"cpuIdentityStable":all(c==cpu_identity[0] for c in cpu_identity),
                        "cpuIdentity":cpu_identity[0],"allResidentTwo":all(c["residentProcessorCount"]==2 for c in counters),
                        "faultedSamples":sum(bool({"FAULTED","ROLLBACK_REQUIRED"}.intersection(c["stateNames"])) for c in counters),
                        "reportedVmExitDelta":delta,"reportedVmExitsPerSecond":delta/duration,
                        "exitReasonDelta":reason_deltas,
                        "counterScope": "Existing ordinary-exit telemetry; reflected/handled L2 exits return before this counter. This is not all hardware exits across layers. Reasons 50/53 are nested INVEPT/INVVPID instruction exits, not total invalidations executed.",
                        "nonpagedPoolGrowthBytes":last["memory"]["PoolNonpagedBytes"]-first["memory"]["PoolNonpagedBytes"],
                        "vmwarePrivateGrowthBytes":next(p["privateBytes"] for p in last["processes"] if p["name"]=="vmware-vmx")-next(p["privateBytes"] for p in first["processes"] if p["name"]=="vmware-vmx"),
                        "guestHeartbeats":heartbeats,"guestBootIds":sorted(set(h[2] for h in heartbeats)),
                        "observerDurationMs":distribution([s["captureDurationMs"] for s in good]),
                        "droppedEventsDelta":counters[-1]["droppedEventCount"]-counters[0]["droppedEventCount"],
                        "overwrittenEventsDelta":counters[-1]["overwrittenEventCount"]-counters[0]["overwrittenEventCount"]})
    table(derived/"stability-samples.csv",rows);write(derived/"stability.json",results)
    return results


def transitions(root, derived):
    rows, cpu_rows = [], []
    for path in sorted(root.glob("transition-*.json")):
        r = read(path)
        if "probeRaw" not in r:
            rows.append({"runId":r["runId"],"result":"incomplete","source":path.name});continue
        probes=[json.loads(line) for line in r["probeRaw"].splitlines() if line.startswith('{"schemaVersion":1,"kind":')]
        if not probes:
            rows.append({"runId":r["runId"],"result":"missing_probe","source":path.name});continue
        p=probes[-1];before,after=hvm(r["before"]),hvm(r["after"])
        same=continuity(r["before"],r["after"])
        wanted=0 if r["command"]=="stop" else 2
        valid=(same and p["commandExitCode"]==0 and after["residentProcessorCount"]==wanted
               and not {"FAULTED","ROLLBACK_REQUIRED"}.intersection(after["stateNames"]))
        outcome="pass" if valid else "command_timeout_state_reached" if p["commandExitCode"]==258 and same and after["residentProcessorCount"]==wanted else "failed"
        row={"runId":r["runId"],"command":r["command"],"iteration":r["iteration"],"condition":r["condition"],"result":outcome,
             "continuity":same,"residentBefore":before["residentProcessorCount"],"residentAfter":after["residentProcessorCount"],
             "commandWallMs":1000*(p["commandEndQpc"]-p["commandStartQpc"])/p["qpcFrequency"],
             "internalTransitionPauseMs":None,"censoredAtTimeout":p["commandExitCode"]==258,"source":path.name}
        rows.append(row)
        for c in p["processors"]:
            pre,overlap,post=[],[],[]
            for gap in c["largestGaps"]:
                if not gap["startQpc"]:continue
                ms=1000*(gap["endQpc"]-gap["startQpc"])/p["qpcFrequency"]
                bucket=pre if gap["endQpc"]<p["commandStartQpc"] else post if gap["startQpc"]>p["commandEndQpc"] else overlap
                bucket.append(ms)
            cpu_rows.append({"runId":r["runId"],"command":r["command"],"group":c["group"],"number":c["number"],
                             "affinityError":c["affinityError"],"samples":c["samples"],
                             "largestPreCommandGapMs":max(pre,default=None),"largestCommandOverlapGapMs":max(overlap,default=None),
                             "largestPostCommandGapMs":max(post,default=None)})
    table(derived/"transition-runs.csv",rows);table(derived/"transition-cpu-gaps.csv",cpu_rows)
    return {"results":dict(Counter(f'{r.get("condition")}:{r.get("command")}:{r["result"]}' for r in rows)),
            "commandWallMs":{condition+":"+cmd:distribution([r["commandWallMs"] for r in rows if r.get("command")==cmd and r.get("condition")==condition and r["result"]=="pass"]) for condition in sorted({r.get("condition","") for r in rows}) for cmd in ("resident-nested-hidehv","stop")},
            "internalPhaseTimes":None,"perCpuTransitionTime":None,"ipiLatency":None,
            "limits":"Busy QPC loops on both CPUs perturb scheduling. Top 16 gaps only per CPU; missing overlap is not zero. CLI process creation is inside command wall time. Gaps and command duration cannot identify exact kernel quiesce/capture/VMCS/EPT/resume timings or simultaneous topology entry."}


def main():
    ap=argparse.ArgumentParser();ap.add_argument("directory",type=Path);args=ap.parse_args()
    root=args.directory;derived=root/"derived";derived.mkdir(exist_ok=True)
    result={"schemaVersion":1,"experiment":root.name,"windows":windows(root,derived),
            "nestedPages":nested(root,derived),"writeIsolation":isolation(root,derived),
            "linux":linux(root,derived),"stability":stability(root,derived),"transitions":transitions(root,derived)}
    write(derived/"summary.json",result)
    index=[{"path":str(p.relative_to(root)).replace("\\","/"),"bytes":p.stat().st_size,
            "sha256":hashlib.sha256(p.read_bytes()).hexdigest()}
           for p in sorted(root.rglob("*")) if p.is_file() and 'derived' not in p.relative_to(root).parts and p.name!="raw-index.json"]
    write(root/"raw-index.json",index)
    print(json.dumps({"rawFiles":len(index),"windowsComparisons":result["windows"]["matchedComparisons"],
                      "nested":result["nestedPages"]["byConditionAndResult"],"transitions":result["transitions"]["results"]},ensure_ascii=False))


if __name__=="__main__":main()
