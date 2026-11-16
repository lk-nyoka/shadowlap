#!/bin/sh
# Run one demo scenario. Works on QNX (ksh) and Linux (sh/bash).
#
#   sh run_demo.sh <scenario> [out_dir]
#
# Scenarios:
#   baseline  plant + v1 only (no shadow) - the timing reference
#   shadow    plant + v1 + v2 shadowing a full lap
#   crash     v2 crashes partway through the lap
#   freeze    v2 hangs for 2 s right as the car brakes for the first chicane
#   sigstop   v2 is stopped from outside (SIGSTOP) for 2 s, then resumed
#   flood     v2 plus CPU-burning threads at shadow priority
#   attack    v2 tries to command the brakes directly
#   all       every scenario above, one after another
#
# Optional environment:
#   BIN=dir           where the bbw_* programs are (default: script folder, then build/host)
#   LAP=file          lap CSV (default data/lap_a.csv)
#   SECONDS_MAX=n     stop after n seconds (default: whole lap)
#   PROD_CPU=n        pin plant and v1 to CPU n
#   SHADOW_CPU=n      pin v2, comparer and hog to CPU n
#   HOG_PRIO=n        priority of the CPU burners (default 10 = shadow level; 0 = normal)
#   V2_USER=name      QNX only: run v2 as this user (e.g. qnxuser) so it
#                     cannot open the production region or the actuator

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "${BIN:-}" ]; then
    if [ -x "$HERE/bbw_plant" ]; then BIN=$HERE; else BIN=$HERE/../build/host; fi
fi
LAP=${LAP:-$HERE/data/lap_a.csv}
[ -f "$LAP" ] || LAP=$HERE/../data/lap_a.csv
SCEN=${1:-shadow}
OUTROOT=${2:-out}

# CPU burners run at the shadow's priority on QNX. On Linux the kernel's
# real-time throttling (sched_rt_runtime_us) pauses ALL real-time threads,
# v1 included, once real-time work uses 95% of a second, so a greedy
# real-time shadow can still hurt v1 there. Default to normal priority on Linux.
if [ "$(uname -s)" = "QNX" ]; then HOGP=${HOG_PRIO:-10}; else HOGP=${HOG_PRIO:-0}; fi

pin() { [ -n "${1:-}" ] && echo "--cpu $1"; }
PP=$(pin "${PROD_CPU:-}")
SP=$(pin "${SHADOW_CPU:-}")
MAXA=""
[ -n "${SECONDS_MAX:-}" ] && MAXA="--max-seconds $SECONDS_MAX"

# exec, so that "$!" after "as_v2 ... &" is v2's own process id
as_v2() {
    if [ -n "${V2_USER:-}" ] && command -v on >/dev/null 2>&1; then
        exec on -u "$V2_USER" "$@"
    else
        exec "$@"
    fi
}

# frame number where the first braking zone starts (100 frames per second).
# Asked of the plant itself so the target needs no awk.
first_brake_seq() {
    "$BIN/bbw_plant" --lap "$LAP" --first-brake
}

# Whole seconds only: some minimal shells' sleep does not take fractions.
START_DELAY_MS=2500

cleanup() {
    for p in ${PIDS:-}; do kill -9 "$p" 2>/dev/null; done
    PIDS=""
}

run_one() {
    s=$1
    out=$OUTROOT/$s
    mkdir -p "$out"
    echo "=== scenario: $s  ->  $out"
    PIDS=""

    "$BIN/bbw_plant" --lap "$LAP" $MAXA $PP --start-delay-ms $START_DELAY_MS > "$out/plant.log" 2>&1 &
    PLANT=$!
    PIDS="$PIDS $PLANT"
    sleep 1
    "$BIN/bbw_v1" $PP > "$out/v1.log" 2>&1 &
    PIDS="$PIDS $!"
    "$BIN/bbw_comparer" --out "$out" $SP > "$out/comparer.log" 2>&1 &
    COMP=$!
    PIDS="$PIDS $COMP"

    FB=$(first_brake_seq)
    case $s in
        baseline) ;;
        shadow)   as_v2 "$BIN/bbw_v2" $SP > "$out/v2.log" 2>&1 & PIDS="$PIDS $!" ;;
        crash)    as_v2 "$BIN/bbw_v2" $SP --crash-at $((FB + 50)) > "$out/v2.log" 2>&1 & PIDS="$PIDS $!" ;;
        freeze)   as_v2 "$BIN/bbw_v2" $SP --freeze-at "$FB" --freeze-ms 2000 > "$out/v2.log" 2>&1 & PIDS="$PIDS $!" ;;
        sigstop)
            as_v2 "$BIN/bbw_v2" $SP > "$out/v2.log" 2>&1 &
            V2=$!
            PIDS="$PIDS $V2"
            # the lap starts ~2.5 s after launch; stop v2 as the car reaches the first braking zone
            ( sleep $((FB / 100 + 3)); kill -STOP $V2 && echo "[demo] SIGSTOP v2 (pid $V2)" >> "$out/events.log";
              sleep 2; kill -CONT $V2 && echo "[demo] SIGCONT v2" >> "$out/events.log" ) &
            PIDS="$PIDS $!"
            ;;
        flood)
            as_v2 "$BIN/bbw_v2" $SP > "$out/v2.log" 2>&1 &
            PIDS="$PIDS $!"
            "$BIN/bbw_hog" --threads 4 --seconds 120 --prio "$HOGP" $SP > "$out/hog.log" 2>&1 &
            PIDS="$PIDS $!"
            ;;
        attack)
            sleep 2
            ( as_v2 "$BIN/bbw_v2" --attack ) > "$out/v2.log" 2>&1     # subshell: as_v2 uses exec
            echo "attack exit code: $?" >> "$out/v2.log"
            ;;
        *) echo "unknown scenario $s"; cleanup; return 1 ;;
    esac

    wait $PLANT
    wait $COMP 2>/dev/null
    cleanup
    cat "$out/plant.log" "$out/comparer.log" | grep "done" | sed 's/^/    /'
    # if started with sudo, give the results back to the normal user
    [ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$OUTROOT" 2>/dev/null
    cat "$out"/v2.log "$out"/events.log 2>/dev/null | grep -E "ATTACK|injected|SIG" | sed "s/^/    /"
    return 0
}

trap 'cleanup; exit 1' INT TERM

if [ "$SCEN" = "all" ]; then
    for s in baseline shadow crash freeze sigstop flood attack; do run_one $s; done
else
    run_one "$SCEN"
fi
