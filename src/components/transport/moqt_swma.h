/* SWMAth window estimator port, based on public-moq-demo player.ts,
 * commit 2071137a5967e645ab97b6e914dc4f30c8c47d95 (Apache-2.0).
 * Modified: bounded C storage, microsecond durations, bytes/s output.
 * Original copyright/license: experiments/moq/literature/NOTICE.md.
 * This is estimation math, not the paper's complete streaming system.
 */
#ifndef AIOT_MOQT_SWMA_H
#define AIOT_MOQT_SWMA_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
typedef struct { double bytes, us; } moqt_swma_sample;
typedef struct {
    moqt_swma_sample window[25];
    uint64_t count;
    double value;
    unsigned updates;
} moqt_swma;
static inline void moqt_swma_observe(moqt_swma *s, size_t bytes, uint64_t us) {
    s->window[s->count % 25] = (moqt_swma_sample){(double)bytes,(double)us};
    s->count++;
    if(s->count<25 || s->count%5) return;
    moqt_swma_sample a[25]; unsigned n=0;
    /* Original percentage filter removes nonpositive durations, sorts by rate,
     * trims ceil(5% n) fastest and floor(5% n) slowest. Zero-duration entries
     * are concatenated back but ignored by computeTPut; equivalent here. */
    for(unsigned i=0;i<25;i++) {
        unsigned j=(unsigned)((s->count+i)%25);
        if(s->window[j].us>0) a[n++]=s->window[j];
    }
    for(unsigned i=1;i<n;i++) {
        moqt_swma_sample x=a[i];unsigned j=i;
        while(j && a[j-1].bytes/a[j-1].us<x.bytes/x.us){a[j]=a[j-1];j--;}
        a[j]=x;
    }
    unsigned top=(n*5+99)/100,bottom=n*5/100;
    double size=0,duration=0;
    for(unsigned i=top;i<n-bottom;i++) if(a[i].bytes>0){size+=a[i].bytes;duration+=a[i].us;}
    if(duration>0 && size>0){s->value=size*1000000.0/duration;s->updates++;}
}
static inline double moqt_ifa_observe(double previous, size_t bytes, uint64_t us) {
    /* Called only for the first chunk of a segment by the original player. */
    double next=us && bytes?(double)bytes*1000000.0/(double)us:NAN;
    double last=isnan(previous)?0:previous; /* JS (previous || 0) */
    return last==0 || next<last*100 ? next:previous;
}
#endif
