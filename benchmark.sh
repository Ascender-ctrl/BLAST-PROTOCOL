#!/bin/bash
RECEIVER_IP=${1:-"127.0.0.1"}
PORT=${2:-8080}
TEST_FILE="test_12mb.bin"
REC_SIZE=1024
BLAST_SIZE=256
OUT_CSV="benchmark_results.csv"

if [ ! -f "$TEST_FILE" ]; then
    dd if=/dev/urandom of="$TEST_FILE" bs=1M count=12 status=none
fi

echo "loss_rate,run,packets_sent,packets_dropped,records_resent,elapsed_sec,throughput_mbps" > "$OUT_CSV"

for LOSS in 0 1 5 10 20 30; do
    for RUN in 1 2 3 4 5; do
        echo "Running Loss=${LOSS}% (Run ${RUN}/5)..."
        OUTPUT=$(./sender "$RECEIVER_IP" "$PORT" "$TEST_FILE" "$REC_SIZE" "$BLAST_SIZE" "$LOSS")
        
        SENT=$(echo "$OUTPUT" | awk '/Packets Sent/ {print $4}')
        DROPPED=$(echo "$OUTPUT" | awk '/Packets Dropped/ {print $4}')
        RESENT=$(echo "$OUTPUT" | awk '/Records Resent/ {print $4}')
        TIME=$(echo "$OUTPUT" | awk '/Elapsed Time/ {print $4}')
        SPEED=$(echo "$OUTPUT" | awk '/Throughput/ {print $3}')
        
        echo "${LOSS},${RUN},${SENT},${DROPPED},${RESENT},${TIME},${SPEED}" >> "$OUT_CSV"
        sleep 1
    done
done

echo "Done! Results saved to $OUT_CSV"
