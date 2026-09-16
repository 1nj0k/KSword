"""Reject a successful HTTP transfer when its business or identity evidence fails."""
import copy
import unittest
from analyze_policy_comparison import check, parse


class PolicyEvidenceTests(unittest.TestCase):
    def samples(self):
        return [dict(pid=9, ticks=40, boot="same", uptime=100+i, rc=0, bytes=4096,
                     alive=1, md5="53398dd1f3a4c282ace3b660acd92ff7",
                     holder=dict(pid=8, gpa="0x1000", samePfn=True, uptime=99+i),
                     oracle=dict(bytes=4096, validPolicy=True, decision="quote-1250", uptime=100.2+i))
                for i in range(5)]

    def test_valid_business_response(self):
        self.assertEqual(check(self.samples(), True, 5)["count"], 5)

    def test_incomplete_oracle_cannot_meet_sample_count(self):
        samples = self.samples()
        del samples[0]["oracle"]
        with self.assertRaisesRegex(AssertionError, "only 4"):
            check(samples, True, 5)

    def test_stale_oracle_changed_body_and_identity_are_rejected(self):
        for section, key, value in (("oracle", "uptime", 80), ("oracle", "validPolicy", False),
                                    ("holder", "samePfn", False), (None, "ticks", 41),
                                    (None, "md5", "620f0b67a91f7f74151bc5be745b7110")):
            with self.subTest(section=section, key=key):
                samples = copy.deepcopy(self.samples())
                target = samples[0][section] if section else samples[0]
                target[key] = value
                with self.assertRaises(AssertionError):
                    check(samples, True, 5)

    def test_parse_keeps_raw_offsets_through_interleaved_diagnostics(self):
        prefix = "unrelated diagnostic\r\n"
        row = "paper-http-sample-v2 0 100 0 4096 53398dd1f3a4c282ace3b660acd92ff7 9 40 1 11111111-2222-3333-4444-555555555555\r\n"
        text = prefix + row + "paper-vcpu diagnostic\r\n"
        text += '{"kind":"application-page","pid":8}\r\n'
        text += '{"kind":"policy-oracle","validPolicy":true}\r\n'
        samples = parse(text)
        self.assertEqual(samples[0]["offset"], len(prefix))
        self.assertEqual(samples[0]["holder"]["pid"], 8)
        self.assertTrue(samples[0]["oracle"]["validPolicy"])


if __name__ == "__main__":
    unittest.main()
