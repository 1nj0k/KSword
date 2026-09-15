#!/bin/sh
# Six same-source workloads. /tmp is RAM-backed, so do not label it disk I/O.
set -u
dir=/tmp/ksword-paper-results
mkdir -p "$dir"
binary=/tmp/microbench-linux
hash=$(sha256sum "$binary" | cut -d' ' -f1)
boot=$(cat /proc/sys/kernel/random/boot_id)
for i in 0 1 2 3 4 5 6 7; do
    # Rotate workload order deterministically; i=0 is the warmup block.
    case $((i % 3)) in
        0) work='cpu cpuid memory latency net ping';;
        1) work='latency net ping cpu cpuid memory';;
        2) work='ping cpu cpuid memory latency net';;
    esac
    for w in $work; do
        id="linux-$w-$i"
        file="$dir/$id.jsonl"
        u=$(cut -d' ' -f1 /proc/uptime)
        printf '{"kind":"run-start","runId":"%s","workload":"%s","iteration":%s,"bootId":"%s","guestUptime":%s,"binarySha256":"%s"}\n' "$id" "$w" "$i" "$boot" "$u" "$hash" >"$file"
        timeout -s KILL 180 "$binary" "$w" >>"$file" 2>&1
        rc=$?
        u=$(cut -d' ' -f1 /proc/uptime)
        printf '{"kind":"run-end","runId":"%s","exitCode":%s,"guestUptime":%s}\n' "$id" "$rc" "$u" >>"$file"
        sync
        # A copy of every per-run file is transmitted to the durable host serial log.
        printf '\npaper-linux-run-begin %s\n' "$id" >/dev/ttyS0
        cat "$file" >/dev/ttyS0
        printf 'paper-linux-run-end %s\n' "$id" >/dev/ttyS0
    done
done
echo paper-linux-bench-complete >/dev/ttyS0
