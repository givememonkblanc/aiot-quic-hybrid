import pathlib,subprocess,tempfile,unittest
class FrameTests(unittest.TestCase):
 def test_loss_and_connection_boundaries(self):
  source=pathlib.Path('host_receiver/picoquic/picohttp/demoserver.c').read_text();a=source.index('static void aiot_image_reassemble(');b=source.index('\n/*',a+1)
  c='''#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define PICOQUIC_CONNECTION_ID_MAX_SIZE 20
#define AIOT_IMG_MAX 262144
static uint8_t aiot_img_buf[AIOT_IMG_MAX]; static size_t aiot_img_len; static int complete;
static uint8_t published[AIOT_IMG_MAX]; static size_t published_len;
static void aiot_image_write(const uint8_t*p,size_t n){complete++;published_len=n;memcpy(published,p,n);}
static void aiot_image_archive(const uint8_t*p,size_t n,uint32_t t){}
'''+source[a:b]+'''
int main(void){
uint8_t start[]={255,216,1}, end[]={2,255,217};
aiot_image_reassemble(start,3,1,1,"a");aiot_image_reassemble(end,3,1,2,"a");assert(complete==1);
aiot_image_reassemble(start,3,2,3,"a");aiot_image_reassemble(end,3,2,5,"a");assert(complete==1);
aiot_image_reassemble(start,3,3,6,"a");aiot_image_reassemble(end,3,3,7,"b");assert(complete==1);
aiot_image_reassemble(start,3,4,8,"a");aiot_image_reassemble(end,3,5,9,"a");assert(complete==1);
aiot_image_reassemble(start,3,6,10,"a");aiot_image_reassemble(end,3,6,11,"a");assert(complete==2);
/* Preserve large JPEG-sized payloads byte-for-byte, beyond uint16_t range. */
static uint8_t large[100000];memset(large,0x11,sizeof(large));
large[0]=255;large[1]=216;large[sizeof(large)-2]=255;large[sizeof(large)-1]=217;
uint32_t seq=100;
for(size_t off=0;off<sizeof(large);off+=1024){size_t n=sizeof(large)-off;if(n>1024)n=1024;
 aiot_image_reassemble(large+off,n,7,seq++,"a");}
assert(complete==3);assert(published_len==sizeof(large));assert(!memcmp(large,published,sizeof(large)));
/* Buffer-cap overflow must not publish, and the next frame must recover. */
aiot_image_reassemble(start,3,8,seq++,"a");
for(int i=0;i<257;i++)aiot_image_reassemble(large+2,1024,8,seq++,"a");
aiot_image_reassemble(end,3,8,seq++,"a");assert(complete==3);
aiot_image_reassemble(start,3,9,seq++,"a");aiot_image_reassemble(end,3,9,seq++,"a");assert(complete==4);
return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'test.c').write_text(c);subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
