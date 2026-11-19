#!/usr/bin/env bash
# Correctness tests: send files from sender to receiver and check that
# each copy is identical and both programs exit without errors.
#
# Run from the project folder after `make`:
#   bash tests/correctness.sh            (all tests)
#   QUICK=1 bash tests/correctness.sh    (skip the 10 MB and 15 MB files)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/test_output/correctness"
SRC="$WORK/src"
LOGS="$WORK/logs"
PORT_BASE=${PORT_BASE:-9300}
TIMEOUT=${TIMEOUT:-180}

rm -rf "$WORK"
mkdir -p "$SRC" "$LOGS"

# Use whichever timeout/md5 commands exist (they differ on Linux and macOS).
if command -v timeout >/dev/null 2>&1; then TO="timeout $TIMEOUT"
elif command -v gtimeout >/dev/null 2>&1; then TO="gtimeout $TIMEOUT"
else TO=""; fi
md5() { if command -v md5sum >/dev/null 2>&1; then md5sum | cut -c1-12; else command md5 -q | cut -c1-12; fi; }

[ -x "$ROOT/sender" ] && [ -x "$ROOT/receiver" ] || { echo "Run 'make' first."; exit 1; }

# Test files
make_file() {   # random file of $2 bytes (macOS `head` can't do 0 bytes)
    if [ "$2" -eq 0 ]; then : > "$SRC/$1"; else head -c "$2" /dev/urandom > "$SRC/$1"; fi
}
make_file empty.bin        0
make_file tiny.bin         100          # less than one record
make_file one_rec.bin      512          # exactly one record
make_file exact.bin        262144       # exactly 2 full blasts
make_file uneven.bin       1000003      # last record is short
make_file tail1.bin        262145       # 1 byte into a new blast
if [ -z "${QUICK:-}" ]; then
    make_file ten_mb.bin   $((10 * 1024 * 1024))
    make_file fifteen_mb.bin $((15 * 1024 * 1024 + 777))
fi

PASS=0; FAIL=0; N=0

# run_case <name> <file> <record_size> <M> <sender_loss%> [receiver_loss%]
run_case() {
    local name=$1 file=$2 rs=$3 m=$4 sdrop=$5 rdrop=${6:-0}
    local port=$((PORT_BASE + N)); N=$((N + 1))
    local out="$WORK/out_$N"; mkdir -p "$out"

    $TO "$ROOT/receiver" "$port" "$out" "$rdrop" > "$LOGS/$N-$name.rx.log" 2>&1 &
    local rpid=$!
    # wait (up to 5 s) until the receiver is listening
    for _ in $(seq 50); do grep -q listening "$LOGS/$N-$name.rx.log" 2>/dev/null && break; sleep 0.1; done
    $TO "$ROOT/sender" 127.0.0.1 "$port" "$SRC/$file" "$rs" "$m" "$sdrop" \
        > "$LOGS/$N-$name.tx.log" 2>&1
    local sstat=$?
    wait "$rpid"; local rstat=$?

    local verdict="PASS" why=""
    if [ $sstat -ne 0 ]; then verdict="FAIL"; why="sender exit $sstat"; fi
    if [ $rstat -ne 0 ]; then verdict="FAIL"; why="$why receiver exit $rstat"; fi
    if ! cmp -s "$SRC/$file" "$out/$file"; then verdict="FAIL"; why="$why file differs"; fi

    if [ "$verdict" = PASS ]; then
        PASS=$((PASS + 1))
        why="md5 $(md5 < "$out/$file")"
    else
        FAIL=$((FAIL + 1))
    fi
    printf "%-4s %-28s %-15s rs=%-4s M=%-4s loss=%s%%/%s%%  %s\n" \
        "$verdict" "$name" "$file" "$rs" "$m" "$sdrop" "$rdrop" "$why"
    rm -rf "$out"
}

echo "== Edge-case file sizes (0% loss) =="
run_case empty_file      empty.bin    512 256 0
run_case smaller_than_rec tiny.bin    512 256 0
run_case exactly_one_rec one_rec.bin  512 256 0
run_case exact_blasts    exact.bin    512 256 0
run_case uneven_tail     uneven.bin   512 256 0
run_case one_past_blast  tail1.bin    512 256 0

echo "== Record sizes and blast sizes =="
for rs in 256 512 1024; do run_case "rec_size_$rs" uneven.bin "$rs" 256 0; done
run_case blast_M_1       uneven.bin  1024 1   0
run_case blast_M_16      uneven.bin  1024 16  0
run_case blast_M_17      uneven.bin  1024 17  0
run_case blast_M_512     uneven.bin  256  512 0

echo "== Data loss (sender garbler) =="
for p in 1 5 10 20 30; do run_case "loss_$p" uneven.bin 512 256 "$p"; done
run_case whole_blast_lost_M512 uneven.bin 256 512 30   # big blast, many missing records

echo "== Lost control packets in both directions (receiver garbler on) =="
run_case lost_ack_recmiss_10 uneven.bin 512 256 10 10
run_case lost_ack_recmiss_30 uneven.bin 512 256 30 30

if [ -z "${QUICK:-}" ]; then
    echo "== Full-size files =="
    run_case ten_mb_0        ten_mb.bin     1024 512 0
    run_case fifteen_mb_0    fifteen_mb.bin 1024 512 0
    run_case ten_mb_10       ten_mb.bin     1024 512 10
    run_case fifteen_mb_20   fifteen_mb.bin 512  512 20 5
fi

echo "== Closing timer / robustness (scripted fake sender) =="
if python3 -I "$ROOT/tests/closing_timer_test.py" "$ROOT/receiver" $((PORT_BASE + 200)) \
        > "$LOGS/closing_timer.log" 2>&1; then
    PASS=$((PASS + 1)); grep '^PASS' "$LOGS/closing_timer.log"
else
    FAIL=$((FAIL + 1)); grep -E '^(PASS|FAIL)' "$LOGS/closing_timer.log"
    echo "FAIL closing_timer_test (see $LOGS/closing_timer.log)"
fi

echo
echo "Passed: $PASS   Failed: $FAIL   (logs in $LOGS)"
[ $FAIL -eq 0 ]
