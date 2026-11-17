#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libgen.h>

#include "protocol.h"
#include "garbler.h"

#define TIMEOUT_SEC 0
#define TIMEOUT_USEC 200000  // 200 ms retransmission timeout
#define MAX_RETRIES 50

int recv_with_timeout(int sockfd, void *buf, size_t max_len, struct sockaddr_in *src_addr, socklen_t *addrlen) {
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = TIMEOUT_USEC;

    int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (rv <= 0) {
        return rv;
    }
    return (int)recvfrom(sockfd, buf, max_len, 0, (struct sockaddr *)src_addr, addrlen);
}

void send_records_batch(int sockfd, struct sockaddr_in *serv_addr, FILE *fp,
                        uint32_t blast_id, uint64_t blast_start_offset,
                        uint64_t file_size, uint32_t record_size,
                        const uint32_t *rec_ids, uint32_t count) {
    DataPacket pkt;
    uint32_t idx = 0;

    while (idx < count) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.hdr.type = PKT_DATA;
        pkt.hdr.blast_id = blast_id;
        pkt.num_records = 0;

        while (pkt.num_records < MAX_RECORDS_PER_PKT && idx < count) {
            uint32_t r_id = rec_ids[idx];
            uint64_t file_offset = blast_start_offset + ((uint64_t)r_id * record_size);

            if (file_offset >= file_size) {
                idx++;
                continue;
            }

            uint64_t remaining = file_size - file_offset;
            uint16_t to_read = (remaining < record_size) ? (uint16_t)remaining : (uint16_t)record_size;

            fseek(fp, (long)file_offset, SEEK_SET);
            size_t n = fread(pkt.records[pkt.num_records].data, 1, to_read, fp);

            pkt.records[pkt.num_records].record_id = r_id;
            pkt.records[pkt.num_records].data_len = (uint16_t)n;
            pkt.num_records++;
            idx++;
        }

        if (pkt.num_records > 0) {
            size_t send_bytes = sizeof(PacketHeader) + sizeof(uint8_t) +
                                (pkt.num_records * sizeof(RecordEntry));
            pkt.hdr.payload_len = (uint16_t)(send_bytes - sizeof(PacketHeader));
            garbler_sendto(sockfd, &pkt, send_bytes, 0,
                           (struct sockaddr *)serv_addr, sizeof(*serv_addr));
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr, "Usage: %s <receiver_ip> <port> <file_path> <record_size: 256|512|1024> <blast_size_M> <drop_rate_percent>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    char *filepath = argv[3];
    uint32_t record_size = (uint32_t)atoi(argv[4]);
    uint32_t blast_size = (uint32_t)atoi(argv[5]);
    float drop_rate = (float)atof(argv[6]);

    if (record_size != 256 && record_size != 512 && record_size != 1024) {
        fprintf(stderr, "Error: record_size must be 256, 512, or 1024.\n");
        return 1;
    }
    if (blast_size == 0 || blast_size > MAX_MISSING_RECORDS) {
        fprintf(stderr, "Error: blast_size M must be between 1 and %d.\n", MAX_MISSING_RECORDS);
        return 1;
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("stat");
        return 1;
    }
    uint64_t file_size = (uint64_t)st.st_size;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        fclose(fp);
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        fclose(fp);
        return 1;
    }

    garbler_init(drop_rate);

    struct timeval start_tv, end_tv;
    gettimeofday(&start_tv, NULL);

    // --- PHASE 1: FILE_HDR Handshake ---
    FileHdrPacket hdr_pkt;
    memset(&hdr_pkt, 0, sizeof(hdr_pkt));
    hdr_pkt.hdr.type = PKT_FILE_HDR;
    hdr_pkt.hdr.blast_id = 0;
    hdr_pkt.hdr.payload_len = sizeof(FileHdrPacket) - sizeof(PacketHeader);
    hdr_pkt.file_size = file_size;
    hdr_pkt.record_size = record_size;
    hdr_pkt.blast_size = blast_size;

    char *path_copy = strdup(filepath);
    strncpy(hdr_pkt.filename, basename(path_copy), MAX_FILENAME_LEN - 1);
    free(path_copy);

    int retries = 0;
    int handshake_done = 0;
    uint8_t recv_buf[2048];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (retries < MAX_RETRIES && !handshake_done) {
        garbler_sendto(sockfd, &hdr_pkt, sizeof(hdr_pkt), 0,
                       (struct sockaddr *)&serv_addr, sizeof(serv_addr));

        int n = recv_with_timeout(sockfd, recv_buf, sizeof(recv_buf), &from_addr, &from_len);
        if (n >= (int)sizeof(PacketHeader)) {
            PacketHeader *resp = (PacketHeader *)recv_buf;
            if (resp->type == PKT_FILE_HDR_ACK) {
                handshake_done = 1;
            }
        } else {
            retries++;
        }
    }

    if (!handshake_done) {
        fprintf(stderr, "Error: Receiver unresponsive during FILE_HDR handshake.\n");
        close(sockfd);
        fclose(fp);
        return 1;
    }

    // --- PHASE 2: Blast Loop ---
    uint64_t total_records = (file_size + record_size - 1) / record_size;
    uint32_t total_blasts = (uint32_t)((total_records + blast_size - 1) / blast_size);
    uint64_t records_resent = 0;

    uint32_t *rec_list = malloc(blast_size * sizeof(uint32_t));

    for (uint32_t b = 0; b < total_blasts; b++) {
        uint64_t blast_start_rec = (uint64_t)b * blast_size;
        uint32_t recs_in_blast = (uint32_t)((total_records - blast_start_rec < blast_size)
                                            ? (total_records - blast_start_rec)
                                            : blast_size);
        uint64_t blast_byte_offset = blast_start_rec * record_size;

        for (uint32_t i = 0; i < recs_in_blast; i++) {
            rec_list[i] = i;
        }
        send_records_batch(sockfd, &serv_addr, fp, b, blast_byte_offset,
                           file_size, record_size, rec_list, recs_in_blast);

        int blast_complete = 0;
        retries = 0;

        while (!blast_complete && retries < MAX_RETRIES) {
            PacketHeader poll_pkt;
            poll_pkt.type = PKT_IS_BLAST_OVER;
            poll_pkt.blast_id = b;
            poll_pkt.payload_len = 0;

            garbler_sendto(sockfd, &poll_pkt, sizeof(poll_pkt), 0,
                           (struct sockaddr *)&serv_addr, sizeof(serv_addr));

            int n = recv_with_timeout(sockfd, recv_buf, sizeof(recv_buf), &from_addr, &from_len);
            if (n >= (int)sizeof(PacketHeader)) {
                PacketHeader *resp = (PacketHeader *)recv_buf;
                if (resp->type == PKT_REC_MISS && resp->blast_id == b) {
                    retries = 0;
                    RecMissPacket *miss_pkt = (RecMissPacket *)recv_buf;

                    if (miss_pkt->num_missing == 0) {
                        blast_complete = 1;
                    } else {
                        records_resent += miss_pkt->num_missing;
                        send_records_batch(sockfd, &serv_addr, fp, b, blast_byte_offset,
                                           file_size, record_size,
                                           miss_pkt->missing_ids, miss_pkt->num_missing);
                    }
                }
            } else {
                retries++;
            }
        }

        if (!blast_complete) {
            fprintf(stderr, "Error: Receiver unresponsive during blast %u.\n", b);
            free(rec_list);
            close(sockfd);
            fclose(fp);
            return 1;
        }
    }

    free(rec_list);

    // --- PHASE 3: Disconnect ---
    PacketHeader disc_pkt;
    disc_pkt.type = PKT_DISCONNECT;
    disc_pkt.blast_id = total_blasts;
    disc_pkt.payload_len = 0;

    for (int i = 0; i < 3; i++) {
        garbler_sendto(sockfd, &disc_pkt, sizeof(disc_pkt), 0,
                       (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    }

    gettimeofday(&end_tv, NULL);
    double elapsed = (end_tv.tv_sec - start_tv.tv_sec) +
                     (end_tv.tv_usec - start_tv.tv_usec) / 1e6;
    double mb_transferred = (double)file_size / (1024.0 * 1024.0);
    double speed_mbps = (elapsed > 0.0) ? (mb_transferred / elapsed) : 0.0;

    GarblerStats stats = garbler_get_stats();

    printf("=== BLAST Transfer Summary ===\n");
    printf("File Size        : %llu bytes (%.2f MB)\n", (unsigned long long)file_size, mb_transferred);
    printf("Loss Rate Config : %.1f%%\n", drop_rate);
    printf("Packets Sent     : %llu\n", (unsigned long long)stats.total_sent);
    printf("Packets Dropped  : %llu\n", (unsigned long long)stats.total_dropped);
    printf("Records Resent   : %llu\n", (unsigned long long)records_resent);
    printf("Elapsed Time     : %.4f s\n", elapsed);
    printf("Throughput       : %.4f MB/s\n", speed_mbps);

    close(sockfd);
    fclose(fp);
    return 0;
}
