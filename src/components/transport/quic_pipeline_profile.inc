/* Optional nested CPU/wall accounting. Scope is only the image DATAGRAM writer. */
#include "esp_private/wifi.h"
static bool s_pipe_scope;
static struct {uint64_t aead_cpu, aead_wall, aead_calls, hp_cpu, hp_wall, hp_calls;
    uint64_t packet_cpu, udp_cpu, socket_cpu, socket_wall, socket_calls;} s_pipe;
static int pipe_encrypt(uint8_t *dest,const ngtcp2_crypto_aead *aead,
 const ngtcp2_crypto_aead_ctx *ctx,const uint8_t *plain,size_t len,
 const uint8_t *nonce,size_t noncelen,const uint8_t *aad,size_t aadlen)
{
    uint32_t c=aiot_task_runtime_now();int64_t t=esp_timer_get_time();
    int ret=ngtcp2_crypto_encrypt_cb(dest,aead,ctx,plain,len,nonce,noncelen,aad,aadlen);
    if(s_pipe_scope){s_pipe.aead_wall+=esp_timer_get_time()-t;s_pipe.aead_cpu+=(uint32_t)(aiot_task_runtime_now()-c);++s_pipe.aead_calls;}
    return ret;
}
static int pipe_hp(uint8_t *dest,const ngtcp2_crypto_cipher *hp,const ngtcp2_crypto_cipher_ctx *ctx,const uint8_t *sample)
{
    uint32_t c=aiot_task_runtime_now();int64_t t=esp_timer_get_time();
    int ret=ngtcp2_crypto_hp_mask_cb(dest,hp,ctx,sample);
    if(s_pipe_scope){s_pipe.hp_wall+=esp_timer_get_time()-t;s_pipe.hp_cpu+=(uint32_t)(aiot_task_runtime_now()-c);++s_pipe.hp_calls;}
    return ret;
}
/* Match encrypted UDP payloads, without retaining image content or key material.
 * TX-done latency includes lwIP, Wi-Fi queue, airtime and MAC retries. */
static portMUX_TYPE s_wifi_probe_mux=portMUX_INITIALIZER_UNLOCKED;
static struct {bool used;uint16_t len;uint8_t head[16],tail[8];int64_t start;} s_wifi_pending[128];
static uint64_t s_wifi_registered,s_wifi_callbacks,s_wifi_matched,s_wifi_failed,s_wifi_us,s_wifi_max_us,s_wifi_overwritten;
static void pipe_wifi_done(uint8_t ifidx,uint8_t *data,uint16_t *length,bool status)
{
    (void)ifidx;
    if(!data || !length)return;
    size_t n=*length;const uint8_t *payload=NULL;size_t plen=0;
    /* Driver frames may include Ethernet/LLC headers. Validate a bounded IPv4
     * UDP candidate and then require an exact 24-byte encrypted fingerprint. */
    for(size_t o=0;o<64 && o+28<=n;++o){
        if((data[o]>>4)!=4 || (data[o]&15)<5 || data[o+9]!=17)continue;
        size_t ihl=(data[o]&15)*4,u=o+ihl;
        if(u+8>n || ((unsigned)data[u+2]*256+data[u+3])!=14567)continue;
        size_t ul=(unsigned)data[u+4]*256+data[u+5];
        if(ul<32 || u+ul>n)continue;
        payload=data+u+8;plen=ul-8;break;
    }
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_wifi_probe_mux);++s_wifi_callbacks;
    if(payload)for(unsigned i=0;i<128;++i){
        if(!s_wifi_pending[i].used || s_wifi_pending[i].len!=plen ||
           memcmp(s_wifi_pending[i].head,payload,16) || memcmp(s_wifi_pending[i].tail,payload+plen-8,8))continue;
        uint64_t elapsed=now-s_wifi_pending[i].start;s_wifi_pending[i].used=false;
        ++s_wifi_matched;s_wifi_failed+=!status;s_wifi_us+=elapsed;
        if(elapsed>s_wifi_max_us)s_wifi_max_us=elapsed;
        break;
    }
    portEXIT_CRITICAL(&s_wifi_probe_mux);
}
static void pipe_wifi_track(const uint8_t *data,size_t len)
{
    static unsigned next;static bool attempted;
    if(!attempted){attempted=true;s_wifi_registered=esp_wifi_set_tx_done_cb(pipe_wifi_done)==ESP_OK;}
    if(!s_wifi_registered || len<24)return;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_wifi_probe_mux);
    unsigned i=next++%128;s_wifi_overwritten+=s_wifi_pending[i].used;
    s_wifi_pending[i].used=true;s_wifi_pending[i].len=len;s_wifi_pending[i].start=now;
    memcpy(s_wifi_pending[i].head,data,16);memcpy(s_wifi_pending[i].tail,data+len-8,8);
    portEXIT_CRITICAL(&s_wifi_probe_mux);
}
static void pipe_report(int64_t now)
{
    uint64_t cb,matched,failed,us,maxus,overwritten;
    portENTER_CRITICAL(&s_wifi_probe_mux);
    cb=s_wifi_callbacks;matched=s_wifi_matched;failed=s_wifi_failed;us=s_wifi_us;maxus=s_wifi_max_us;overwritten=s_wifi_overwritten;
    portEXIT_CRITICAL(&s_wifi_probe_mux);
    printf("[PIPE_PROFILE] {\"us\":%lld,\"aead_cpu\":%llu,\"aead_wall\":%llu,\"aead_calls\":%llu,\"hp_cpu\":%llu,\"hp_wall\":%llu,\"hp_calls\":%llu,\"packet_cpu\":%llu,\"udp_cpu\":%llu,\"socket_cpu\":%llu,\"socket_wall\":%llu,\"socket_calls\":%llu,\"wifi_registered\":%llu,\"wifi_callbacks\":%llu,\"wifi_matched\":%llu,\"wifi_failed\":%llu,\"wifi_us\":%llu,\"wifi_max_us\":%llu,\"wifi_overwritten\":%llu}\n",
      (long long)now,(unsigned long long)s_pipe.aead_cpu,(unsigned long long)s_pipe.aead_wall,(unsigned long long)s_pipe.aead_calls,
      (unsigned long long)s_pipe.hp_cpu,(unsigned long long)s_pipe.hp_wall,(unsigned long long)s_pipe.hp_calls,
      (unsigned long long)s_pipe.packet_cpu,(unsigned long long)s_pipe.udp_cpu,(unsigned long long)s_pipe.socket_cpu,
      (unsigned long long)s_pipe.socket_wall,(unsigned long long)s_pipe.socket_calls,(unsigned long long)s_wifi_registered,
      (unsigned long long)cb,(unsigned long long)matched,(unsigned long long)failed,(unsigned long long)us,(unsigned long long)maxus,(unsigned long long)overwritten);
}
