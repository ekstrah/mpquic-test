// traffic-app/pacer.h
#ifndef MP_PACER_H
#define MP_PACER_H

#include <stdint.h>

typedef struct st_mp_pacer_t {
    uint64_t start_time;      /* microseconds, matches picoquic_current_time() convention */
    uint64_t interval_us;     /* time between chunks to hit the target rate */
    uint64_t nb_chunks_sent;
} mp_pacer_t;

/* chunk_bytes: size of each send. target_bps: desired bits per second. */
void mp_pacer_init(mp_pacer_t* pacer, uint64_t start_time, uint64_t chunk_bytes, uint64_t target_bps);

/* 1 if a chunk is due to be sent at time `now`, 0 otherwise. */
int mp_pacer_is_due(const mp_pacer_t* pacer, uint64_t now);

/* Absolute time (microseconds) the next chunk is due - use this to schedule the next wake. */
uint64_t mp_pacer_next_time(const mp_pacer_t* pacer);

/* Call after actually sending a chunk. */
void mp_pacer_advance(mp_pacer_t* pacer);

#endif
