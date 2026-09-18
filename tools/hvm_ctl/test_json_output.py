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
strings = subprocess.check_output([str(fixture), 'strings']).splitlines()
assert json.loads(strings[0]) == '没有拒绝过"\\\n\t😀'
assert json.loads(strings[1]) == '\ufffd\ufffd'
assert all(line.isascii() for line in strings)
commands = subprocess.check_output([str(cli), '--json', 'commands'])
assert commands.isascii()
assert len(json.loads(commands)['commands']) == 60

# This actually crosses the native stdout -> PS 5.1 string pipeline which failed
# in the guest, with the Chinese legacy decoder explicitly selected.
script = """
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.Encoding]::GetEncoding(936)
$value = & '%s' | ConvertFrom-Json
if ($value.backend -ne 2 -or $value.nestedLastRefusalSiteText.Length -ne 5) { throw 'Status mismatch' }
$catalog = & '%s' --json commands | ConvertFrom-Json
if ($catalog.commands.Count -ne 60) { throw 'Catalog mismatch' }
'POWERSHELL_CP936_JSON=PASS'
""" % (str(fixture).replace("'", "''"), str(cli).replace("'", "''"))
result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script], capture_output=True)
assert result.returncode == 0, result.stderr
print(result.stdout.decode('ascii').strip())
print('QUERY_JSON_UTF8_ESCAPES=PASS (production formatter; simulated response only)')
