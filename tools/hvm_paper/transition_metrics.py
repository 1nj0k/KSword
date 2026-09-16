"""Validate internal intervals; an absent boundary is never zero.

The IPI envelope bounds the rendezvous, not application latency or simultaneous
entry. QPC values share one Windows boot and use the driver's reported frequency.
"""
import argparse
import json
from pathlib import Path
from collections import Counter, defaultdict

CPU_NAMES = ('ipiEnter', 'ipiLeave', 'vmcsBegin', 'stateCaptured',
             'vmcsWritten', 'entryBefore', 'entryAfter')
GLOBAL_NAMES = ('resourcesBegin', 'resourcesEnd', 'eptBegin', 'eptEnd',
                'rendezvousBegin', 'rendezvousEnd')


def interval(values, mask, names, first, last, frequency):
    required = (1 << names.index(first)) | (1 << names.index(last))
    if mask & required != required:
        return None
    begin, end = int(values[first]), int(values[last])
    if end < begin:
        raise ValueError(f'reversed interval: {first}/{last}')
    return (end - begin) * 1_000_000 / frequency


def decode(m):
    if m['kind'] != 'hvm-metrics' or m['version'] not in (1, 2):
        raise ValueError('unknown metrics schema')
    if not m['transitionCoherent'] or m['transitionSequence'] % 2:
        raise ValueError('incomplete transition snapshot')
    if m['lastStatus'] != '0x00000000':
        raise ValueError('failed transition; not a success latency sample')
    f = int(m['qpcFrequency'])
    if f <= 0:
        raise ValueError('missing frequency')
    begin, end = int(m['commandBeginQpc']), int(m['commandEndQpc'])
    if not 0 < begin <= end <= int(m['snapshotBeginQpc']) <= int(m['snapshotEndQpc']):
        raise ValueError('invalid operation/snapshot ordering')
    cpus = m['processors']
    if not cpus or len(cpus) != m['processorCount']:
        raise ValueError('incomplete CPU set')
    ids = [(c['group'], c['number']) for c in cpus]
    if len(set(ids)) != len(ids):
        raise ValueError('duplicate CPU identity')
    g, gm = m['globalQpc'], m['globalValidMask']
    result = {'commandBodyUs': (end - begin) * 1_000_000 / f}
    for first, last, name in (
        ('resourcesBegin', 'resourcesEnd', 'resourcePreparationUs'),
        ('eptBegin', 'eptEnd', 'eptBuildUs'),
        ('rendezvousBegin', 'rendezvousEnd', 'rendezvousEnvelopeUs'),
    ):
        result[name] = interval(g, gm, GLOBAL_NAMES, first, last, f)
    if result['rendezvousEnvelopeUs'] is None:
        raise ValueError('missing rendezvous interval')
    cpu_rows = []
    for c in cpus:
        q, mask = c['qpc'], c['validMask']
        expected_mask = 127 if m['command'] == 5 else 3
        if mask & expected_mask != expected_mask:
            raise ValueError('missing CPU boundaries')
        sequence = ('ipiEnter', 'vmcsBegin', 'stateCaptured', 'vmcsWritten',
                    'entryBefore', 'entryAfter', 'ipiLeave') if m['command'] == 5 else ('ipiEnter', 'ipiLeave')
        ordered = [begin, int(g['rendezvousBegin']), *(int(q[s]) for s in sequence),
                   int(g['rendezvousEnd']), end]
        if ordered != sorted(ordered):
            raise ValueError('CPU boundaries outside their containing interval')
        row = {'group': c['group'], 'number': c['number']}
        for first, last, name in (
            ('ipiEnter', 'ipiLeave', 'ipiCallbackUs'),
            ('vmcsBegin', 'stateCaptured', 'captureAndControlSelectionUs'),
            ('stateCaptured', 'vmcsWritten', 'vmcsProgrammingUs'),
            ('entryBefore', 'entryAfter', 'entryContinuationUs'),
        ):
            row[name] = interval(q, mask, CPU_NAMES, first, last, f)
        cpu_rows.append(row)
    for stage, name in (('ipiEnter', 'ipiArrivalSkewUs'), ('ipiLeave', 'ipiDepartureSkewUs'),
                        ('entryAfter', 'entryReturnSkewUs')):
        values = [int(c['qpc'][stage]) for c in cpus if c['validMask'] & (1 << CPU_NAMES.index(stage))]
        result[name] = (max(values) - min(values)) * 1_000_000 / f if len(values) == len(cpus) else None
    return result, cpu_rows


def summarize(root, derived):
    from analyze import read, table, write, continuity, hvm, distribution
    rows, cpus, groups = [], [], defaultdict(list)
    for path in sorted(root.glob('transition-*.json')):
        r = read(path)
        row = {'runId': r['runId'], 'source': path.name, 'command': r['command'],
               'iteration': r['iteration'], 'driverSha256': r.get('driverSha256'), 'valid': False}
        try:
            m = json.loads(r['metricsRaw'])
            command = 5 if r['command'] == 'resident-nested-hidehv' else 6
            if m['command'] != command or r['metricsExitCode'] != 0:
                raise ValueError('query does not identify the measured command')
            if not continuity(r['before'], r['after']):
                raise ValueError('Windows boot/process identities changed')
            a, b = hvm(r['after']), hvm(r['before'])
            count = m['processorCount']
            if (b['residentProcessorCount'], a['residentProcessorCount']) != ((0, count) if command == 5 else (count, 0)):
                raise ValueError('unexpected residency transition')
            expected_ids = {(c['group'], c['number']) for c in a['processors']}
            if {(c['group'], c['number']) for c in m['processors']} != expected_ids:
                raise ValueError('CPU identity differs from runtime snapshot')
            values, per_cpu = decode(m)
            row.update(values, valid=True, transitionSequence=m['transitionSequence'])
            for cpu in per_cpu:
                cpus.append({'runId': r['runId'], 'command': r['command'], **cpu})
            for key, value in values.items():
                if value is not None:
                    groups[(r['command'], key)].append(value)
        except (KeyError, ValueError, TypeError) as error:
            row['error'] = str(error)
        rows.append(row)
    table(derived / 'internal-transition-runs.csv', rows)
    table(derived / 'internal-transition-cpus.csv', cpus)
    preparations = []
    for path in sorted(root.glob('ab-preflight-*.json')):
        r = read(path)
        if not r.get('prepareMetricsRaw'):
            continue
        m = json.loads(r['prepareMetricsRaw'])
        sample = {'source': path.name, 'valid': False}
        try:
            if not m['transitionCoherent'] or m['command'] != 1 or m['lastStatus'] != '0x00000000':
                raise ValueError('no completed prepare interval')
            if not any(c['command'] == 'prepare-eptpsw' and c['exitCode'] == 0 for c in r['commands']):
                raise ValueError('pre-existing prepare interval; not executed in this run')
            begin, end = int(m['commandBeginQpc']), int(m['commandEndQpc'])
            f = int(m['qpcFrequency'])
            q = m['globalQpc']
            ept = interval(q, m['globalValidMask'], GLOBAL_NAMES, 'eptBegin', 'eptEnd', f)
            if ept is None or not 0 < begin <= int(q['eptBegin']) <= int(q['eptEnd']) <= end:
                raise ValueError('missing/out-of-order EPT construction')
            sample.update(valid=True, prepareBodyUs=(end-begin)*1e6/f, eptBuildUs=ept)
        except (KeyError, ValueError, TypeError) as error:
            sample['error'] = str(error)
        preparations.append(sample)
    summary = {'results': dict(Counter(f"{r['command']}:{r['valid']}" for r in rows)),
               'preparationRuns': preparations,
               'distributionsUs': [{'command': c, 'metric': m, **distribution(v)} for (c, m), v in sorted(groups.items())],
               'scope': 'Instrumented C boundaries within one Windows QPC domain. Parallel CPU intervals must not be summed. Rendezvous envelope is a disruption upper bound, not application pause. Entry continuation includes assembly around VMLAUNCH. CPU entry skew does not prove simultaneous entry. Cold resource preparation and warmed repetitions are retained separately by iteration.'}
    write(derived / 'internal-transition-summary.json', summary)
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    root = parser.parse_args().directory
    derived = root / 'derived'
    derived.mkdir(exist_ok=True)
    print(json.dumps(summarize(root, derived), indent=2))
