"""Guard against accepting stale, partial, cross-boot or incomplete evidence."""
import hashlib
import unittest
from analyze_smp import both, fresh, trace, ledger


def line(cpu, uptime=1, boot='11111111-1111-1111-1111-111111111111', fill=0xa5):
    return f'paper-vcpu {cpu} {uptime} {boot} {fill:02x}{fill:02x}{fill:02x}{fill:02x} {hashlib.md5(bytes([fill])*4096).hexdigest()}\n'


class EvidenceTests(unittest.TestCase):
    def test_one_cpu_is_not_multicore_evidence(self):
        self.assertFalse(both(line(0),0xa5))
        self.assertTrue(both(line(0)+line(1),0xa5))

    def test_latest_sample_must_match(self):
        self.assertFalse(both(line(0)+line(1)+line(0,2,fill=0xd1),0xa5))

    def test_stale_and_cross_boot_are_refused(self):
        old=line(0)+line(1)
        self.assertFalse(fresh(old,old))
        self.assertFalse(fresh(old,line(0,2,boot='22222222-2222-2222-2222-222222222222')+line(1,2)))
        self.assertTrue(fresh(old,line(0,2)+line(1,2)))

    def test_full_page_hash_required(self):
        original=line(0)+line(1)
        self.assertFalse(both(original.replace(hashlib.md5(bytes([0xa5])*4096).hexdigest(),'0'*32),0xa5))

    def test_missing_trace_stage_is_not_zero_latency(self):
        action={'parsed':{'operationId':1,'lastStatus':'0x00000000'}}
        with self.assertRaises(ValueError):
            trace({'parsed':{'rows':[]}},action,[1,2,3,11],10000000)

    def test_leak_and_counter_reset_are_refused(self):
        names=('ruleAllocations','ruleFrees','replacementAllocations','replacementFrees','inveptAttempts','inveptSucceeded','inveptFailed')
        old=dict.fromkeys(names,'0');new=dict(old,ruleAllocations='1')
        self.assertFalse(ledger(old,new)[0])
        self.assertFalse(ledger(dict(old,inveptAttempts='2'),old)[0])


if __name__=='__main__':unittest.main()
