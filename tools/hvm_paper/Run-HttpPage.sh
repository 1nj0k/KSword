#!/bin/sh
# A real BusyBox httpd serves one controlled resident file-cache page.
# The server is reachable only on the nested guest's loopback interface.
set -eu
dir=/tmp/page-app
bin=/tmp/paper-bin
case "$1" in
 setup)
    mkdir -p "$dir"
    "$bin/page-holder" "$dir/policy.json" 900 >"$dir/pfn.jsonl" 2>&1 &
    echo $! >"$dir/holder.pid"
    sleep 1
    kill -0 "$(cat "$dir/holder.pid")"
    "$bin/busybox" httpd -f -p 127.0.0.1:18081 -h "$dir" >"$dir/httpd.log" 2>&1 &
    echo $! >"$dir/server.pid"
    sleep 1
    kill -0 "$(cat "$dir/server.pid")"
    echo paper-http-setup
    cat /proc/sys/kernel/random/boot_id
    cat "$dir/pfn.jsonl"
    md5sum "$bin/busybox" "$bin/page-holder" "$dir/policy.json"
    ;;
 observe)
    # Start before the measured control interval; no VNC or VMM management calls
    # are needed while the Windows-side controller performs its transactions.
    echo "paper-http-identity $(cat /proc/sys/kernel/random/boot_id) $(cat /sys/devices/system/cpu/online)"
    server=$(cat "$dir/server.pid")
    i=0
    while [ "$i" -lt 180 ]; do
        rc=0
        wget -q -O "$dir/response" http://127.0.0.1:18081/policy.json || rc=$?
        # Re-read identity for every response; a startup snapshot cannot prove continuity.
        ticks=0
        if [ -r "/proc/$server/stat" ]; then ticks=$(cut -d' ' -f22 "/proc/$server/stat"); fi
        holder_alive=0
        if kill -0 "$(cat "$dir/holder.pid")" 2>/dev/null; then holder_alive=1; fi
        printf 'paper-http-sample-v2 %s %s %s %s %s %s %s %s %s\n' "$i" \
            "$(cut -d' ' -f1 /proc/uptime)" "$rc" "$(wc -c <"$dir/response")" \
            "$(md5sum "$dir/response" | cut -d' ' -f1)" "$server" "$ticks" "$holder_alive" \
            "$(cat /proc/sys/kernel/random/boot_id)"
        tail -n 1 "$dir/pfn.jsonl"
        i=$((i + 1))
        sleep 1
    done
    echo paper-http-observe-end
    ;;
 sample)
    stage="$2"
    echo "paper-http-begin $stage"
    cat /proc/sys/kernel/random/boot_id
    cat /proc/uptime
    cat "/proc/$(cat "$dir/server.pid")/stat"
    tail -n 1 "$dir/pfn.jsonl"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        rc=0
        wget -q -O "$dir/response" http://127.0.0.1:18081/policy.json || rc=$?
        printf 'paper-http-response %s %s %s ' "$stage" "$i" "$rc"
        wc -c <"$dir/response" | tr '\n' ' '
        md5sum "$dir/response"
    done
    tail -n 1 "$dir/pfn.jsonl"
    cat /proc/uptime
    echo "paper-http-end $stage"
    ;;
 stop)
    kill "$(cat "$dir/server.pid")"
    kill "$(cat "$dir/holder.pid")"
    ;;
 *) exit 2;;
esac
