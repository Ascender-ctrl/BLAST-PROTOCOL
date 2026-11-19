# BLAST File Transfer Protocol over UDP

CSc 412 Computer Networks project. Reliable blast-oriented file transfer in C over POSIX UDP sockets.

## Build

```
make            # builds ./sender and ./receiver (both link garbler.c)
make test       # builds, then runs the correctness suite
```

## Run

```
./receiver <port> <output_dir> [drop_rate_percent]
./sender   <receiver_ip> <port> <file_path> <record_size: 256|512|1024> <blast_size_M> <drop_rate_percent>
```

Start the receiver first. `output_dir` must be a different directory from the source file, because the receiver saves the file under the name the sender gives it. The receiver's optional drop rate runs its own replies (FILE_HDR_ACK, REC_MISS) through the garbler, so lost control packets can be tested in the receiver-to-sender direction too.

## Receiver design (receiver.c)

| Phase | Behaviour |
|---|---|
| WAIT_HDR | Waits for FILE_HDR, validates record size, M and filename (no paths), opens `output_dir/<name>`, allocates the blast buffer (M × record_size) and replies FILE_HDR_ACK. Duplicate FILE_HDRs (lost ACK) are re-ACKed. |
| TRANSFER | DATA records for the current blast are copied into the buffer and marked in a received bitmap. Duplicates, stale blasts, out-of-range IDs, wrong `data_len` and packets from other hosts are counted and ignored. On IS_BLAST_OVER(b) it replies REC_MISS with the missing IDs. When nothing is missing it writes the blast to disk in order and moves to blast b+1. |
| CLOSING | Entered when the last blast is complete. The closing timer answers retransmitted IS_BLAST_OVERs (whose empty REC_MISS reply was lost) and waits for DISCONNECT. Each packet that arrives restarts the timer, and after 3 s of silence the receiver exits. This also ends the receiver cleanly if every DISCONNECT was dropped. |

Timers are driven by `select()` with a 100 ms tick. If the sender goes silent for 30 s mid-transfer, the receiver aborts and deletes the partial file.

Each REC_MISS is capped at 509 IDs so it fits the sender's 2048-byte receive buffer. Any remaining IDs are reported on the next IS_BLAST_OVER. The socket's SO_RCVBUF is raised to 8 MB so back-to-back blasts are not dropped by the kernel on top of the garbler's simulated loss.

## Tests

| Script | What it checks |
|---|---|
| `tests/correctness.sh` | 25 sender to receiver transfers checked with `cmp`/md5. Covers empty, sub-record, exact-blast and uneven-tail files, all record sizes, M = 1/16/17/512, 1-30% data loss, lost ACK/REC_MISS (receiver garbler), and 10 MB / 15 MB files. Both programs must also exit 0. `QUICK=1` skips the large files. |
| `tests/closing_timer_test.py` | A scripted fake sender checks selective repeat, re-ACK of duplicate FILE_HDR, junk/stale/stranger packets, the REC_MISS cap, re-answering IS_BLAST_OVER during linger, and the closing timer with all DISCONNECTs lost. |
| `tests/plot_results.py` | Draws throughput vs loss rate from a CSV (`loss_pct, throughput_mbps[, record_size]`) or from sender logs, and prints a Markdown table of mean/std/min/max. |
