#!/bin/bash
# Apply/remove the stress-load netem profile from COMPARISON_PLAN
# (delay 40ms jitter 20ms, loss 2%, rate 8Mbit) on the server Wi-Fi interface.
# Usage: netem_stress.sh on|off|status [iface]
set -u
IFACE=${2:-wlp194s0}
case ${1:?usage: netem_stress.sh on|off|status [iface]} in
    on)
        sudo tc qdisc replace dev "$IFACE" root netem delay 40ms 20ms loss 2% rate 8mbit
        ;;
    off)
        sudo tc qdisc del dev "$IFACE" root 2>/dev/null
        ;;
esac
tc qdisc show dev "$IFACE"
