"""Conservative effect/transaction checks for the two-CPU reserved-page experiment.

Readback is sampled, not a proof of atomic 4-KiB replacement or absence of every
stale translation. All timestamps used for a latency are in one QPC domain.
"""
import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path
from analyze import read, write, table, continuity, distribution

RX = re.compile(r'paper-vcpu ([01]) ([\d.]+) ([0-9a-f-]{36}) ([0-9a-f]{8}) ([0-9a-f]{32})')


def observations(text):
    return [(int(c), float(u), b, v, h) for c, u, b, v, h in RX.findall(text)]


def latest(text):
    out = {}
    for sample in observations(text):
        out[sample[0]] = sample
    return out


def matches(sample, fill):
    value = bytes([fill]) * 4096
    return sample[3:] == (f'{fill:02x}' * 4, hashlib.md5(value).hexdigest())


def both(text, fill):
    result = latest(text)
    return set(result) == {0, 1} and all(matches(s, fill) for s in result.values())


def fresh(before, after):
    b, a = latest(before), latest(after)
    return (set(b) == set(a) == {0, 1} and
            len({s[2] for s in (*b.values(), *a.values())}) == 1 and
            all(a[c][1] > b[c][1] for c in b))


def ledger(before, after):
    names = ('ruleAllocations', 'ruleFrees', 'replacementAllocations', 'replacementFrees',
             'inveptAttempts', 'inveptSucceeded', 'inveptFailed')
    delta = {k: int(after[k]) - int(before[k]) for k in names}
    balanced = (delta['ruleAllocations'] == delta['ruleFrees'] and
                delta['replacementAllocations'] == delta['replacementFrees'] and
                int(after['ruleAllocations']) == int(after['ruleFrees']) and
                int(after['replacementAllocations']) == int(after['replacementFrees']) and
                min(delta.values()) >= 0 and delta['inveptFailed'] == 0)
    return balanced, delta


def trace(events, action, expected, frequency):
    op = action['parsed']['operationId']
    rows = [r for r in events['parsed']['rows'] if r.get('type') == 6 and r.get('pageOperationId') == op]
    stages = [r['pageStage'] for r in rows]
    if stages != expected:
        raise ValueError(f'incomplete trace for operation {op}: {stages}, expected {expected}')
    times = [int(r['timestampQpc']) for r in rows]
    seq = [int(r['sequence']) for r in rows]
    if times != sorted(times) or len(set(seq)) != len(seq) or seq != sorted(seq):
        raise ValueError('nonmonotonic/duplicate trace')
    if rows[-1]['status'] != action['parsed']['lastStatus']:
        raise ValueError('trace/control result mismatch')
    stamps = dict(zip(stages, times))
    intervals = {'operationBodyUs': (times[-1] - times[0]) * 1e6 / frequency}
    for begin, end, name in ((2, 3, 'allocateInitializeUs'), (5, 6, 'commitDrainUs'), (8, 9, 'retireDrainUs')):
        intervals[name] = (stamps[end] - stamps[begin]) * 1e6 / frequency if begin in stamps and end in stamps else None
    return intervals


def faults(root):
    results = []
    expected = {1: [1,2,3,11], 2: [1,2,3,10,11], 3: list(range(1,12)),
                4: list(range(1,12)), 5: [1,7,8,9,11]}
    status = {1:'0xC000009A', 2:'0xC0000120', 3:'0xC0000120', 4:'0xC0350071', 5:'0xC0350071'}
    for path in sorted(root.glob('page-fault-*.json')):
        r = read(path)
        row = {'runId':r['runId'], 'source':path.name, 'faultMode':r['faultMode'], 'result':'incomplete'}
        try:
            if r['status'] != 'control_assertions_pass_guest_evidence_requires_analysis':
                raise ValueError(r.get('error', r['status']))
            b, a, mode = r['before'], r['after'], r['faultMode']
            if not all(r['assertions'].values()):
                raise ValueError('control/lifecycle assertion failed')
            if not (both(b['windows']['serial']['text'], 0xa5) and both(a['windows']['serial']['text'], 0xa5)
                    and fresh(b['windows']['serial']['text'], a['windows']['serial']['text'])):
                raise ValueError('missing fresh two-CPU original-page readback')
            balanced, delta = ledger(b['metrics']['parsed'], a['metrics']['parsed'])
            if not balanced:
                raise ValueError('allocation or invalidation ledger mismatch')
            if r['action']['parsed']['lastStatus'] != status[mode]:
                raise ValueError('unexpected injected status: ' + r['action']['parsed']['lastStatus'])
            freq = int(a['metrics']['parsed']['qpcFrequency'])
            timing = trace(r['events'], r['action'], expected[mode], freq)
            if mode == 5:
                trace(r['events'], r['map'], [1,2,3,4,5,6,11], freq)
                trace(r['events'], r['retry'], [1,8,9,10,11], freq)
                retained = r['retained']['metrics']['parsed']
                if not (int(retained['ruleAllocations']) - int(retained['ruleFrees']) == 1 and
                        int(retained['replacementAllocations']) - int(retained['replacementFrees']) == 1):
                    raise ValueError('failed drain did not retain exactly one object and page')
            row.update(result='pass', guestBootId=latest(a['windows']['serial']['text'])[0][2],
                       driverSha256=a['windows']['driverSha256'], **delta, **timing)
        except (KeyError, ValueError, TypeError) as e:
            row['error'] = str(e)
        results.append(row)
    return results


def cycles(root, serial):
    results = []
    for path in sorted(root.glob('nested-page-*.json')):
        r = read(path)
        row = {'runId':r['runId'], 'source':path.name, 'case':r['case'], 'condition':r['condition'], 'result':'incomplete'}
        try:
            if r['status'] != 'recorded_requires_analysis':
                raise ValueError(r.get('error', r['status']))
            b, a = r['before'], r['after']
            if not continuity(b, a):
                raise ValueError('Windows/process identity changed')
            if any(s['status']['parsed']['residentProcessorCount'] != 2 or
                   {'FAULTED','ROLLBACK_REQUIRED'}.intersection(s['status']['parsed']['stateNames']) for s in (b,a)):
                raise ValueError('unhealthy resident state')
            if a['page']['parsed']['active'] or a['page']['parsed']['retired']:
                raise ValueError('nonempty final slot')
            balanced, delta = ledger(b['metrics']['parsed'], a['metrics']['parsed'])
            if not balanced:
                raise ValueError('allocation or invalidation ledger mismatch')
            before = serial[max(0,b['serialOffset']-6000):b['serialOffset']]
            if not both(before, 0xa5):
                raise ValueError('missing original readback before operation')
            if r['case'] == 'remap-restore':
                mapped, restored = r['mappedSerial'], r['restoredSerial']
                if not (both(mapped,int(r['fill'],16)) and both(restored,0xa5) and
                        fresh(before,mapped) and fresh(mapped,restored)):
                    raise ValueError('missing fresh full-page effect on both CPUs')
                p = r['mapped']['page']['parsed']
                if p['active'] != 1 or p['composedCount'] <= 0 or any(r[s]['exitCode'] for s in ('map','remove')):
                    raise ValueError('map/remove control or composition failed')
                if int(p['originalPhysicalPage'],16) == int(p['shadowPhysicalPage'],16):
                    raise ValueError('original/replacement aliases')
                freq=int(a['metrics']['parsed']['qpcFrequency'])
                mt=trace(r['mapEvents'],r['map'],[1,2,3,4,5,6,11],freq)
                rt=trace(r['removeEvents'],r['remove'],[1,7,8,9,10,11],freq)
                row.update(result='pass',originalBacking=p['originalPhysicalPage'],replacementBacking=p['shadowPhysicalPage'],
                           fill=r['fill'],mapBodyUs=mt['operationBodyUs'],restoreBodyUs=rt['operationBodyUs'],
                           commitDrainUs=mt['commitDrainUs'],retireDrainUs=rt['retireDrainUs'])
            else:
                if (r['action']['exitCode'] == 0 or b['page']['parsed']['generation'] != a['page']['parsed']['generation']
                        or not both(r['serial'],0xa5) or not fresh(before,r['serial'])):
                    raise ValueError('rejection/state/readback check failed')
                row['result']='clean_reject'
            row.update(**delta)
        except (KeyError, ValueError, TypeError) as e:
            row['error']=str(e)
        results.append(row)
    return results


def main():
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    args=parser.parse_args();root=args.directory;derived=root/'derived';derived.mkdir(exist_ok=True)
    # StreamReader offsets count CR and LF separately; preserve raw newlines.
    with (root/'serial.txt').open(encoding='utf-8-sig',newline='') as f: serial=f.read()
    f,c=faults(root),cycles(root,serial)
    table(derived/'faults.csv',f);table(derived/'cycles.csv',c)
    summary={'faults':dict(Counter(f"{r['faultMode']}:{r['result']}" for r in f)),
             'cycles':dict(Counter(f"{r['condition']}:{r['case']}:{r['result']}" for r in c)),
             'latencyUs':{k:distribution([r[k] for r in c if r['result']=='pass']) for k in ('mapBodyUs','restoreBodyUs','commitDrainUs','retireDrainUs')},
             'scope':'Two guest CPU-pinned observers; one reserved 4 KiB page and one boot. Sampled full-page MD5/readback, not arbitrary application atomicity or universal stale-translation freedom. Actual INVEPT deltas include background work. Fault modes 4/5 omit one drain call; not injected hardware INVEPT faults. Allocation ledger covers only page-control objects/backing.',
             'incomplete':[r for r in f+c if r['result']=='incomplete']}
    write(derived/'smp-summary.json',summary);print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
