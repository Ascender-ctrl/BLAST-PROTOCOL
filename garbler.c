#include "garbler.h"
#include <stdlib.h>
#include <time.h>

static float g_drop_prob = 0.0f;
static GarblerStats g_stats = {0, 0, 0};

void garbler_init(float drop_rate_percent) {
    srand((unsigned int)time(NULL));
    if (drop_rate_percent < 0.0f) drop_rate_percent = 0.0f;
    if (drop_rate_percent > 100.0f) drop_rate_percent = 100.0f;
    g_drop_prob = drop_rate_percent / 100.0f;
    g_stats.total_attempted = 0;
    g_stats.total_dropped = 0;
    g_stats.total_sent = 0;
}

ssize_t garbler_sendto(int sockfd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, socklen_t addrlen) {
    g_stats.total_attempted++;
    float r = (float)rand() / (float)RAND_MAX;

    if (r < g_drop_prob) {
        g_stats.total_dropped++;
        return (ssize_t)len;
    }

    g_stats.total_sent++;
    return sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

GarblerStats garbler_get_stats(void) {
    return g_stats;
}
