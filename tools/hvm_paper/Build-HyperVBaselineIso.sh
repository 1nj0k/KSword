#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
dep="$repo/.deps/hvm-paper"
stage="$dep/hyperv-overlay"
mkdir -p "$stage/opt"
cat > "$stage/opt/bootlocal.sh" <<'BOOT'
#!/bin/sh
exec >/dev/ttyS0 2>&1
echo HYPERV_NORMAL_INIT_READY
uname -a
printf 'VERSION='; cat /etc/os-release
printf 'BOOT_ID='; cat /proc/sys/kernel/random/boot_id
printf 'CPU_ONLINE='; cat /sys/devices/system/cpu/online
printf 'INIT='; cat /proc/1/comm
printf 'CMDLINE='; cat /proc/cmdline
printf 'MEMINFO='; head -n 5 /proc/meminfo
i=0
while [ "$i" -lt 120 ]; do
    printf 'HV_HEARTBEAT seq=%s boot=' "$i"
    tr -d '\n' </proc/sys/kernel/random/boot_id
    printf ' uptime='; cat /proc/uptime
    i=$((i+1))
    sleep 2
done
BOOT
chmod 755 "$stage/opt/bootlocal.sh"
python3 - "$stage" "$dep/hyperv-overlay.gz" <<'PY'
import gzip, pathlib, sys
root = pathlib.Path(sys.argv[1])
archive = bytearray()
for ino, (name, mode, data) in enumerate([
    ('opt', 0o40755, b''),
    ('opt/bootlocal.sh', 0o100755, (root/'opt/bootlocal.sh').read_bytes()),
    ('TRAILER!!!', 0, b''),
], 1):
    encoded = name.encode()+b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
    archive.extend(('070701'+''.join(f'{field:08x}' for field in fields)).encode())
    archive.extend(encoded)
    archive.extend(b'\0' * (-len(archive) % 4))
    archive.extend(data)
    archive.extend(b'\0' * (-len(archive) % 4))
pathlib.Path(sys.argv[2]).write_bytes(gzip.compress(archive, mtime=0))
PY
cat > "$dep/hyperv-isolinux.cfg" <<'CFG'
DEFAULT evidence
PROMPT 0
TIMEOUT 1
LABEL evidence
KERNEL /boot/vmlinuz64
INITRD /boot/corepure64.gz,/boot/hyperv-overlay.gz
APPEND console=ttyS0,115200n8 ignore_loglevel loglevel=7
CFG
# Rebuild El Torito explicitly, including the relocated ISOLINUX boot-info table.
iso_root="$dep/hyperv-iso-root"
xorriso -osirrox on -overwrite on -indev "$dep/TinyCorePure64-17.1.iso" -extract / "$iso_root"
chmod u+w "$iso_root/boot/isolinux/isolinux.cfg" "$iso_root/boot"
cp "$dep/hyperv-isolinux.cfg" "$iso_root/boot/isolinux/isolinux.cfg"
cp "$dep/hyperv-overlay.gz" "$iso_root/boot/hyperv-overlay.gz"
xorriso -as mkisofs -R -J -iso-level 3 -V TC_HV_EVIDENCE \
    -b boot/isolinux/isolinux.bin -c boot/isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -o "$dep/TinyCore-HyperV-evidence.iso" "$iso_root"
sha256sum "$dep/TinyCorePure64-17.1.iso" "$dep/hyperv-overlay.gz" "$dep/TinyCore-HyperV-evidence.iso"
