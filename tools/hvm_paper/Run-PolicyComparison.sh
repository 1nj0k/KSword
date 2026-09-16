#!/bin/sh
# Run autonomously inside the descendant after the page-holder/httpd setup.
set -eu
# Explicit fixture selection avoids reopening or silently replacing an old pin.
dir=${POLICY_DIR:-/tmp/page-app}
bin=${POLICY_BIN:-/tmp/paper-bin}
file=${POLICY_FILE:-policy.json}
pfn=${POLICY_PFN_LOG:-pfn.jsonl}
holder_pid=${POLICY_HOLDER_PID_FILE:-holder.pid}
case "$1" in
 observe)
    echo "paper-http-identity $(cat /proc/sys/kernel/random/boot_id) $(cat /sys/devices/system/cpu/online)"
    i=0
    while [ "$i" -lt 360 ]; do
        rc=0
        wget -q -O "$dir/response" "http://127.0.0.1:18081/$file" || rc=$?
        server=$(cat "$dir/server.pid")
        ticks=0
        if [ -r "/proc/$server/stat" ]; then ticks=$(cut -d' ' -f22 "/proc/$server/stat"); fi
        alive=0
        if kill -0 "$(cat "$dir/$holder_pid")" 2>/dev/null; then alive=1; fi
        printf 'paper-http-sample-v2 %s %s %s %s %s %s %s %s %s\n' "$i" \
            "$(cut -d' ' -f1 /proc/uptime)" "$rc" "$(wc -c <"$dir/response")" \
            "$(md5sum "$dir/response" | cut -d' ' -f1)" "$server" \
            "$ticks" "$alive" "$(cat /proc/sys/kernel/random/boot_id)"
        tail -n 1 "$dir/$pfn"
        "$bin/http-policy-oracle" "$dir/response"
        i=$((i+1));sleep 1
    done
    ;;
 inplace)
    # Comparator: writes the original cache page and therefore needs saved bytes.
    # Its controls are explicitly guest-side, unlike the EPT intervention.
    test ! -e "$dir/original-for-inplace"
    cp "$dir/$file" "$dir/original-for-inplace"
    echo policy-inplace-before;md5sum "$dir/$file"
    sleep 12
    echo policy-inplace-write;cat /proc/uptime
    dd if=/dev/zero of="$dir/$file" bs=4096 count=1 conv=notrunc 2>/dev/null
    sleep 12
    echo policy-inplace-restore;cat /proc/uptime
    dd if="$dir/original-for-inplace" of="$dir/$file" bs=4096 count=1 conv=notrunc 2>/dev/null
    md5sum "$dir/$file"
    ;;
 *) exit 2;;
esac
