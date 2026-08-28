// traffic-app/test_pacer.c
#include <assert.h>
#include <stdio.h>
#include "pacer.h"

int main(void) {
    mp_pacer_t p;

    /* 1200-byte chunks at 10,000,000 bps -> interval = 1200*8*1e6/10e6 = 960us */
    mp_pacer_init(&p, 1000000, 1200, 10000000);
    assert(p.interval_us == 960);
    assert(mp_pacer_is_due(&p, 1000000) == 1);   /* first chunk due immediately at start_time */
    assert(mp_pacer_is_due(&p, 999999) == 0);    /* not due before start_time */
    assert(mp_pacer_next_time(&p) == 1000000);

    mp_pacer_advance(&p);
    assert(p.nb_chunks_sent == 1);
    assert(mp_pacer_next_time(&p) == 1000960);
    assert(mp_pacer_is_due(&p, 1000959) == 0);
    assert(mp_pacer_is_due(&p, 1000960) == 1);

    /* 125-byte chunks at 1,000,000 bps -> interval = 125*8*1e6/1e6 = 1000us */
    mp_pacer_t q;
    mp_pacer_init(&q, 0, 125, 1000000);
    assert(q.interval_us == 1000);

    /* No drift after many advances: next_time == start_time + N*interval exactly */
    mp_pacer_t r;
    mp_pacer_init(&r, 500, 1200, 10000000);
    for (int i = 0; i < 1000; i++) {
        mp_pacer_advance(&r);
    }
    assert(mp_pacer_next_time(&r) == 500 + 1000ULL * 960ULL);

    printf("all pacer tests passed\n");
    return 0;
}
