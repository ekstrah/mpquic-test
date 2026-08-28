// traffic-app/pacer.c
#include "pacer.h"

void mp_pacer_init(mp_pacer_t* pacer, uint64_t start_time, uint64_t chunk_bytes, uint64_t target_bps) {
    pacer->start_time = start_time;
    pacer->interval_us = (chunk_bytes * 8ULL * 1000000ULL) / target_bps;
    pacer->nb_chunks_sent = 0;
}

uint64_t mp_pacer_next_time(const mp_pacer_t* pacer) {
    return pacer->start_time + pacer->nb_chunks_sent * pacer->interval_us;
}

int mp_pacer_is_due(const mp_pacer_t* pacer, uint64_t now) {
    return now >= mp_pacer_next_time(pacer);
}

void mp_pacer_advance(mp_pacer_t* pacer) {
    pacer->nb_chunks_sent += 1;
}
