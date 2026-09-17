"""Bind a lab candidate to source, PE/PDB identity and locally collected evidence.

This records evidence; it never turns a successful build into a hardware pass.
Requires pefile (already used by the repository's PDB tools).
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import struct
import subprocess
import uuid

import pefile


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def pdb_identity(path):
    data = path.read_bytes()
    if data[:32] != b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0':
        raise ValueError('Expected a Microsoft MSF 7 PDB')
    block_size, _, blocks, directory_size, _, block_map = struct.unpack_from('<6I', data, 32)
    if block_size not in (512, 1024, 2048, 4096) or blocks * block_size != len(data):
        raise ValueError('Invalid PDB block geometry')
    count = (directory_size + block_size - 1) // block_size
    directory_blocks = struct.unpack_from(f'<{count}I', data, block_map * block_size)
    directory = b''.join(data[n * block_size:(n + 1) * block_size] for n in directory_blocks)[:directory_size]
    streams, = struct.unpack_from('<I', directory)
    sizes = struct.unpack_from(f'<{streams}I', directory, 4)
    cursor = 4 + streams * 4
    for stream, size in enumerate(sizes):
        count = 0 if size == 0xffffffff else (size + block_size - 1) // block_size
        pages = struct.unpack_from(f'<{count}I', directory, cursor)
        cursor += count * 4
        if stream == 1:
            info = b''.join(data[n * block_size:(n + 1) * block_size] for n in pages)[:size]
            age, = struct.unpack_from('<I', info, 8)
            return {'guid': str(uuid.UUID(bytes_le=info[12:28])), 'age': age}
    raise ValueError('PDB info stream is missing')


def pe_identity(path):
    with pefile.PE(str(path)) as pe:
        records = []
        for entry in getattr(pe, 'DIRECTORY_ENTRY_DEBUG', []):
            if entry.struct.Type != 2:
                continue
            raw = pe.__data__[entry.struct.PointerToRawData:entry.struct.PointerToRawData + entry.struct.SizeOfData]
            if raw[:4] == b'RSDS':
                age, = struct.unpack_from('<I', raw, 20)
                records.append({'guid': str(uuid.UUID(bytes_le=raw[4:20])), 'age': age})
        if len(records) != 1:
            raise ValueError(f'Expected one RSDS record: {path}')
        return records[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=pathlib.Path, required=True)
    parser.add_argument('--vmx', type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    candidate = args.candidate.resolve()
    driver = candidate / 'KswordARK.sys'
    pdb = candidate / 'KswordARK.pdb'
    pe_id, pdb_id = pe_identity(driver), pdb_identity(pdb)
    if pe_id != pdb_id:
        raise ValueError(f'SYS/PDB mismatch: {pe_id} != {pdb_id}')

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(root), *arguments])

    changed = set(git('diff', '--name-only', '-z', 'HEAD').decode('utf-8').split('\0'))
    changed.update(git('ls-files', '--others', '--exclude-standard', '-z').decode('utf-8').split('\0'))
    source = {name: sha256(root / name) if (root / name).is_file() else None
              for name in sorted(changed) if name}
    files = {name: sha256(candidate / name) for name in
             ('KswordARK.sys', 'KswordARK.pdb', 'hvm_ctl.exe', 'AMD-Lab-TestSigning.cer', 'signing.json')}
    evidence = {p.name: sha256(p) for p in pathlib.Path(__file__).parent.glob('*.log')}
    manifest = {
        'schema': 1, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'sourceCommit': git('rev-parse', 'HEAD').decode().strip(),
        'dirtySourceSha256': source, 'candidateSha256': files, 'rsds': pe_id,
        'pdbIdentityMatched': True, 'localLogSha256': evidence,
        'hardwareAcceptance': {'hostBootRoundTrip': 'NotRun', 'vmwareNativeMode': 'NotRun',
                               'guestSvmRoundTrip': 'NotRun', 'multicoreResidency': 'NotRun',
                               'twoHourSoak': 'NotRun', 'guestDriverLoad': 'NotRun'},
    }
    if args.vmx:
        vmx = args.vmx.resolve()
        manifest['vmx'] = {'path': str(vmx), 'sha256': sha256(vmx)}
    local = pathlib.Path(__file__).parent / 'host.local.json'
    if local.is_file():
        manifest['hostInventory'] = json.loads(local.read_text(encoding='utf-8-sig'))
    target = candidate / 'identity.json'
    target.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'IDENTITY_RESULT=PASS SYS_PDB={pe_id["guid"]}:{pe_id["age"]} HARDWARE=NOT_RUN')
    print(target)


if __name__ == '__main__':
    main()
