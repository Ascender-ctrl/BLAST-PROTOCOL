#ifndef PROTOCOL_H
#define Protocol_H

#include <stdint.h>

#define MAX_FILENAME_LEN 256
#define MAX_RECORD_SIZE 1024
#define MAX_RECORDS_PER_PKT 16
#define MAX_MISSING_RECORDS 512
typedef enum {
    PKT_FILE_HDR = 1,
    PKT_FILE_HDR_ACK = 2,
    PKT_DATA = 3,
    PKT_IS_BLAST_OVER = 4,
    PKT_REC_MISS = 5,
    PKT_DISCONNECT = 6
} PacketType;

#pragma pack(push, 1)

typedef struct {
    uint8_t type;
    uint32_t blast_id;
    uint16_t payload_len;
} PacketHeader;

typedef struct {
    PacketHeader hdr;
    uint64_t file_size;
    uint32_t record_size;   // 256, 512, or 1024
    uint32_t blast_size;    // M records per blast
    char filename[MAX_FILENAME_LEN];
} FileHdrPacket;
typedef struct {
    uint32_t record_id;     // Relative index within current blast (0 to M-1)
    uint16_t data_len;      // Actual bytes in this record (handles uneven tail)
    uint8_t data[MAX_RECORD_SIZE];
} RecordEntry;

typedef struct {
    PacketHeader hdr;
    uint8_t num_records;    // 1 to 16 records packed
    RecordEntry records[MAX_RECORDS_PER_PKT];
} DataPacket;

typedef struct {
    PacketHeader hdr;
    uint16_t num_missing;   // 0 means all records in blast received
    uint32_t missing_ids[MAX_MISSING_RECORDS];
} RecMissPacket;

#pragma pack(pop)

#endif // PROTOCOL_H
