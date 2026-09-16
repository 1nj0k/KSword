#!/bin/sh
# Six same-source workloads. /tmp is RAM-backed, so do not label it disk I/O.
set -u
repetitions=${1:-7}
limit=${2:-180}
batch=${3:-linux}
case "$repetitions:$limit:$batch" in *[!0-9A-Za-z:_-]*) exit 2;; esac
dir=/tmp/ksword-paper-results-$batch
mkdir -p "$dir"
binary=/tmp/microbench-linux
# The stock TinyCore image supplies MD5; the ISO manifest separately records SHA256.
hash=$(md5sum "$binary" | cut -d' ' -f1)
boot=$(cat /proc/sys/kernel/random/boot_id)
i=0
while [ "$i" -le "$repetitions" ]; do
    # Rotate workload order deterministically; i=0 is the warmup block.
    case $((i % 3)) in
        0) work='cpu cpuid memory latency net ping';;
        1) work='latency net ping cpu cpuid memory';;
        2) work='ping cpu cpuid memory latency net';;
    esac
    for w in $work; do
        id="$batch-$w-$i"
        file="$dir/$id.jsonl"
        u=$(cut -d' ' -f1 /proc/uptime)
        printf '{"kind":"run-start","runId":"%s","workload":"%s","iteration":%s,"bootId":"%s","guestUptime":%s,"binaryMD5":"%s"}\n' "$id" "$w" "$i" "$boot" "$u" "$hash" >"$file"
        timeout -s KILL "$limit" "$binary" "$w" >>"$file" 2>&1
        rc=$?
        u=$(cut -d' ' -f1 /proc/uptime)
        printf '{"kind":"run-end","runId":"%s","exitCode":%s,"guestUptime":%s}\n' "$id" "$rc" "$u" >>"$file"
        sync
        # A copy of every per-run file is transmitted to the durable host serial log.
        printf '\npaper-linux-run-begin %s\n' "$id" >/dev/ttyS0
        cat "$file" >/dev/ttyS0
        printf 'paper-linux-run-end %s\n' "$id" >/dev/ttyS0
    done
    i=$((i + 1))
done
echo paper-linux-bench-complete >/dev/ttyS0
