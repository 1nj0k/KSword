"""Regression checks for refusing incomplete or contradictory timing evidence."""
import copy
import unittest
from transition_metrics import decode


def sample():
    cpu = {'group': 0, 'number': 0, 'validMask': 127,
           'qpc': dict(ipiEnter='110', vmcsBegin='120', stateCaptured='140',
                       vmcsWritten='150', entryBefore='155', entryAfter='170', ipiLeave='180')}
    return dict(kind='hvm-metrics', version=1, transitionCoherent=True, transitionSequence=2,
                command=5, lastStatus='0x00000000', qpcFrequency='10000000',
                commandBeginQpc='100', commandEndQpc='200', snapshotBeginQpc='210',
                snapshotEndQpc='220', processorCount=1, processors=[cpu], globalValidMask=48,
                globalQpc=dict(resourcesBegin='0', resourcesEnd='0', eptBegin='0', eptEnd='0',
                               rendezvousBegin='105', rendezvousEnd='190'))


class EvidenceTests(unittest.TestCase):
    def test_units_and_missing_phase(self):
        result, rows = decode(sample())
        self.assertEqual(result['rendezvousEnvelopeUs'], 8.5)
        self.assertIsNone(result['eptBuildUs'])
        self.assertEqual(rows[0]['captureAndControlSelectionUs'], 2.0)

    def test_torn_snapshot_is_refused(self):
        value = sample()
        value['transitionCoherent'] = False
        with self.assertRaises(ValueError):
            decode(value)

    def test_failed_transition_cannot_be_a_success_latency(self):
        value = sample()
        value['lastStatus'] = '0xC0000001'
        with self.assertRaises(ValueError):
            decode(value)

    def test_missing_cpu_entry_is_refused(self):
        value = sample()
        value['processors'][0]['validMask'] = 3
        with self.assertRaises(ValueError):
            decode(value)

    def test_outside_rendezvous_is_refused(self):
        value = sample()
        value['processors'][0]['qpc']['ipiLeave'] = '201'
        with self.assertRaises(ValueError):
            decode(value)

    def test_duplicate_cpu_is_refused(self):
        value = sample()
        value['processors'].append(copy.deepcopy(value['processors'][0]))
        value['processorCount'] = 2
        with self.assertRaises(ValueError):
            decode(value)


if __name__ == '__main__':
    unittest.main()
