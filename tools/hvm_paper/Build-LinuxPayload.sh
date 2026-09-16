#!/bin/sh
# Run from the repository root in the existing Ubuntu WSL toolchain.
set -eu
dep=.deps/hvm-paper
out="$dep/payload"
mkdir -p "$out"
gcc -O2 -Wall -Wextra -Werror -static -pthread tools/hvm_paper/microbench.c -o "$out/microbench-linux"
gcc -O2 -Wall -Wextra -Werror -static tools/hvm_paper/page_holder.c -o "$out/page-holder"
gcc -O2 -Wall -Wextra -Werror -static tools/hvm_paper/http_policy_oracle.c -o "$out/http-policy-oracle"
src="$dep/busybox-1.37.0"
test -f "$src/Makefile"
make -s -C "$src" allnoconfig
# BusyBox's Kconfig retains the first occurrence; replace, never append overrides.
for option in STATIC HTTPD BUSYBOX SHOW_USAGE FEATURE_VERBOSE_USAGE FEATURE_HTTPD_RANGES FEATURE_HTTPD_LAST_MODIFIED FEATURE_IPV6 FEATURE_PREFER_IPV4_ADDRESS; do
    sed -i "s/^# CONFIG_${option} is not set/CONFIG_${option}=y/" "$src/.config"
done
yes '' | make -s -C "$src" oldconfig
make -s -C "$src" -j4
file "$src/busybox" | grep 'statically linked'
"$src/busybox" --list | grep '^httpd$'
cp "$src/busybox" "$out/busybox"
cp "$src/LICENSE" "$out/BUSYBOX-LICENSE"
cp "$src/.config" "$out/busybox.config"
cp tools/hvm_paper/Run-LinuxBenchmarks.sh tools/hvm_paper/Run-HttpPage.sh "$out/"
cp tools/hvm_paper/Run-PolicyComparison.sh "$out/"
# Keep the existing workload script's absolute path explicit and reproducible.
sha256sum "$out"/* >"$dep/payload-sha256.txt"
xorriso -as mkisofs -quiet -R -J -V PAPER_PAYLOAD -o "$dep/payload.iso" "$out"
sha256sum "$dep/payload.iso"
