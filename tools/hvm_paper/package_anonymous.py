"""Create a local, length-preserving anonymous evidence copy and private provenance.

Never modify originals. The private key and identity map are outside the ZIP.
Binary identity SHA values become stable aliases; packaged file checksums are real.
"""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
ANALYZERS = ("analyze.py", "analyze_smp.py", "transition_metrics.py", "stability_v2.py",
             "analyze_nested_bench.py", "validate.py", "analyze_http_page.py", "analyze_lifecycle.py",
             "analyze_followup.py", "analyze_policy_comparison.py")
GUID = re.compile(r"\b[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\b")
DIGEST = re.compile(r"(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])|(?<![0-9a-fA-F])[0-9a-fA-F]{40}(?![0-9a-fA-F])")
MAC = re.compile(r"\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b")
VMX_UUID = re.compile(r'(\buuid\.(?:bios|location)(?:\\*")?\s*[=:]\s*\\*")'
                      r'([0-9a-fA-F]{2}(?:[ -][0-9a-fA-F]{2}){15})(\\*")', re.I)
BIOS_UUID = re.compile(r'(\bBIOS-UUID\s+(?:is\s+|[=:]\s*))'
                       r'([0-9a-fA-F]{2}(?:[ -][0-9a-fA-F]{2}){15})', re.I)
IP = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Redactor:
    def __init__(self, key):
        self.key = key
        self.mapping = {}
        self.literals = {"KSword": "HVUnit", "Felix3322": "author000", "WangWei-CM": "author0000",
                         "Felix": "user0", "DESKTOP-KJE7JPM": "HOST000-0000000",
                         "DESKTOP-1CBJL0S": "HOST001-0000000"}
        assert all(len(k) == len(v) for k, v in self.literals.items())

    def digest(self, value):
        return hmac.new(self.key, value.lower().encode(), hashlib.sha256).hexdigest()

    def alias(self, value, kind):
        code = self.digest(kind + ":" + value)
        if kind == "guid":
            result = f"{code[:8]}-{code[8:12]}-{code[12:16]}-{code[16:20]}-{code[20:32]}"
        elif kind == "mac":
            result = ":".join(code[i:i+2] for i in range(0, 12, 2))
        elif kind == "vmx-uuid":
            digits = iter(code)
            result = "".join(next(digits) if c in "0123456789abcdefABCDEF" else c for c in value)
        else:
            result = code[:len(value)]
        if value.isupper():
            result = result.upper()
        self.mapping[value] = result
        return result

    def text(self, text):
        original = text
        for old, new in sorted(self.literals.items(), key=lambda pair: -len(pair[0])):
            def replace(match):
                value = match[0]
                result = new.upper() if value.isupper() else new.lower() if value.islower() else new
                self.mapping[value] = result
                return result
            text = re.sub(re.escape(old), replace, text, flags=re.I)
        text = GUID.sub(lambda m: self.alias(m[0], "guid"), text)
        text = DIGEST.sub(lambda m: self.alias(m[0], "digest"), text)
        text = MAC.sub(lambda m: self.alias(m[0], "mac"), text)
        text = VMX_UUID.sub(lambda m: m[1] + self.alias(m[2], "vmx-uuid") + m[3], text)
        text = BIOS_UUID.sub(lambda m: m[1] + self.alias(m[2], "vmx-uuid"), text)
        def ip(match):
            value = match[0]
            octets = value.split(".")
            if any(int(n) > 255 for n in octets) or value in ("127.0.0.1", "0.0.0.0", "255.255.255.255"):
                return value
            private = octets[0] == "10" or octets[:2] == ["192", "168"] or (octets[0] == "172" and 16 <= int(octets[1]) <= 31)
            if not private:
                return value
            code = self.digest("ip:" + value)
            bounds = {1: (1, 9), 2: (10, 90), 3: (100, 155)}
            result = ".".join(str(bounds[len(n)][0] + int(code[i*4:i*4+4], 16) % bounds[len(n)][1])
                              for i, n in enumerate(octets))
            self.mapping[value] = result
            return result
        text = IP.sub(ip, text)
        # Serial offsets count UTF-16 code units. Equal ASCII substitutions preserve both encodings.
        if len(text.encode("utf-16-le")) != len(original.encode("utf-16-le")):
            raise ValueError("redaction changed serial offset units")
        return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    # Refuse any accidental overwrite of a previous review package or private key.
    if output.exists():
        raise ValueError("output already exists; choose a new package directory")
    output.mkdir(parents=True)
    public = output / "evidence"
    public.mkdir()
    redactor = Redactor(secrets.token_bytes(32))
    provenance = []
    datasets = {"legacy-2x2": ROOT / "docs/next/paper-data/20260915-gap-closure",
                "multicore-4x2": ROOT / "docs/next/paper-data/20260915-4x2",
                "pro-followup": ROOT / "docs/next/paper-data/20260916-followup"}
    # Discover exact machine labels from structured records before transforming
    # embedded serial/JSON strings. Keep functional process and OS names intact.
    def discover(value):
        if isinstance(value, dict):
            for name, item in value.items():
                if name.lower() in ("machine", "pscomputername") and isinstance(item, str) and len(item) >= 4:
                    if not any(token.lower() in item.lower() for token in redactor.literals):
                        redactor.literals[item] = "host" + redactor.digest(item)[:len(item)-4]
                discover(item)
        elif isinstance(value, list):
            for item in value:
                discover(item)
    for source in datasets.values():
        for path in source.rglob("*.json"):
            if "derived" not in path.relative_to(source).parts:
                discover(json.loads(path.read_text(encoding="utf-8-sig")))
    for label, source in datasets.items():
        for path in sorted(source.rglob("*")):
            if not path.is_file() or "derived" in path.relative_to(source).parts or path.name in ("raw-index.json", "anonymous-package.json"):
                continue
            if path.suffix.lower() not in (".json", ".jsonl", ".txt", ".csv", ".md", ".log"):
                continue
            raw = path.read_bytes()
            text = raw.decode("utf-8-sig")
            transformed = redactor.text(text).encode("utf-8")
            if raw.startswith(b"\xef\xbb\xbf"):
                transformed = b"\xef\xbb\xbf" + transformed
            relative = Path(redactor.text((Path(label) / path.relative_to(source)).as_posix()))
            target = public / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(transformed)
            provenance.append({"source": str(path.relative_to(ROOT)), "public": relative.as_posix(),
                               "originalSha256": sha(raw), "anonymousSha256": sha(transformed),
                               "originalBytes": len(raw), "anonymousBytes": len(transformed)})
    analysis = public / "analysis"
    analysis.mkdir()
    for name in ANALYZERS:
        path = ROOT / "tools/hvm_paper" / name
        # Analyzer source has no target credentials or machine-specific deployment code.
        analysis.joinpath(name).write_text(redactor.text(path.read_text(encoding="utf-8-sig")), encoding="utf-8")
    reproduce = '''import os, pathlib, subprocess, sys
sys.dont_write_bytecode=True
os.environ['PYTHONDONTWRITEBYTECODE']='1'
root=pathlib.Path(__file__).resolve().parent
def run(script, folder):
    subprocess.run([sys.executable, str(root/'analysis'/script), str(root/folder)], check=True, stdout=subprocess.DEVNULL)
for folder in ('legacy-2x2', 'legacy-2x2/final-windows', 'multicore-4x2/before', 'multicore-4x2/after'):
    run('analyze.py', folder)
    run('transition_metrics.py', folder)
for folder in ('legacy-2x2/smp', 'multicore-4x2/smp', 'pro-followup/lifecycle'):
    run('analyze_smp.py', folder)
sys.path.insert(0, str(root/'analysis'))
from analyze import stability
stability(root/'legacy-2x2/smp', root/'legacy-2x2/smp/derived')
run('analyze_nested_bench.py', 'multicore-4x2')
run('analyze_http_page.py', 'multicore-4x2/application')
run('analyze_http_page.py', 'multicore-4x2/application-v2')
run('analyze_lifecycle.py', 'multicore-4x2')
run('analyze_followup.py', 'pro-followup')
run('analyze_http_page.py', 'pro-followup/application')
run('analyze_policy_comparison.py', 'pro-followup/application')
print('REPRODUCE=PASS')
'''
    (public / "reproduce.py").write_text(reproduce)
    readme = '''# Anonymous experimental evidence

Run `python reproduce.py` with Python 3.10 or later. It uses only the standard library.
Per-run raw records and all recorded failures are retained. Derived tables are regenerated.

## Scope

- `multicore-4x2`: four Windows vCPUs, two Linux guest vCPUs, implementation comparison,
  repeated remap/restore, fault cases, process-owner revocation, and a real HTTP server scenario.
- `legacy-2x2`: earlier evidence, including the ten-minute stability observation.
  That observation does **not** establish ten-minute stability for the new 4x2 build.
- `after-host-build-confounded`, if present: deliberately excluded from primary performance
  conclusions because a host compilation overlapped these measurements.
- `pro-followup`: a working inner Hyper-V/TinyCore baseline and refused monitor
  admission, same-binary exit attribution, paced TCP observations, a version 3
  EPT lease regression and an application policy-fault comparison. The Hyper-V
  result does not demonstrate hot insertion or descendant EPT control.
- No overnight run or Windows 10 LTSC result is claimed.
- Previously running full-chain insertion remains deferred.

## Redaction and integrity

Names, machine labels, GUIDs, MACs, network addresses and 40/64-character identity digests
use consistent aliases. Redaction preserves text length, newlines and UTF-16 serial offsets.
Loopback addresses and functional MD5 checksums remain intact. CPU model, OS builds,
timestamps, durations, PID continuity, GPA values and failures remain available for analysis.
The SHA values inside raw records are identity **aliases**, not downloadable binary checksums.
`manifest.json` contains actual SHA256 checksums of the anonymous files. Private provenance
and the alias key are outside this archive. Originals remain unchanged.

This is an evidence/analysis package, not an anonymous buildable source artifact.
Hardware/build/date combinations can still be distinctive; review the package before submission.
No archive or paper was uploaded by this workflow.
'''
    (public / "README.md").write_text(readme)
    # Rebuild from transformed originals, proving serial offsets remain valid in practice.
    subprocess.run([sys.executable, str(public / "reproduce.py")], check=True,
                   env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    leaks = []
    for path in public.rglob("*"):
        if path.is_file():
            text = path.read_text(encoding="utf-8-sig")
            for token in redactor.literals:
                if re.search(re.escape(token), text + "\n" + path.relative_to(public).as_posix(), re.I):
                    leaks.append({"file": path.relative_to(public).as_posix(), "token": token})
            if re.search(r"ConvertTo-SecureString|BEGIN (?:RSA |OPENSSH )?PRIVATE KEY|github\.com/(?:Felix|KSword)", text, re.I):
                leaks.append({"file": path.relative_to(public).as_posix(), "token": "credential-or-identity-pattern"})
    if leaks:
        (output / "private-scan-failures.json").write_text(json.dumps(leaks, indent=2))
        raise ValueError(f"identity scan failed: {len(leaks)} hits")
    manifest = [{"path": p.relative_to(public).as_posix(), "bytes": p.stat().st_size,
                 "sha256": sha(p.read_bytes())} for p in sorted(public.rglob("*")) if p.is_file()]
    (public / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    private = {"redactionKeyHex": redactor.key.hex(), "aliases": redactor.mapping, "files": provenance}
    (output / "PRIVATE-provenance.json").write_text(json.dumps(private, indent=2) + "\n")
    archive = output / "anonymous-evidence.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as stream:
        for path in sorted(public.rglob("*")):
            if path.is_file():
                stream.write(path, "evidence/" + path.relative_to(public).as_posix())
    with zipfile.ZipFile(archive) as stream:
        if stream.testzip() is not None:
            raise ValueError("ZIP integrity failure")
        for item in manifest:
            if sha(stream.read("evidence/" + item["path"])) != item["sha256"]:
                raise ValueError("archive manifest mismatch")
        if any("PRIVATE" in name for name in stream.namelist()):
            raise ValueError("private provenance entered archive")
    result = {"archive": str(archive), "sha256": sha(archive.read_bytes()),
              "bytes": archive.stat().st_size, "files": len(manifest),
              "reproduction": "passed", "identityScan": "passed", "zipIntegrity": "passed"}
    (output / "package-result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
