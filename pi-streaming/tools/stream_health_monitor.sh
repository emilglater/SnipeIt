#!/bin/bash
# End-to-end stream health monitor. Samples every 2 s:
#  - FFmpeg relay: fps + speed from /tmp/ffmpeg.log (speed<1x = relay lagging)
#  - mediaMTX: cumulative "write queue is full" count
#  - RTSP client TCP socket (:8554): Send-Q, retransmits, cwnd, rtt
#  - AP radio: per-station tx-failed delta
# Usage: sudo bash stream_health_monitor.sh [duration_s] [logfile]
DUR=${1:-240}
LOG=${2:-/tmp/stream_health.log}
RAW=${LOG%.log}_ss_raw.log

declare -A prev_tx prev_fail
prev_retrans=0

: > "$RAW"
echo "time,ff_fps,ff_speed,mtx_wqfull_total,rtsp_client,sendq,cwnd,rtt_ms,retrans_delta,sta_txpkts,sta_txfail,ping_ms" > "$LOG"
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
    ts=$(date +%H:%M:%S)

    # FFmpeg: last progress blob (log uses \r)
    ffline=$(tail -c 1500 /tmp/ffmpeg.log 2>/dev/null | tr '\r' '\n' | grep 'fps=' | tail -1)
    ff_fps=$(echo "$ffline"   | grep -o 'fps= *[0-9.]*'   | grep -o '[0-9.]*$')
    ff_speed=$(echo "$ffline" | grep -o 'speed= *[0-9.]*x' | grep -o '[0-9.]*')
    wq=$(grep -c 'write queue is full' /tmp/mediamtx.log 2>/dev/null)

    # RTSP client socket: any ESTAB on :8554 to a non-localhost peer.
    # Full dump goes to $RAW (snd_wnd/rcv_wnd tell zero-window from radio-dead).
    ss -tinm state established "( sport = :8554 )" 2>/dev/null \
        | sed "s/^/$ts /" >> "$RAW"
    sock=$(ss -tin state established "( sport = :8554 )" 2>/dev/null \
           | grep -A1 '10\.42\.1\.\|192\.168\.' | head -2)
    peer=$(echo "$sock" | head -1 | awk '{print $4}')
    sendq=$(echo "$sock" | head -1 | awk '{print $2}')
    cwnd=$(echo "$sock"   | grep -o 'cwnd:[0-9]*'        | cut -d: -f2)
    rtt=$(echo "$sock"    | grep -o 'rtt:[0-9.]*'        | cut -d: -f2)
    retrans=$(echo "$sock"| grep -o 'retrans:[0-9]*/[0-9]*' | cut -d/ -f2)
    retrans=${retrans:-$prev_retrans}
    d_re=$(( retrans - prev_retrans )); prev_retrans=$retrans

    # radio: first (only) station on uap0
    read -r mac tp tf < <(iw dev uap0 station dump 2>/dev/null | awk '
        /^Station/ {mac=$2} /tx packets:/ {tp=$3} /tx failed:/ {print mac, tp, $3; exit}')
    d_tp=""; d_tf=""
    if [ -n "$mac" ]; then
        d_tp=$(( tp - ${prev_tx[$mac]:-tp} )); d_tf=$(( tf - ${prev_fail[$mac]:-tf} ))
        prev_tx[$mac]=$tp; prev_fail[$mac]=$tf
    fi

    # ping the station's IP (from the peer if present, else the known lease)
    pip=${peer#*[}; pip=${pip%%]*}; pip=${pip##*:}
    [ -z "$pip" ] || [ "$pip" = "-" ] && pip=$(awk '{print $3; exit}' /var/lib/NetworkManager/dnsmasq-uap0.leases 2>/dev/null)
    ping_ms="-"
    if [ -n "$pip" ]; then
        r=$(ping -c1 -W1 "$pip" 2>/dev/null | grep -o 'time=[0-9.]*' | cut -d= -f2)
        ping_ms=${r:-LOSS}
    fi

    echo "$ts,${ff_fps:--},${ff_speed:--},${wq:-0},${peer:--},${sendq:--},${cwnd:--},${rtt:--},$d_re,${d_tp:--},${d_tf:--},$ping_ms" >> "$LOG"
    sleep 2
done
echo "done: $LOG"
