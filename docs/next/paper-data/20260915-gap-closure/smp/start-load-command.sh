(echo paper-load-start; cat /proc/stat; for c in 1 2; do taskset $c timeout -s KILL 120 sh -c 'while :; do :; done' & done; wait; echo paper-load-end; cat /proc/stat) >/dev/ttyS0 2>&1 &
