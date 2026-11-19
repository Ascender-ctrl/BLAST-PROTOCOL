/*
 * receiver.c - BLAST file receiver (CSc 412 file-transfer project)
 *
 * Protocol phases as seen from the receiver:
 *   Phase 1  WAIT_HDR  : wait for FILE_HDR, open output file, allocate the
 *                        blast buffer (B'), reply FILE_HDR_ACK.  Duplicate
 *                        FILE_HDRs (our ACK was lost) are simply re-ACKed.
 *   Phase 2  TRANSFER  : store DATA records for the current blast in memory.
 *                        On IS_BLAST_OVER(b) reply REC_MISS with the list of
 *                        missing record IDs; when that list is empty the blast
 *                        is flushed to disk and we move on to blast b+1.
 *   Phase 3  CLOSING   : entered when the last blast is complete.  The
 *                        closing timer keeps the receiver alive so it can
 *                        answer retransmitted IS_BLAST_OVERs (whose empty
 *                        REC_MISS reply may have been lost) and catch the
 *                        DISCONNECT.  Every packet that arrives restarts the
 *                        timer; after CLOSE_LINGER_MS of silence we exit.
 *                        This also covers the case where all of the sender's
 *                        DISCONNECT packets were dropped.
 *
 * All timers are driven by select() with a short tick, the same technique
 * the sender uses for its retransmission timers.
 *
 * Wire format note: sender.c sends every field in host byte order and sends
 * sizeof(RecordEntry) bytes per record regardless of data_len, so this file
 * parses packets the same way.  Both programs must run on machines with the
 * same endianness (true for any x86/ARM Linux pair).
 *
 * Usage: ./receiver <port> <output_dir> [drop_rate_percent]
 *   drop_rate_percent (optional, default 0) runs the receiver's own replies
 *   (FILE_HDR_ACK, REC_MISS) through the garbler, so lost control packets
 *   in the receiver->sender direction can be tested too.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "protocol.h"
#include "garbler.h"

/* ---- Tunables ---------------------------------------------------------- */
#define TICK_MS            100     /* select() granularity for all timers    */
#define CLOSE_LINGER_MS    3000    /* closing timer: silence before exit     */
#define IDLE_TIMEOUT_MS    30000   /* abort if sender goes silent mid-file   */
#define MAX_BLAST_SIZE     10000   /* spec allows M in 200..10000            */
#define SOCK_RCVBUF_BYTES  (8 * 1024 * 1024)

/* sender.c receives replies into a 2048-byte buffer, so a REC_MISS larger
 * than that would be truncated.  Cap each reply; any IDs left over are
 * reported in the reply to the sender's next IS_BLAST_OVER. */
#define SENDER_RECV_BUF    2048
#define MAX_MISS_PER_REPLY ((SENDER_RECV_BUF - sizeof(PacketHeader) - sizeof(uint16_t)) / sizeof(uint32_t))

/* Largest datagram the sender can produce (16 full RecordEntry structs). */
#define RX_BUF_SIZE        (sizeof(DataPacket) + 64)

typedef enum { RX_WAIT_HDR, RX_TRANSFER, RX_CLOSING } RxPhase;

typedef struct {
    uint64_t data_pkts;          /* DATA packets accepted for current blast */
    uint64_t records_new;        /* first copies of records                 */
    uint64_t records_dup;        /* duplicate copies (already had it)       */
    uint64_t stale_pkts;         /* DATA for an already-finished blast      */
    uint64_t bad_pkts;           /* malformed / wrong sender / bad record   */
    uint64_t hdr_rx;             /* FILE_HDR packets (incl. duplicates)     */
    uint64_t poll_rx;            /* IS_BLAST_OVER packets received          */
    uint64_t recmiss_tx;         /* REC_MISS replies sent                   */
    uint64_t missing_reported;   /* sum of num_missing over all replies     */
    uint64_t bytes_written;
    int      disconnect_seen;
} RxStats;

typedef struct {
    RxPhase  phase;
    int      sockfd;
    struct sockaddr_in peer;     /* sender we are locked to after FILE_HDR  */

    /* negotiated in FILE_HDR */
    uint64_t file_size;
    uint32_t record_size;
    uint32_t blast_size;
    uint64_t total_records;
    uint32_t total_blasts;
    char     out_path[1024];
    FILE    *out;

    /* current blast (the B' buffer from Figure 1 of the spec) */
    uint32_t cur_blast;
    uint32_t recs_in_blast;
    uint32_t have_count;
    uint8_t *have;               /* have[i] = 1 if record i of blast stored */
    uint8_t *buf;                /* blast_size * record_size bytes          */

    RxStats  st;
    double   t_start;            /* time FILE_HDR accepted                  */
    double   t_done;             /* time last blast flushed                 */
} Rx;

/* ---- Small helpers ----------------------------------------------------- */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_to_peer(Rx *rx, const void *pkt, size_t len) {
    garbler_sendto(rx->sockfd, pkt, len, 0,
                   (const struct sockaddr *)&rx->peer, sizeof(rx->peer));
}

/* Number of records in blast b (last blast may be short). */
static uint32_t records_in_blast(const Rx *rx, uint32_t b) {
    uint64_t first = (uint64_t)b * rx->blast_size;
    uint64_t left  = rx->total_records - first;
    return (uint32_t)(left < rx->blast_size ? left : rx->blast_size);
}

/* Expected byte length of record rid of the current blast (handles the
 * uneven tail when file_size is not a multiple of record_size). */
static uint32_t expected_len(const Rx *rx, uint32_t rid) {
    uint64_t off  = ((uint64_t)rx->cur_blast * rx->blast_size + rid) * rx->record_size;
    uint64_t left = rx->file_size - off;
    return (uint32_t)(left < rx->record_size ? left : rx->record_size);
}

static void start_blast(Rx *rx, uint32_t b) {
    rx->cur_blast = b;
    rx->have_count = 0;
    if (b < rx->total_blasts) {
        rx->recs_in_blast = records_in_blast(rx, b);
        memset(rx->have, 0, rx->blast_size);
    } else {
        rx->recs_in_blast = 0;
    }
}

/* Filenames come off the network: only allow a plain name, no paths. */
static int filename_ok(const char *name) {
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    return strchr(name, '/') == NULL;
}

/* ---- Outgoing control packets ----------------------------------------- */
static void send_hdr_ack(Rx *rx) {
    PacketHeader ack;
    ack.type = PKT_FILE_HDR_ACK;
    ack.blast_id = 0;
    ack.payload_len = 0;
    send_to_peer(rx, &ack, sizeof(ack));
}

static void send_rec_miss(Rx *rx, uint32_t blast_id, const uint32_t *ids, uint16_t n) {
    RecMissPacket pkt;
    pkt.hdr.type = PKT_REC_MISS;
    pkt.hdr.blast_id = blast_id;
    pkt.num_missing = n;
    if (n) memcpy(pkt.missing_ids, ids, n * sizeof(uint32_t));
    size_t len = sizeof(PacketHeader) + sizeof(uint16_t) + n * sizeof(uint32_t);
    pkt.hdr.payload_len = (uint16_t)(len - sizeof(PacketHeader));
    send_to_peer(rx, &pkt, len);
    rx->st.recmiss_tx++;
    rx->st.missing_reported += n;
}

/* ---- Disk flush ------------------------------------------------------- */
/* Blasts complete strictly in order, so a blast is appended sequentially. */
static int flush_blast(Rx *rx) {
    uint64_t start = (uint64_t)rx->cur_blast * rx->blast_size * rx->record_size;
    uint64_t want  = (uint64_t)rx->recs_in_blast * rx->record_size;
    if (start + want > rx->file_size) want = rx->file_size - start;

    if (fwrite(rx->buf, 1, want, rx->out) != want) {
        perror("fwrite");
        return -1;
    }
    rx->st.bytes_written += want;
    return 0;
}

/* ---- Incoming packet handlers ----------------------------------------- */
static int handle_file_hdr(Rx *rx, const uint8_t *pkt, ssize_t n,
                           const struct sockaddr_in *from, const char *out_dir) {
    rx->st.hdr_rx++;

    if (rx->phase != RX_WAIT_HDR) {
        /* Our ACK was lost and the sender retried: ACK again. */
        if (same_peer(from, &rx->peer)) send_hdr_ack(rx);
        return 0;
    }
    if (n < (ssize_t)sizeof(FileHdrPacket)) { rx->st.bad_pkts++; return 0; }

    FileHdrPacket h;
    memcpy(&h, pkt, sizeof(h));
    h.filename[MAX_FILENAME_LEN - 1] = '\0';

    if (h.record_size != 256 && h.record_size != 512 && h.record_size != 1024) {
        fprintf(stderr, "receiver: rejecting FILE_HDR, bad record size %u\n", h.record_size);
        rx->st.bad_pkts++;
        return 0;
    }
    if (h.blast_size == 0 || h.blast_size > MAX_BLAST_SIZE) {
        fprintf(stderr, "receiver: rejecting FILE_HDR, bad blast size %u\n", h.blast_size);
        rx->st.bad_pkts++;
        return 0;
    }
    if (!filename_ok(h.filename)) {
        fprintf(stderr, "receiver: rejecting FILE_HDR, unsafe filename\n");
        rx->st.bad_pkts++;
        return 0;
    }

    rx->peer          = *from;
    rx->file_size     = h.file_size;
    rx->record_size   = h.record_size;
    rx->blast_size    = h.blast_size;
    rx->total_records = (h.file_size + h.record_size - 1) / h.record_size;
    rx->total_blasts  = (uint32_t)((rx->total_records + h.blast_size - 1) / h.blast_size);

    snprintf(rx->out_path, sizeof(rx->out_path), "%s/%s", out_dir, h.filename);
    rx->out = fopen(rx->out_path, "wb");
    if (!rx->out) { perror(rx->out_path); return -1; }

    rx->have = calloc(rx->blast_size, 1);
    rx->buf  = malloc((size_t)rx->blast_size * rx->record_size);
    if (!rx->have || !rx->buf) { perror("malloc"); return -1; }

    printf("receiver: FILE_HDR '%s'  %llu bytes  record=%u  M=%u  (%llu records, %u blasts)\n",
           h.filename, (unsigned long long)h.file_size, h.record_size, h.blast_size,
           (unsigned long long)rx->total_records, rx->total_blasts);

    rx->t_start = now_ms();
    start_blast(rx, 0);
    send_hdr_ack(rx);

    if (rx->total_blasts == 0) {            /* empty file: nothing to blast */
        rx->t_done = now_ms();
        rx->phase = RX_CLOSING;
    } else {
        rx->phase = RX_TRANSFER;
    }
    return 0;
}

static void handle_data(Rx *rx, const uint8_t *pkt, ssize_t n) {
    const PacketHeader *ph = (const PacketHeader *)pkt;

    if (rx->phase != RX_TRANSFER || ph->blast_id != rx->cur_blast) {
        rx->st.stale_pkts++;               /* late duplicate of an old blast */
        return;
    }
    if (n < (ssize_t)(sizeof(PacketHeader) + 1)) { rx->st.bad_pkts++; return; }

    uint8_t nrec = pkt[sizeof(PacketHeader)];
    size_t need = sizeof(PacketHeader) + 1 + (size_t)nrec * sizeof(RecordEntry);
    if (nrec == 0 || nrec > MAX_RECORDS_PER_PKT || (size_t)n < need) {
        rx->st.bad_pkts++;
        return;
    }

    rx->st.data_pkts++;
    const uint8_t *p = pkt + sizeof(PacketHeader) + 1;
    for (uint8_t i = 0; i < nrec; i++, p += sizeof(RecordEntry)) {
        RecordEntry re;
        memcpy(&re.record_id, p, sizeof(re.record_id));
        memcpy(&re.data_len, p + sizeof(re.record_id), sizeof(re.data_len));
        const uint8_t *data = p + sizeof(re.record_id) + sizeof(re.data_len);

        if (re.record_id >= rx->recs_in_blast ||
            re.data_len != expected_len(rx, re.record_id)) {
            rx->st.bad_pkts++;
            continue;
        }
        if (rx->have[re.record_id]) { rx->st.records_dup++; continue; }

        memcpy(rx->buf + (size_t)re.record_id * rx->record_size, data, re.data_len);
        rx->have[re.record_id] = 1;
        rx->have_count++;
        rx->st.records_new++;
    }
}

static int handle_is_blast_over(Rx *rx, uint32_t b) {
    rx->st.poll_rx++;

    if (rx->phase == RX_WAIT_HDR) return 0;

    if (b < rx->cur_blast || rx->phase == RX_CLOSING) {
        /* Already complete: our empty REC_MISS was lost, so resend it. */
        send_rec_miss(rx, b, NULL, 0);
        return 0;
    }
    if (b > rx->cur_blast) { rx->st.bad_pkts++; return 0; }

    /* b == cur_blast: report what is still missing. */
    uint32_t missing[MAX_MISSING_RECORDS];
    uint16_t nmiss = 0;
    for (uint32_t r = 0; r < rx->recs_in_blast && nmiss < MAX_MISS_PER_REPLY; r++)
        if (!rx->have[r]) missing[nmiss++] = r;

    if (nmiss == 0) {
        if (flush_blast(rx) < 0) return -1;
        start_blast(rx, b + 1);
        if (rx->cur_blast == rx->total_blasts) {
            fflush(rx->out);
            rx->t_done = now_ms();
            rx->phase = RX_CLOSING;         /* arm the closing timer */
        }
    }
    send_rec_miss(rx, b, missing, nmiss);
    return 0;
}

/* ---- Main -------------------------------------------------------------- */
static void print_summary(const Rx *rx, int ok) {
    double secs = (rx->t_done > rx->t_start) ? (rx->t_done - rx->t_start) / 1000.0 : 0.0;
    double mb = rx->st.bytes_written / (1024.0 * 1024.0);
    GarblerStats g = garbler_get_stats();

    printf("=== BLAST Receiver Summary ===\n");
    printf("Result           : %s\n", ok ? "COMPLETE" : "FAILED");
    printf("Output File      : %s\n", rx->out_path[0] ? rx->out_path : "(none)");
    printf("Bytes Written    : %llu (%.2f MB)\n", (unsigned long long)rx->st.bytes_written, mb);
    printf("DATA Packets     : %llu (stale %llu, bad %llu)\n",
           (unsigned long long)rx->st.data_pkts, (unsigned long long)rx->st.stale_pkts,
           (unsigned long long)rx->st.bad_pkts);
    printf("Records New/Dup  : %llu / %llu\n",
           (unsigned long long)rx->st.records_new, (unsigned long long)rx->st.records_dup);
    printf("FILE_HDR Rx      : %llu\n", (unsigned long long)rx->st.hdr_rx);
    printf("IS_BLAST_OVER Rx : %llu\n", (unsigned long long)rx->st.poll_rx);
    printf("REC_MISS Sent    : %llu (ids reported %llu, replies dropped %llu)\n",
           (unsigned long long)rx->st.recmiss_tx, (unsigned long long)rx->st.missing_reported,
           (unsigned long long)g.total_dropped);
    printf("DISCONNECT Seen  : %s\n", rx->st.disconnect_seen ? "yes" : "no (closing timer expired)");
    printf("Transfer Time    : %.4f s\n", secs);
    printf("Throughput       : %.4f MB/s\n", secs > 0 ? mb / secs : 0.0);
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <port> <output_dir> [drop_rate_percent]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_dir = argv[2];
    float drop = (argc == 4) ? (float)atof(argv[3]) : 0.0f;

    struct stat sd;
    if (stat(out_dir, &sd) != 0 || !S_ISDIR(sd.st_mode)) {
        fprintf(stderr, "receiver: output_dir '%s' is not a directory\n", out_dir);
        return 1;
    }

    Rx rx;
    memset(&rx, 0, sizeof(rx));
    rx.phase = RX_WAIT_HDR;

    rx.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx.sockfd < 0) { perror("socket"); return 1; }

    /* A blast arrives back-to-back; a bigger kernel buffer (B in Figure 1)
     * avoids real drops on top of the garbler's simulated ones. */
    int rcvbuf = SOCK_RCVBUF_BYTES;
    setsockopt(rx.sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    socklen_t optlen = sizeof(rcvbuf);
    getsockopt(rx.sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(rx.sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(rx.sockfd);
        return 1;
    }

    garbler_init(drop);
    printf("receiver: listening on UDP %d, writing to %s/, reply drop %.1f%%, SO_RCVBUF %d\n",
           port, out_dir, drop, rcvbuf);
    fflush(stdout);

    uint8_t *pkt = malloc(RX_BUF_SIZE);
    if (!pkt) { perror("malloc"); return 1; }

    int ok = 0, running = 1;
    double last_rx = now_ms();

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(rx.sockfd, &rfds);
        struct timeval tv = { 0, TICK_MS * 1000 };

        int rv = select(rx.sockfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (rv > 0) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(rx.sockfd, pkt, RX_BUF_SIZE, 0, (struct sockaddr *)&from, &flen);
            if (n < (ssize_t)sizeof(PacketHeader)) continue;

            const PacketHeader *ph = (const PacketHeader *)pkt;
            if (rx.phase != RX_WAIT_HDR && !same_peer(&from, &rx.peer)) {
                rx.st.bad_pkts++;           /* stranger: ignore */
                continue;
            }
            last_rx = now_ms();

            int err = 0;
            switch (ph->type) {
            case PKT_FILE_HDR:      err = handle_file_hdr(&rx, pkt, n, &from, out_dir); break;
            case PKT_DATA:          handle_data(&rx, pkt, n); break;
            case PKT_IS_BLAST_OVER: err = handle_is_blast_over(&rx, ph->blast_id); break;
            case PKT_DISCONNECT:
                if (rx.phase == RX_TRANSFER)
                    fprintf(stderr, "receiver: DISCONNECT before last blast completed\n");
                rx.st.disconnect_seen = 1;   /* keep lingering; timer below exits */
                break;
            default:                rx.st.bad_pkts++; break;
            }
            if (err) break;
        }

        /* Timers */
        double idle = now_ms() - last_rx;
        if (rx.phase == RX_CLOSING && idle >= CLOSE_LINGER_MS) {
            ok = 1;
            running = 0;
        } else if (rx.phase == RX_TRANSFER && idle >= IDLE_TIMEOUT_MS) {
            fprintf(stderr, "receiver: sender silent for %d ms, aborting\n", IDLE_TIMEOUT_MS);
            running = 0;
        }
    }

    if (rx.out) {
        fclose(rx.out);
        if (!ok) remove(rx.out_path);       /* never leave a half-written file */
    }
    print_summary(&rx, ok);

    free(pkt);
    free(rx.have);
    free(rx.buf);
    close(rx.sockfd);
    return ok ? 0 : 1;
}