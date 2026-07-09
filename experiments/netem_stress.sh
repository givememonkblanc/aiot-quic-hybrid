#!/bin/bash
# Apply/remove the stress-load netem profile from COMPARISON_PLAN
# (delay 40ms jitter 20ms, loss 2%, rate 8Mbit) on the server Wi-Fi interface.
#
#   netem_stress.sh on|off|status [iface]   egress (downlink) only
#   netem_stress.sh bidir [iface]           egress + ingress via ifb, so the
#                                           device uplink (datagrams toward the
#                                           server) also sees loss/delay
#
# NETEM_ARGS overrides the profile, e.g.
#   NETEM_ARGS="delay 100ms 20ms loss 5% rate 8mbit" netem_stress.sh bidir
set -u
IFACE=${2:-wlp194s0}
NETEM_ARGS=${NETEM_ARGS:-"delay 40ms 20ms loss 2% rate 8mbit"}
case ${1:?usage: netem_stress.sh on|bidir|off|status [iface]} in
    on)
        # shellcheck disable=SC2086
        sudo tc qdisc replace dev "$IFACE" root netem $NETEM_ARGS
        ;;
    bidir)
        # shellcheck disable=SC2086
        sudo tc qdisc replace dev "$IFACE" root netem $NETEM_ARGS
        sudo modprobe ifb
        sudo ip link add ifb0 type ifb 2>/dev/null
        sudo ip link set ifb0 up
        sudo tc qdisc replace dev "$IFACE" handle ffff: ingress
        sudo tc filter replace dev "$IFACE" parent ffff: protocol ip u32 \
            match u32 0 0 action mirred egress redirect dev ifb0
        # shellcheck disable=SC2086
        sudo tc qdisc replace dev ifb0 root netem $NETEM_ARGS
        ;;
    off)
        sudo tc qdisc del dev "$IFACE" root 2>/dev/null
        sudo tc qdisc del dev "$IFACE" ingress 2>/dev/null
        sudo tc qdisc del dev ifb0 root 2>/dev/null
        sudo ip link set ifb0 down 2>/dev/null
        ;;
esac
tc qdisc show dev "$IFACE"
tc qdisc show dev ifb0 2>/dev/null
