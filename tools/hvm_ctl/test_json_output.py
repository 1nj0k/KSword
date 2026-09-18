"""Use the production query formatter and Windows PowerShell 5.1 pipe decoder."""
import json
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
fixture = root / 'tools/hvm_ctl/test_query_json.exe'
cli = root / 'tools/hvm_ctl/hvm_ctl.exe'
raw = subprocess.check_output([str(fixture)])
assert raw.isascii(), 'Status JSON must survive ANSI/OEM pipe decoding'
status = json.loads(raw)
assert status['backend'] == 2 and status['svmProbe']['asidCount'] == 64
assert status['nestedLastRefusalSiteText'] == '没有拒绝过'
assert status['svmProbe']['rejectReason'] == 8
assert status['svmProbe']['rejectReasonName'] == 'CR4_UNSUPPORTED_STATE'
assert status['svmProbe']['stateValidMask'] == 31
assert status['svmProbe']['cpuid1Ecx'] == '0x0C000000'
assert status['svmProbe']['xsaveFeatures'] == '0x00000008'
assert status['svmProbe']['cr4'] == '0x0000000000800000'
assert status['svmProbe']['xcr0'] == '0x0000000000000007'
assert status['svmProbe']['xss'] == '0x0000000000000800'
strings = subprocess.check_output([str(fixture), 'strings']).splitlines()
assert json.loads(strings[0]) == '没有拒绝过"\\\n\t😀'
assert json.loads(strings[1]) == '\ufffd\ufffd'
assert all(line.isascii() for line in strings)
commands = subprocess.check_output([str(cli), '--json', 'commands'])
assert commands.isascii()
assert len(json.loads(commands)['commands']) == 62
metrics = json.loads(subprocess.check_output([str(fixture), 'metrics']))
assert metrics['version'] == 4 and metrics['backend'] == 2
assert metrics['svmProcessors'][0]['nestedProbe'] == {
    'valid': 1, 'sequence': 2, 'status': '0x00000000', 'entries': 1,
    'reflections': 1, 'faults': 7, 'exit': '0xFEDCBA9876543210',
    'marker': '0x000000004B534E31'}

# This actually crosses the native stdout -> PS 5.1 string pipeline which failed
# in the guest, with the Chinese legacy decoder explicitly selected.
script = """
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.Encoding]::GetEncoding(936)
$value = & '%s' | ConvertFrom-Json
if ($value.backend -ne 2 -or $value.nestedLastRefusalSiteText.Length -ne 5) { throw 'Status mismatch' }
$catalog = & '%s' --json commands | ConvertFrom-Json
if ($catalog.commands.Count -ne 62) { throw 'Catalog mismatch' }
'POWERSHELL_CP936_JSON=PASS'
""" % (str(fixture).replace("'", "''"), str(cli).replace("'", "''"))
result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script], capture_output=True)
assert result.returncode == 0, result.stderr
print(result.stdout.decode('ascii').strip())
print('QUERY_JSON_UTF8_ESCAPES=PASS (production formatter; simulated response only)')
