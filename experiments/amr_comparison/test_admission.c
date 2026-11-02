#include "video_admission.h"
#include <assert.h>
int main(void) {
 video_admission_t s={0};
 assert(!video_policy_set(4)); assert(video_policy_set(2)); assert(video_policy_get()==2);
 assert(!video_admission_allow(&s,0,1,0,0,0,0,100000,0));
 assert(video_admission_allow(&s,1,1000000,-55,50,20000,0,100000,0));
 assert(!video_admission_allow(&s,1,1000100,-55,50,20000,0,100000,1));
 assert(!video_admission_allow(&s,1,1000200,-55,50,20000,90000,100000,0));
 assert(video_admission_allow(&s,2,2000000,-80,50,20000,0,100000,0)); assert(s.weak);
 assert(!video_admission_allow(&s,2,2100000,-80,50,20000,0,100000,0));
 assert(video_admission_allow(&s,2,3000000,0,50,20000,0,100000,0)); assert(s.weak);
 /* Continuous GOOD recovery; the first good sample cannot release hysteresis. */
 s.rssi_ewma=-55;
 video_admission_allow(&s,2,4000000,-55,50,20000,0,100000,0); assert(s.weak);
 video_admission_allow(&s,2,4500000,-55,50,20000,0,100000,0); assert(s.weak);
 video_admission_allow(&s,2,5000000,-55,50,20000,0,100000,0); assert(!s.weak);
 video_admission_allow(&s,3,6000000,-55,400,20000,0,100000,0); assert(s.weak);
 /* Policy switches reset EWMA/hold state; unknown RSSI does not trip BAD. */
 video_admission_allow(&s,2,7000000,0,50,20000,0,100000,0); assert(!s.weak);
 return 0;
}
