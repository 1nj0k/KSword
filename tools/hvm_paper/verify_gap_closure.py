"""Verify the frozen 20260915 gap-closure archive, including known incomplete runs.

This checks archive identity and analysis denominators, not hypervisor correctness.
Recompute derived data with the commands in the dataset README before invoking it.
"""
import argparse
import csv
import hashlib
from collections import Counter
from pathlib import Path
from analyze import read, write


def verify(root):
    issues=[]
    index=read(root/'raw-index.json')
    indexed={r['path'] for r in index}
    actual={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()
            and 'derived' not in p.relative_to(root).parts and p.name!='raw-index.json'}
    if indexed!=actual or len(indexed)!=len(index):issues.append('raw file set mismatch')
    for item in index:
        p=root/item['path']
        if not p.is_file() or p.stat().st_size!=item['bytes'] or hashlib.sha256(p.read_bytes()).hexdigest()!=item['sha256']:
            issues.append('raw bytes changed: '+item['path'])
    manifest=read(root/'source-manifest.json')
    fw=root/'final-windows';smp=root/'smp'
    ids=[];boots=set();benchmarks=set();counts=Counter()
    for p in fw.glob('windows1-*.json'):
        r=read(p)
        if 'workload' not in r:continue
        ids.append(r['runId']);benchmarks.add(r['binarySha256'])
        counts[r['configuration'],r['workload']]+=1
        if r['status']!='ok':issues.append('benchmark failed: '+p.name)
        for side in ('before','after'):
            boots.add(r[side]['bootUtc'])
            if any(v['name'].replace('.exe','')=='vmware-vmx' for v in r[side]['processes']):
                issues.append('VMware in Windows-only block: '+p.name)
    if len(ids)!=168 or len(set(ids))!=168:issues.append('Windows execution count/IDs')
    if len(boots)!=1 or len(benchmarks)!=1:issues.append('mixed Windows boot/benchmark binary')
    if len(counts)!=21 or set(counts.values())!={8}:issues.append('warmup/repetition denominator')
    w=read(fw/'derived/summary.json')['windows']
    if w['metricRows']!=192 or w['invalidMetricRows'] or w['failedRuns']:issues.append('invalid Windows metrics')
    if len(w['matchedComparisons'])!=8:issues.append('missing metric comparison')
    for r in w['matchedComparisons']:
        if r['offN']!=14 or r['onN']!=7 or abs((r['onMedianSeconds']/r['offMedianSeconds']-1)*100-r['elapsedOverheadPercent'])>1e-9:
            issues.append('comparison arithmetic/denominator: '+r['workload'])
    t=read(fw/'derived/internal-transition-summary.json')
    if t['results']!={'resident-nested-hidehv:True':10,'stop:True':10}:issues.append('internal transition completeness')
    ts=read(fw/'derived/summary.json')['transitions']['results']
    if len(ts)!=2 or sum(ts.values())!=20 or any(not k.endswith(':pass') for k in ts):
        issues.append('transition continuity/results')
    for p in fw.glob('deployment-*.json'):
        # Exact deployed image is also retained in the preflight/environment;
        # do not infer image identity from the source revision alone.
        if manifest['driver'] not in p.read_text(encoding='utf-8-sig'):
            issues.append('deployment binary mismatch: '+p.name)
    s=read(smp/'derived/smp-summary.json')
    if s['effectResults']!={'guest-cpu-load:True':10,'idle:True':10}:issues.append('effect denominator')
    if sum(s['faults'].values())!=50 or sum(n for k,n in s['faults'].items() if k.endswith(':pass'))!=44:
        issues.append('fault denominator/known incomplete records')
    if len(s['incomplete'])!=10:issues.append('known incomplete traces/readbacks omitted')
    for name,pattern,expected in (('faults','page-fault-*.json',50),('cycles','nested-page-*.json',23)):
        raw={p.name for p in smp.glob(pattern)}
        with (smp/'derived'/f'{name}.csv').open(encoding='utf-8-sig',newline='') as f:rows=list(csv.DictReader(f))
        if len(rows)!=expected or len(raw)!=expected or {r['source'] for r in rows}!=raw:
            issues.append(name+' raw/derived coverage mismatch')
    for p in smp.glob('page-fault-*.json'):
        r=read(p)
        if not all(r['assertions'].values()):issues.append('fault control assertion: '+p.name)
        if r['after']['windows']['driverSha256']!=manifest['driver']:issues.append('fault binary mismatch')
    if len(s['writeIsolation'])!=1 or s['writeIsolation'][0]['result']!='pass':issues.append('write isolation incomplete')
    st=read(smp/'derived/stability.json')
    if len(st)!=1 or st[0]['result']!='pass' or st[0]['sampleCount']!=21 or st[0]['driverSha256']!=manifest['driver']:
        issues.append('soak incomplete/identity mismatch')
    baselines=[read(p) for p in root.glob('baseline-*.json') if 'kind' in read(p)]
    if len(baselines)!=3 or any(r['status']!='start_rejected' for r in baselines):issues.append('baseline refusals missing')
    teardown=[read(p) for p in smp.glob('vmware-teardown-*.json')]
    if len(teardown)!=1 or teardown[0]['status']!='passed':issues.append('teardown result missing')
    return {'schemaVersion':1,'status':'pass' if not issues else 'failed',
            'scope':'Frozen archive hashes, identities and complete/incomplete denominators; not runtime correctness',
            'sourceCommit':manifest['sourceCommit'],'driverSha256':manifest['driver'],
            'rawFilesChecked':len(index),'windowsExecutions':len(ids),'windowsBoots':sorted(boots),
            'benchmarkHashes':sorted(benchmarks),'issues':issues}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    root=parser.parse_args().directory
    result=verify(root);write(root/'derived/validation-current.json',result)
    print(result);raise SystemExit(result['status']!='pass')
