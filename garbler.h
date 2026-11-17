#ifndef GARBLER_H
#define GARBLER_H

#include <sys/socket.h>
#include <sys/types.h>
#include <stdint.h>

typedef struct {
    uint64_t total_attempted;
    uint64_t total_dropped;
    uint64_t total_sent;
} GarblerStats;

void garbler_init(float drop_rate_percent);
ssize_t garbler_sendto(int sockfd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, socklen_t addrlen);
GarblerStats garbler_get_stats(void);

#endif // GARBLER_H
