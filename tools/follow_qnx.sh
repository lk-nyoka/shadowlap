#!/bin/sh
# Stream a QNX run to this PC while it happens, so tools/live.py can show it.
#
#   sh tools/follow_qnx.sh <qnx-ip> <scenario> [remote_dir] [user]
#   e.g.  sh tools/follow_qnx.sh 192.168.122.50 shadow
#
# Then in another window:  python3 tools/live.py out-qnx
# and start the run on QNX: sh run_demo.sh shadow out
#
# It waits for the run to start, copies each new line of compare.csv over ssh
# into out-qnx/<scenario>/compare.csv, and stops with Ctrl+C.
set -u
IP=${1:?usage: follow_qnx.sh <qnx-ip> <scenario> [remote_dir] [user]}
SCEN=${2:?usage: follow_qnx.sh <qnx-ip> <scenario> [remote_dir] [user]}
RDIR=${3:-/data/home/root/shadowlap}
RUSER=${4:-root}
LOCAL=out-qnx/$SCEN
mkdir -p "$LOCAL"
REMOTE=$RDIR/out/$SCEN/compare.csv

echo "Waiting for $REMOTE on $IP (start the run on QNX now) ..."
# Remote side: wait until the file is new or changed (a fresh run), then follow it.
ssh -o ServerAliveInterval=5 "$RUSER@$IP" "
  f='$REMOTE'
  if [ -f \"\$f\" ]; then old=\$(ls -l \"\$f\"); else old=''; fi
  while :; do
    if [ -f \"\$f\" ] && [ \"\$(ls -l \"\$f\")\" != \"\$old\" ]; then break; fi
    sleep 1
  done
  exec tail -n +1 -f \"\$f\"
" > "$LOCAL/compare.csv"
