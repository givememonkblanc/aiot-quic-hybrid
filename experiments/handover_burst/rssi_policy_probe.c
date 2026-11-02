/* Decision-path replay only: no radio, QUIC or camera simulation. */
#include "video_admission.h"
#include <stdio.h>
int main(void) {
 puts("policy,phase,attempts,admitted,weak_at_end");
 for(unsigned policy=1;policy<=3;policy++) {
  video_admission_t s={0};
  for(unsigned phase=0;phase<3;phase++) {
   unsigned admitted=0;
   for(unsigned n=0;n<100;n++) {
    unsigned long long t=1000000ULL+(phase*100ULL+n)*100000ULL;
    admitted+=video_admission_allow(&s,policy,t,phase==1?-82:-55,40,262144,0,35000,0);
   }
   printf("%u,%s,100,%u,%d\n",policy,phase==0?"healthy":phase==1?"weak":"recovery",admitted,s.weak);
  }
 }
 return 0;
}
