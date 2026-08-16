#!/bin/bash
# Hotspot link monitor: per-station TX-failure deltas + ping RTT/loss per DHCP client.
# Usage: sudo bash hotspot_monitor.sh [duration_seconds] [logfile]
DUR=${1:-120}
LOG=${2:-/tmp/hotspot_monitor.log}
LEASES=/var/lib/NetworkManager/dnsmasq-uap0.leases

declare -A prev_tx prev_fail

echo "time,mac,name,ip,tx_pkts_delta,tx_fail_delta,ping_ms" > "$LOG"
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
    ts=$(date +%H:%M:%S)
    while read -r mac tp tf; do
        d_tp=$(( tp - ${prev_tx[$mac]:-tp} ))
        d_tf=$(( tf - ${prev_fail[$mac]:-tf} ))
        prev_tx[$mac]=$tp
        prev_fail[$mac]=$tf
        entry=$(awk -v m="$mac" 'tolower($2)==tolower(m){print $4","$3}' "$LEASES" 2>/dev/null)
        name=${entry%%,*}; ip=${entry##*,}
        ping_ms="-"
        if [ -n "$ip" ] && [ "$ip" != "$name" ]; then
            r=$(ping -c1 -W1 "$ip" 2>/dev/null | grep -o 'time=[0-9.]*' | cut -d= -f2)
            ping_ms=${r:-LOSS}
        fi
        echo "$ts,$mac,${name:-?},${ip:-?},$d_tp,$d_tf,$ping_ms" >> "$LOG"
    done < <(iw dev uap0 station dump | awk '
        /^Station/ {mac=$2}
        /tx packets:/ {tp=$3}
        /tx failed:/ {print mac, tp, $3}')
    sleep 1
done
echo "done: $LOG"
