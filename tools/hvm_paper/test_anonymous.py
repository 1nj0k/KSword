import json
import unittest
from package_anonymous import Redactor


class RedactionTests(unittest.TestCase):
    def test_offsets_and_identity(self):
        r = Redactor(bytes(range(32)))
        text = 'Felix KSword-HVM-Target DESKTOP-KJE7JPM\r\n中文\r\n'
        text += '64995441-1414-4560-9278-450fbfc72baf\r\n' + 'A1' * 32
        changed = r.text(text)
        self.assertEqual(len(text.encode('utf-16-le')), len(changed.encode('utf-16-le')))
        self.assertEqual(text.count('\r\n'), changed.count('\r\n'))
        self.assertNotIn('Felix', changed)
        self.assertNotIn('KSword', changed)
        self.assertEqual(changed, r.text(text))

    def test_functional_values_and_versions(self):
        r = Redactor(bytes(range(32)))
        text = '10.0.26300.9022 127.0.0.1 25.0.3.0 620f0b67a91f7f74151bc5be745b7110 0x0000000007000000'
        self.assertEqual(r.text(text), text)
        value = r.text('192.168.012.123')
        self.assertEqual(len(value), len('192.168.012.123'))
        self.assertNotEqual(value, '192.168.012.123')

    def test_vmx_uuid_and_functional_bytes(self):
        r = Redactor(bytes(range(32)))
        identifier = '56 4d 95 04 0e 7d 2a 11-98 b0 12 21 43 65 87 a9'
        for key in ('uuid.bios', 'uuid.location'):
            text = key + ' = "' + identifier + '"\r\n'
            for _ in range(3):
                changed = r.text(text)
                self.assertEqual(len(text), len(changed))
                self.assertNotIn(identifier, changed)
                self.assertEqual(changed, r.text(text))
                text = json.dumps(text)
        line = 'vcpu-0 BIOS-UUID is ' + identifier + '\r\n'
        self.assertNotIn(identifier, r.text(line))
        self.assertEqual(len(line), len(r.text(line)))
        self.assertEqual(identifier, r.text(identifier))


if __name__ == '__main__':
    unittest.main()
