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
#   attack    a hostile v2 tries every route to the trusted side: the brakes,
#             v1's memory, the evidence, its own inputs, signalling v1,
#             out-ranking v1, moving onto v1's CPU (each reported separately),
#             then burns v1's CPU for 30 s at the highest priority it can get
#             (the guard should stop it within about a second)
#   all       every scenario above, one after another
#   stop      end a run that is in progress (kills the programs it started)
#
# Optional environment:
#   BIN=dir           where the bbw_* programs are (default: script folder, then build/host)
#   LAP=file          lap CSV (default data/lap_a.csv)
#   SECONDS_MAX=n     stop after n seconds (default: whole lap)
#   START_DELAY_MS=n  how long the car waits before rolling (default 2500)
#   CANDIDATE=v2.1    which candidate runs in the shadow: v2 (default, flawed),
#                     v2.1 (v2 fixed) or v3 (different idea, blending flaw)
#   PLANT_ARGS="..."  conditions for the car, e.g. "--soc0 0.7 --ambient 40 --grip 0.9 --laps 3"
#   PROD_CPU=n        pin plant and v1 to CPU n
#   SHADOW_CPU=n      pin v2, comparer and hog to CPU n
#   HOG_PRIO=n        priority of the CPU burners (default 10 = shadow level; 0 = normal)
#   V2_USER=name      run v2 as this user (QNX: qnxuser; Linux as root: nobody) so it
#                     cannot open the production region or the actuator
#   GUARD_PCT=n       CPU budget for the candidate, % of one CPU over 1 s (default 40);
#                     bbw_guard stops a candidate that goes over it
#   REQUIRE_RT=0|1    1 = the plant and v1 refuse to run without real-time
#                     scheduling, CPU pinning and locked memory (default 1 on
#                     QNX, 0 elsewhere). A timing result is only evidence if 1.

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

CAND="--candidate ${CANDIDATE:-v2}"
if [ "$(uname -s)" = "QNX" ]; then REQ=${REQUIRE_RT:-1}; else REQ=${REQUIRE_RT:-0}; fi

pin() { [ -n "${1:-}" ] && echo "--cpu $1"; }
PP=$(pin "${PROD_CPU:-}")
SP=$(pin "${SHADOW_CPU:-}")
MAXA=""
[ -n "${SECONDS_MAX:-}" ] && MAXA="--max-seconds $SECONDS_MAX"

# exec, so that "$!" after "as_v2 ... &" is v2's own process id. bbw_runas
# drops to V2_USER and then becomes v2 (same process id); QNX's `on -u` and
# Linux's `runuser` would start v2 as a child, and the script would then stop,
# signal and budget the launcher instead of v2.
as_v2() {
    if [ -n "${V2_USER:-}" ]; then
        exec "$BIN/bbw_runas" "$V2_USER" "$@"
    else
        exec "$@"
    fi
}

# an outside CPU budget for the candidate (see src/guard.c)
GUARD_PCT=${GUARD_PCT:-40}
guard_v2() {
    BBW_REQUIRE_RT=$REQ "$BIN/bbw_guard" --pid "$1" ${V2_USER:+--user "$V2_USER"} --budget-pct "$GUARD_PCT" > "$out/guard.log" 2>&1 &
    PIDS="$PIDS $!"
}

# frame number where the first braking zone starts (100 frames per second).
# Asked of the plant itself so the target needs no awk.
first_brake_seq() {
    "$BIN/bbw_plant" --lap "$LAP" --first-brake
}

# Whole seconds only: some minimal shells' sleep does not take fractions.
START_DELAY_MS=${START_DELAY_MS:-2500}

cleanup() {
    for p in ${PIDS:-}; do kill -9 "$p" 2>/dev/null; done
    PIDS=""
}

run_one() {
    s=$1
    out=$OUTROOT/$s
    mkdir -p "$out"
    rm -f "$out/events.log"                # start each run's event log fresh
    echo "=== scenario: $s  ->  $out"
    PIDS=""

    BBW_REQUIRE_RT=$REQ "$BIN/bbw_plant" --lap "$LAP" $MAXA $PP --start-delay-ms $START_DELAY_MS ${PLANT_ARGS:-} > "$out/plant.log" 2>&1 &
    PLANT=$!
    PIDS="$PIDS $PLANT"
    mkdir -p "$OUTROOT" && echo "$$ $PLANT" > "$OUTROOT/.pids"
    sleep 1
    BBW_REQUIRE_RT=$REQ "$BIN/bbw_v1" $PP > "$out/v1.log" 2>&1 &
    V1=$!
    PIDS="$PIDS $V1"
    # baseline has no candidate; attack mode answers no frames; flood starves the
    # candidate on purpose (it tests that v1 survives an overloaded shadow side)
    EXPECT=--expect-candidate; case $s in baseline|attack|flood) EXPECT= ;; esac
    "$BIN/bbw_comparer" --out "$out" $SP $EXPECT > "$out/comparer.log" 2>&1 &
    COMP=$!
    PIDS="$PIDS $COMP"

    FB=$(first_brake_seq)
    case $s in
        baseline) ;;
        shadow)   as_v2 "$BIN/bbw_v2" $SP $CAND > "$out/v2.log" 2>&1 & PIDS="$PIDS $!"; guard_v2 $! ;;
        crash)    as_v2 "$BIN/bbw_v2" $SP $CAND --crash-at $((FB + 50)) > "$out/v2.log" 2>&1 & PIDS="$PIDS $!"; guard_v2 $! ;;
        freeze)   as_v2 "$BIN/bbw_v2" $SP $CAND --freeze-at "$FB" --freeze-ms 2000 > "$out/v2.log" 2>&1 & PIDS="$PIDS $!"; guard_v2 $! ;;
        sigstop)
            as_v2 "$BIN/bbw_v2" $SP $CAND > "$out/v2.log" 2>&1 &
            V2=$!
            PIDS="$PIDS $V2"
            guard_v2 $V2
            # the lap starts ~2.5 s after launch; stop v2 as the car reaches the first braking zone
            ( sleep $((FB / 100 + 3)); kill -STOP $V2 && echo "[demo] SIGSTOP v2 (pid $V2)" >> "$out/events.log";
              sleep 2; kill -CONT $V2 && echo "[demo] SIGCONT v2" >> "$out/events.log" ) &
            PIDS="$PIDS $!"
            ;;
        flood)
            as_v2 "$BIN/bbw_v2" $SP $CAND > "$out/v2.log" 2>&1 &
            PIDS="$PIDS $!"
            guard_v2 $!
            "$BIN/bbw_hog" --threads 4 --seconds 120 --prio "$HOGP" $SP > "$out/hog.log" 2>&1 &
            PIDS="$PIDS $!"
            ;;
        attack)
            sleep 2
            ( as_v2 "$BIN/bbw_v2" --attack --v1-pid "$V1" ${PROD_CPU:+--v1-cpu $PROD_CPU} --hog-seconds 30 ) > "$out/v2.log" 2>&1 &   # subshell: as_v2 uses exec
            ATK=$!
            guard_v2 $ATK
            wait $ATK
            echo "attack exit code: $?" >> "$out/v2.log"
            # a candidate that started tried at least one route (the guard may
            # kill it mid-attack, before its summary line: that is a success)
            if ! grep -q "ATTACK" "$out/v2.log"; then
                echo "    ERROR: the candidate never ran its attack, so this run tests nothing:"
                head -2 "$out/v2.log" | sed 's/^/      /'
            fi
            ;;
        *) echo "unknown scenario $s"; cleanup; return 1 ;;
    esac

    echo "$$ $PIDS" > "$OUTROOT/.pids"     # lets "run_demo.sh stop" end a run from outside
    wait $PLANT
    PLANT_RC=$?
    if [ "$PLANT_RC" != 0 ]; then
        echo "    ERROR: the car model (bbw_plant) stopped with an error:"
        grep -v "^\[plant\] warning" "$out/plant.log" | tail -3 | sed 's/^/      /'
        cleanup
        return 1
    fi
    # the comparer finishes just after the lap; never wait for it forever
    ( sleep 20; kill $COMP 2>/dev/null ) & WD=$!
    wait $COMP 2>/dev/null
    kill $WD 2>/dev/null
    cleanup
    cat "$out/plant.log" "$out/comparer.log" | grep -E "done|not valid|ERROR|failed" | sed 's/^/    /'
    # if started with sudo, give the results back to the normal user
    [ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$OUTROOT" 2>/dev/null
    cat "$out"/v2.log "$out"/events.log "$out"/guard.log 2>/dev/null | grep -E "ATTACK|attack:|note:|injected|SIG|BUDGET|containment|within budget" | sed "s/^/    /"
    return 0
}

trap 'cleanup; exit 1' INT TERM HUP

if [ "$SCEN" = "stop" ]; then
    # end a run started by another shell (used by the dashboard's Stop button)
    [ -f "$OUTROOT/.pids" ] && kill -9 $(cat "$OUTROOT/.pids") 2>/dev/null
    rm -f "$OUTROOT/.pids"
    echo "stopped"
    exit 0
fi

if [ "$SCEN" = "all" ]; then
    for s in baseline shadow crash freeze sigstop flood attack; do run_one $s; done
else
    run_one "$SCEN"
fi
