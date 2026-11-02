#ifndef AIOT_MOQT_TX_POLICY_H
#define AIOT_MOQT_TX_POLICY_H
#include <stdint.h>
#include <stddef.h>
/* Fixed layout: setup/replies/AI (0..3), JPEG objects (4..5).
 * The mask excludes FIN-submitted streams waiting for acknowledgement. */
static inline int moqt_tx_oldest_ready(unsigned mask,const uint64_t queued[6]) {
 for(int i=0;i<4;i++)if(mask&(1U<<i))return i;
 if((mask&0x30U)==0x30U)return queued[4]<=queued[5]?4:5;
 if(mask&(1U<<4))return 4;
 if(mask&(1U<<5))return 5;
 return -1;
}
/* Return how many new preview bytes may be submitted now.  Pacing is checked
 * before the byte cap so a blocked pacing interval never consumes budget. */
static inline size_t moqt_preview_allowance(size_t cap,size_t outstanding,
                                            uint64_t pace_next,uint64_t now) {
 if(pace_next&&now<pace_next)return 0;
 if(!cap)return SIZE_MAX;
 return outstanding<cap?cap-outstanding:0;
}
static inline uint64_t moqt_preview_pace_advance(uint64_t prior,uint64_t now,
                                                 size_t accepted,unsigned kbps) {
 if(!kbps||!accepted)return prior;
 uint64_t base=prior>now?prior:now;
 return base+((uint64_t)accepted*8000U+kbps-1U)/kbps;
}
#endif
