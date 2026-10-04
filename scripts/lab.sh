#!/usr/bin/env bash
# Safe kernel-netem lab: two network namespaces joined by a veth pair.
# Nothing here touches your real NICs, so you cannot lock yourself out. Requires root.
#   sudo scripts/lab.sh demo      # build lab, show before/after ping, tear down
#   sudo scripts/lab.sh up|down   # manage the lab manually
# NOTE: netem shapes EGRESS. We impair veth-a, so only a->b traffic is delayed.
set -euo pipefail
cd "$(dirname "$0")/.."
A=chaos-a; B=chaos-b; BIN=$PWD/chaos

up() {
  ip netns add $A; ip netns add $B
  ip link add veth-a type veth peer name veth-b
  ip link set veth-a netns $A; ip link set veth-b netns $B
  ip -n $A addr add 10.99.0.1/24 dev veth-a; ip -n $B addr add 10.99.0.2/24 dev veth-b
  for ns in $A $B; do ip -n $ns link set lo up; done
  ip -n $A link set veth-a up; ip -n $B link set veth-b up
  echo "lab up: $A (10.99.0.1) <-> $B (10.99.0.2)"
}
down() { ip netns del $A 2>/dev/null || true; ip netns del $B 2>/dev/null || true; echo "lab removed"; }

case "${1:-}" in
  up) up ;;
  down) down ;;
  demo)
    [ -x "$BIN" ] || make
    trap down EXIT; up
    echo "== baseline =="; ip netns exec $A ping -c 5 -i 0.2 10.99.0.2 | tail -3
    echo "== applying 'lossy-wifi' for 10 s (auto-reverts) =="
    ip netns exec $A "$BIN" netem apply --dev veth-a --profile lossy-wifi --set delay=100 --ttl 10 &
    CP=$!
    sleep 1
    ip netns exec $A tc -s qdisc show dev veth-a
    ip netns exec $A ping -c 20 -i 0.2 10.99.0.2 | tail -3
    RC=0
    wait $CP || RC=$?
    echo "== after auto-revert =="; ip netns exec $A tc qdisc show dev veth-a
    if [ $RC -ne 0 ]; then
      echo "!! 'chaos netem apply' failed (exit $RC), so the 'impaired' ping above was NOT impaired."
      echo "!! Common cause: kernel lacks netem. Try: sudo modprobe sch_netem   (and: ip -V; tc -V)"
      exit $RC
    fi
    ;;
  *) echo "usage: sudo $0 demo|up|down"; exit 2 ;;
esac
