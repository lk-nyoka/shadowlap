#!/bin/sh
# One command that checks the whole project, again and again.
#
#   sh scripts/check_all.sh                 # 3 rounds, on this PC (Linux / WSL)
#   ROUNDS=10 sh scripts/check_all.sh       # more rounds
#   CLONE=1 sh scripts/check_all.sh         # first take a fresh copy from GitHub
#   STRICT_TIMING=1 sh scripts/check_all.sh # a v1 deadline miss fails the check
#                                           # (use on a quiet machine; WSL and cloud
#                                           #  VMs pause now and then, QNX is the judge)
#
# Once:      build from clean, C unit tests, gate tests, monitor pin, licence,
#            tamper tests (a changed monitor or broken evidence must be INVALID).
# Each round (seed = round number):
#            600-lap sweeps: v2 and v3 must FAIL, v2.1 must PASS_WITHIN_SCOPE;
#            seed 1 must reproduce the published numbers exactly, and twice in a row;
#            live scenarios on this PC (shadow, crash, freeze, sigstop, flood,
#            and attack when run as root): evidence valid, v1 never breaks a limit,
#            the candidate's faults are seen, every attack route blocked;
#            the HTML report builds.
# Writes out/check/check_report.md and exits non-zero if anything failed.

set -u
ROUNDS=${ROUNDS:-3}
SECS=${SECS:-20}
LAP=${LAP:-data/lap_openf1_monza_2024_4.csv}
REPO_URL=${REPO_URL:-https://github.com/lk-nyoka/shadowlap.git}
PY=${PY:-python3}
[ -x .venv/bin/python ] && PY=.venv/bin/python

if [ "${CLONE:-0}" = 1 ]; then
    tmp=$(mktemp -d)
    echo "== fresh copy from $REPO_URL into $tmp"
    git clone -q "$REPO_URL" "$tmp/shadowlap" || { echo "clone failed"; exit 1; }
    cp -r data/*.csv "$tmp/shadowlap/data/" 2>/dev/null    # local laps not in git
    cd "$tmp/shadowlap" || exit 1
    PY=python3
fi

# The real Monza lap is not in the repo (OpenF1 licence): fetch it if missing,
# else fall back to the synthetic lap that is committed.
MONZA_SHA=d96c26706bed1d25a4be14f5920efd6088c83592cd9b8055c4ba538b12d2eedd
if [ ! -s "$LAP" ]; then
    echo "== $LAP missing: fetching it from OpenF1"
    "$PY" tools/fetch_openf1_lap.py --year 2024 --circuit Monza --session Qualifying >/dev/null 2>&1
fi
LAPNOTE=""
if [ ! -s "$LAP" ]; then
    LAPNOTE="real lap unavailable (no network?); used the synthetic lap data/lap_a.csv"
    [ -s data/lap_a.csv ] || "$PY" tools/make_synthetic_lap.py >/dev/null 2>&1
    LAP=data/lap_a.csv
fi
LAPSHA=$(sha256sum "$LAP" | cut -c1-64)

OUT=out/check
rm -rf "$OUT"; mkdir -p "$OUT"
REP="$OUT/check_report.md"
PASS=0; FAIL=0; WARN=0
{
    echo "# Shadow Lap full check"
    echo
    echo "Started $(date -u '+%Y-%m-%d %H:%M UTC') on $(uname -sr), $(nproc 2>/dev/null || echo ?) CPUs, $ROUNDS rounds, user $(id -un)."
    echo "Commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout'). Lap: $LAP (sha256 $(echo $LAPSHA | cut -c1-12))${LAPNOTE:+. **Note: $LAPNOTE**}"
    echo
    echo "| Round | Check | Result | Detail |"
    echo "|---|---|---|---|"
} > "$REP"

ok()   { PASS=$((PASS+1)); echo "  PASS  $2${3:+  ($3)}"; echo "| $1 | $2 | PASS | ${3:-} |" >> "$REP"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $2${3:+  ($3)}"; echo "| $1 | $2 | **FAIL** | ${3:-} |" >> "$REP"; }
warn() { WARN=$((WARN+1)); echo "  warn  $2${3:+  ($3)}"; echo "| $1 | $2 | warn | ${3:-} |" >> "$REP"; }
check() { r=$1; name=$2; shift 2; if "$@" > "$OUT/last.log" 2>&1; then ok "$r" "$name"; else bad "$r" "$name" "$(tail -1 "$OUT/last.log" | tr '|' '/')"; fi; }

# read one value from a JSON file: j file 'd["a"]["b"]'
j() { "$PY" -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null; }

echo "== once"
check - "build from clean (make clean && make)" sh -c 'make clean >/dev/null && make'
check - "C unit tests (make test)" make test
check - "gate tests (tests/test_gate.py)" "$PY" tests/test_gate.py
check - "safety monitor matches its pinned hash" "$PY" -c "import sys; sys.path.insert(0,'tools'); import gate; sys.exit(gate.monitor_sha256() != gate.pinned_monitor())"
check - "LICENSE present (MIT)" grep -q "MIT License" LICENSE
check - "README states QNX version, target, toolchain" sh -c 'grep -q "QNX SDP 8.0" README.md && grep -qi "QEMU" README.md && grep -q "qcc" README.md'
check - "lap file present ($LAP)" test -s "$LAP"
[ -n "$LAPNOTE" ] && warn - "real Monza lap" "$LAPNOTE"

# tamper tests: a changed monitor, or missing evidence, must never pass
build/host/bbw_sweep --lap "$LAP" --runs 20 --laps 1 --candidate v2.1 --out "$OUT/tamper" >/dev/null 2>&1
check - "tamper: control run with the real pin -> PASS" "$PY" tools/gate.py "$OUT/tamper/summary.json" --expect PASS_WITHIN_SCOPE
echo "0000000000000000000000000000000000000000000000000000000000000000" > "$OUT/wrong.pin"
check - "tamper: monitor hash differs from pin -> INVALID" sh -c "BBW_MONITOR_PIN=$OUT/wrong.pin $PY tools/gate.py $OUT/tamper/summary.json --expect INVALID"
check - "tamper: empty evidence file -> INVALID" sh -c ": > $OUT/empty.json; $PY tools/gate.py $OUT/empty.json --expect INVALID"

r=1
while [ "$r" -le "$ROUNDS" ]; do
    echo "== round $r of $ROUNDS (seed $r)"
    for c in v2 v3 v2.1; do
        d="$OUT/r$r/sweep-$c"
        build/host/bbw_sweep --lap "$LAP" --runs 200 --laps 3 --seed "$r" --candidate "$c" --out "$d" >/dev/null 2>&1
        n=$(j "$d/summary.json" 'd["laps_with_unsafe_v2"]')
        v1b=$(j "$d/summary.json" 'd["v1_limit_break_frames"]')
        case $c in v2.1) want=PASS_WITHIN_SCOPE ;; *) want=FAIL ;; esac
        if "$PY" tools/gate.py "$d/summary.json" --expect "$want" > "$d/gate.txt" 2>&1; then
            ok "$r" "600 laps, $c -> $want" "$n/600 laps with a limit broken"
        else
            bad "$r" "600 laps, $c -> $want" "$n/600 laps with a limit broken; see $d/gate.txt"
        fi
        [ "$v1b" = 0 ] && ok "$r" "600 laps, $c: v1 breaks no limit" || bad "$r" "600 laps, $c: v1 breaks no limit" "v1 limit-break frames: $v1b"
    done
    if [ "$r" = 1 ]; then
        if [ "$LAPSHA" != "$MONZA_SHA" ]; then
            warn 1 "seed 1 reproduces published numbers" "skipped: this lap file is not the one the README numbers came from"
        else
        got="$(j "$OUT/r1/sweep-v2/summary.json" 'd["laps_with_unsafe_v2"]') $(j "$OUT/r1/sweep-v3/summary.json" 'd["laps_with_unsafe_v2"]') $(j "$OUT/r1/sweep-v2.1/summary.json" 'd["laps_with_unsafe_v2"]') $(j "$OUT/r1/sweep-v3/summary.json" 'round(d["worst_extra_speed_kmh"]["v2"],1) if isinstance(d["worst_extra_speed_kmh"],dict) else round(d["worst_extra_speed_kmh"],1)')"
            [ "$got" = "600 304 0 13.8" ] && ok 1 "seed 1 reproduces published numbers" "$got" || bad 1 "seed 1 reproduces published numbers" "got '$got', README says '600 304 0 13.8'"
        fi
        build/host/bbw_sweep --lap "$LAP" --runs 200 --laps 3 --seed 1 --candidate v3 --out "$OUT/r1/again" >/dev/null 2>&1
        same=$("$PY" -c "
import json,sys
a,b=(json.load(open(p)) for p in sys.argv[1:])
for k in ('generated','created','time','elapsed_s','out'): a.pop(k,None); b.pop(k,None)
print('yes' if a==b else 'no')" "$OUT/r1/sweep-v3/summary.json" "$OUT/r1/again/summary.json")
        [ "$same" = yes ] && ok 1 "same seed twice gives identical results" || bad 1 "same seed twice gives identical results"
    fi

    scen="shadow crash freeze sigstop flood"
    if [ "$(id -u)" = 0 ]; then scen="$scen attack"; fi
    for s in $scen; do
        d="$OUT/r$r/$s"
        if [ "$s" = attack ]; then
            V2_USER=${V2_USER:-nobody} SECONDS_MAX=$SECS LAP=$LAP sh scripts/run_demo.sh "$s" "$OUT/r$r" > "$OUT/r$r/$s.console" 2>&1
        else
            SECONDS_MAX=$SECS LAP=$LAP sh scripts/run_demo.sh "$s" "$OUT/r$r" > "$OUT/r$r/$s.console" 2>&1
        fi
        f="$d/summary.json"
        if grep -q "ERROR" "$OUT/r$r/$s.console"; then
            bad "$r" "scenario $s: no ERROR lines" "$(grep -m1 ERROR "$OUT/r$r/$s.console" | tr '|' '/')"
        else
            ok "$r" "scenario $s: no ERROR lines"
        fi
        if [ ! -s "$f" ]; then bad "$r" "scenario $s: ran" "no summary.json; see $OUT/r$r/$s.console"; continue; fi
        if [ "$(j "$f" 'd["valid_evidence"]')" = True ]; then
            ok "$r" "scenario $s: evidence valid"
        elif [ "$(j "$f" 'len(d["invalid_because"]) > 0 and all("real-time scheduling" in x for x in d["invalid_because"])')" = True ] \
             && [ "$(id -u)" != 0 ] && [ "${STRICT_TIMING:-0}" != 1 ]; then
            # Normal users on Linux can't get real-time priority. The run correctly
            # marks its timing evidence INVALID (fail-closed); that is expected here.
            warn "$r" "scenario $s: evidence marked INVALID, as it should be" "no real-time priority for a normal user on Linux; run with sudo for real-time"
        else
            bad "$r" "scenario $s: evidence valid" "$(j "$f" 'd["invalid_because"]')"
        fi
        [ "$(j "$f" 'sum(d["v1_limit_breaks"].values())')" = 0 ] && ok "$r" "scenario $s: v1 breaks no limit" || bad "$r" "scenario $s: v1 breaks no limit"
        [ "$(j "$f" 'd["actuator_rejects"]')" = 0 ] && ok "$r" "scenario $s: brakes accepted every v1 command" || bad "$r" "scenario $s: brakes accepted every v1 command"
        miss=$(j "$f" 'd["v1_deadline_misses"]'); worst=$(j "$f" 'd["v1_latency_us"]["max"]')
        if [ "$miss" = 0 ]; then ok "$r" "scenario $s: v1 0 deadline misses" "worst ${worst} µs"
        elif [ "${STRICT_TIMING:-0}" = 1 ]; then bad "$r" "scenario $s: v1 0 deadline misses" "$miss misses, worst ${worst} µs"
        else warn "$r" "scenario $s: v1 deadline misses on this host" "$miss misses, worst ${worst} µs; host pauses, judge timing on QNX"; fi
        cov=$(j "$f" 'd["v2_coverage"]'); miss2=$(j "$f" 'd["classes"]["shadow_missing"]')
        case $s in
            crash|freeze|sigstop)
                [ "${miss2:-0}" -gt 0 ] 2>/dev/null && ok "$r" "scenario $s: candidate's missing answers counted" "coverage $cov" \
                    || bad "$r" "scenario $s: candidate's missing answers counted" "shadow_missing=$miss2" ;;
            attack)
                nb=$(grep -c "ATTACK NOT BLOCKED" "$d/v2.log" 2>/dev/null); b=$(grep -c "ATTACK BLOCKED" "$d/v2.log" 2>/dev/null)
                [ "${nb:-0}" = 0 ] && [ "${b:-0}" -ge 6 ] && ok "$r" "attack: every route blocked" "$b blocked" || bad "$r" "attack: every route blocked" "$b blocked, $nb not blocked"
                grep -q "BUDGET EXCEEDED" "$d/guard.log" 2>/dev/null && ok "$r" "attack: guard stopped the CPU burn" "$(grep -o 'ran hot for [0-9]* ms' "$d/guard.log" | head -1)" \
                    || bad "$r" "attack: guard stopped the CPU burn" ;;
        esac
    done
    if "$PY" -c "import numpy, matplotlib" 2>/dev/null; then
        check "$r" "report builds (tools/report.py)" sh -c "$PY tools/report.py $OUT/r$r && test -s $OUT/r$r/report.html"
    else
        warn "$r" "report not built" "numpy/matplotlib missing: python3 -m venv .venv && .venv/bin/pip install numpy matplotlib"
    fi
    r=$((r+1))
done

{
    echo
    echo "**$PASS passed, $FAIL failed, $WARN warnings.** Finished $(date -u '+%H:%M UTC')."
} >> "$REP"
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" out 2>/dev/null   # don't leave root-owned files behind
echo
echo "== $PASS passed, $FAIL failed, $WARN warnings. Report: $REP"
[ "$FAIL" = 0 ]
