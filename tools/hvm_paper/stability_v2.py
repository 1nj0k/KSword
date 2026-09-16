"""Current counter semantics and per-CPU heartbeat checks for monitored soaks."""
import json
from analyze import continuity, hvm, seconds, distribution
from analyze_smp import latest, matches


def summarize_samples(samples, source):
    good=[s for s in samples if s['status']=='ok']
    if len(good)<2:
        return {'source':source,'result':'insufficient_samples','samples':len(samples)},[]
    first,last=good[0]['windows1'],good[-1]['windows1']
    expected_cpus=samples[0].get('expectedResidentProcessors',2)
    rows=[];issues=[];previous=None;initial_ids=None;boots=set()
    for sample in good:
        w=sample['windows1'];c=hvm(w);p=json.loads(w['pageStatusRaw']);m=json.loads(w['metricsRaw'])
        cpus=sorted((r['group'],r['number']) for r in c['processors'])
        if initial_ids is None:initial_ids=cpus
        hb=latest(w['serialTail']);boots.update(s[2] for s in hb.values())
        current=[]
        if not continuity(first,w):current.append('Windows/process identity changed')
        if w['driverSha256']!=first['driverSha256']:current.append('driver identity changed')
        if sample.get('expectedResidentProcessors',2)!=expected_cpus:current.append('declared topology changed')
        if cpus!=initial_ids or len(cpus)!=expected_cpus:current.append('CPU identity changed')
        if c['residentProcessorCount']!=expected_cpus or {'FAULTED','ROLLBACK_REQUIRED'}.intersection(c['stateNames']):
            current.append('unhealthy residency')
        if any(w[k]!=0 for k in ('hvmStatusExit','pageStatusExit','metricsExit')):current.append('query failed')
        if p['active'] or p['retired']:current.append('unexpected mapping/retired page')
        if int(m['ruleAllocations'])!=int(m['ruleFrees']) or int(m['replacementAllocations'])!=int(m['replacementFrees']):
            current.append('outstanding page-control allocation')
        if set(hb)!={0,1} or not all(matches(s,0xa5) for s in hb.values()):current.append('missing/corrupt original-page readback')
        elapsed=None;rate=None;inv_rate=None;ept_rate=None
        if previous:
            old,oc,om,ohb=previous
            elapsed=sample['hostElapsedSeconds']-old['hostElapsedSeconds']
            if any(cpu not in ohb or s[1]<=ohb[cpu][1] or s[2]!=ohb[cpu][2] for cpu,s in hb.items()):
                current.append('stale/restarted guest CPU observer')
            delta=c['vmExitCount']-oc['vmExitCount']
            inv=int(m['inveptAttempts'])-int(om['inveptAttempts'])
            ept=c['exitReasonCount'].get('48',0)-oc['exitReasonCount'].get('48',0)
            if min(delta,inv,ept)<0:current.append('counter reset')
            rate=delta/elapsed;inv_rate=inv/elapsed;ept_rate=ept/elapsed
            if int(m['inveptFailed'])!=int(om['inveptFailed']):current.append('INVEPT failure')
        vmx=[v for v in w['processes'] if v['name']=='vmware-vmx']
        if len(vmx)!=1:current.append('VMware process absent/ambiguous')
        row={'source':source,'sequence':sample['sequence'],'capturedUtc':w['capturedUtc'],
             'residentCpus':c['residentProcessorCount'],'vmExits':c['vmExitCount'],
             'vmExitsPerSecond':rate,'eptViolationsPerSecond':ept_rate,'inveptPerSecond':inv_rate,
             'poolNonpagedBytes':w['memory']['PoolNonpagedBytes'],
             'vmwarePrivateBytes':vmx[0]['privateBytes'] if len(vmx)==1 else None,
             'cpu0GuestUptime':hb.get(0,(None,None))[1],'cpu1GuestUptime':hb.get(1,(None,None))[1],
             'ruleOutstanding':int(m['ruleAllocations'])-int(m['ruleFrees']),
             'replacementOutstanding':int(m['replacementAllocations'])-int(m['replacementFrees']),
             'captureDurationMs':sample['captureDurationMs'],'issues':'; '.join(current)}
        rows.append(row)
        if current:issues.append({'sequence':sample['sequence'],'issues':current})
        previous=(sample,c,m,hb)
    c0,cn=hvm(first),hvm(last)
    m0,mn=json.loads(first['metricsRaw']),json.loads(last['metricsRaw'])
    duration=good[-1]['hostElapsedSeconds']-good[0]['hostElapsedSeconds']
    if len(boots)!=1:issues.append({'guestBootIds':sorted(boots)})
    if len(good)!=len(samples):issues.append({'observerErrors':len(samples)-len(good)})
    if duration<samples[0].get('requestedSeconds',0)-2:issues.append({'incompleteRequestedDuration':duration})
    delta=cn['vmExitCount']-c0['vmExitCount']
    return {'source':source,'result':'pass' if not issues else 'failed_or_incomplete_evidence',
            'sampleCount':len(samples),'sampleSpanSeconds':duration,'issues':issues,
            'driverSha256':first['driverSha256'],'windowsBootUtc':first['bootUtc'],'guestBootIds':sorted(boots),
            'cpuIdentity':initial_ids,'expectedResidentProcessors':expected_cpus,'vmExitDelta':delta,'vmExitsPerSecond':delta/duration,
            'exitReasonDelta':{k:cn['exitReasonCount'].get(k,0)-c0['exitReasonCount'].get(k,0) for k in sorted(set(cn['exitReasonCount'])|set(c0['exitReasonCount']))},
            'inveptAttemptDelta':int(mn['inveptAttempts'])-int(m0['inveptAttempts']),
            'inveptFailureDelta':int(mn['inveptFailed'])-int(m0['inveptFailed']),
            'nonpagedPoolGrowthBytes':last['memory']['PoolNonpagedBytes']-first['memory']['PoolNonpagedBytes'],
            'vmwarePrivateGrowthBytes':rows[-1]['vmwarePrivateBytes']-rows[0]['vmwarePrivateBytes'] if all(r['vmwarePrivateBytes'] is not None for r in rows) else None,
            'sampleIntervalExitsPerSecond':distribution([r['vmExitsPerSecond'] for r in rows if r['vmExitsPerSecond'] is not None]),
            'observerDurationMs':distribution([s['captureDurationMs'] for s in good]),
            'counterScope':'All KSword resident dispatch entries, including early-return nested handling/reflection; not all exits in outer Hyper-V. Actual INVEPT calls include background work. VPID is disabled by this monitor; exit reason 53 is an intercepted nested INVVPID instruction.',
            'limits':'One boot, declared Windows vCPU count, two guest CPU observers, empty page-control slot. Point samples cannot exclude every intervening failure. Flat page-control allocations do not prove no other driver/VMware leak.'},rows
