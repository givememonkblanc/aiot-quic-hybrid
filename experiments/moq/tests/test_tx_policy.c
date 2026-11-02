#include "moqt_tx_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 uint64_t queued[6]={0,0,0,10,10,11};
 assert(moqt_tx_oldest_ready(0,queued)==-1);
 assert(moqt_tx_oldest_ready(0x30,queued)==4);
 /* Recycle slot4 after completing its first object. Waiting slot5 must win. */
 queued[4]=12;
 assert(moqt_tx_oldest_ready(0x30,queued)==5);
 /* AI still precedes video; a FIN-submitted video is excluded by the mask. */
 assert(moqt_tx_oldest_ready(0x38,queued)==3);
 assert(moqt_tx_oldest_ready(0x10,queued)==4);
 /* A sequence of admissions alternates service, independent of slot index. */
 for(uint64_t t=13;t<100;t++){
  int i=moqt_tx_oldest_ready(0x30,queued);int other=i==4?5:4;
  assert(queued[i]<=queued[other]);queued[i]=t;
  assert(moqt_tx_oldest_ready(0x30,queued)==other);
 }
 assert(moqt_preview_allowance(0,999,0,100)==SIZE_MAX);
 assert(moqt_preview_allowance(4096,1024,0,100)==3072);
 assert(moqt_preview_allowance(4096,4096,0,100)==0);
 assert(moqt_preview_allowance(4096,0,101,100)==0);
 assert(moqt_preview_pace_advance(0,1000000,1000,500)==1016000);
 assert(moqt_preview_pace_advance(1016000,1005000,500,500)==1024000);
 puts("PASS: recycled JPEG slot cannot overtake older pending image; AI priority preserved");
}
