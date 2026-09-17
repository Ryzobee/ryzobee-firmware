#include "ryz_ble_store.h"
#include <string.h>

/* Explicit wire format, never raw NimBLE structs/bitfields/padding. Version 1,
 * fixed 256 bytes, one durable blob for preference, local IRK and single peer.
 * SDK upgrade must audit the codec and private controller adapter together. */
#define RECORD_SIZE 256U
#define RECORD_DATA 252U
#define RECORD_MAGIC UINT32_C(0x315a4252)
static const char *const STORE_NS = "ryz_ble";
static const char *const STORE_KEY = "record";

void ryz_ble_wipe(void *p, size_t n)
{ volatile unsigned char *b=p; while (n--) *b++=0; }

bool ryz_ble_addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{ return (a->type & 1U)==(b->type & 1U) && !memcmp(a->val,b->val,6); }

static void put(uint8_t **p, uint64_t v, unsigned n)
{ while (n--) { *(*p)++=(uint8_t)v; v>>=8; } }
static uint64_t get(const uint8_t **p, unsigned n)
{ uint64_t v=0; for (unsigned i=0;i<n;++i) v|=(uint64_t)*(*p)++<<(8U*i); return v; }
static void put_addr(uint8_t **p, const ble_addr_t *a)
{ put(p,a->type&1U,1); memcpy(*p,a->val,6); *p+=6; }
static void get_addr(const uint8_t **p, ble_addr_t *a)
{ a->type=(uint8_t)get(p,1); memcpy(a->val,*p,6); *p+=6; }
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c=UINT32_MAX;
    while (n--) { c^=*p++; for (unsigned i=0;i<8;++i) c=(c>>1)^((c&1U)?UINT32_C(0xedb88320):0); }
    return ~c;
}
static void put_sec(uint8_t **p, const struct ble_store_value_sec *s)
{
    put(p,s->key_size,1); put(p,s->ediv,2); put(p,s->rand_num,8);
    put(p,(s->ltk_present?1U:0U)|(s->irk_present?2U:0U)|(s->csrk_present?4U:0U)|
          (s->authenticated?8U:0U)|(s->sc?16U:0U),1);
    memcpy(*p,s->ltk,16); *p+=16; memcpy(*p,s->irk,16); *p+=16;
    memcpy(*p,s->csrk,16); *p+=16; put(p,s->sign_counter,4);
}
static void get_sec(const uint8_t **p, struct ble_store_value_sec *s, const ble_addr_t *peer)
{
    s->peer_addr=*peer; s->key_size=(uint8_t)get(p,1); s->ediv=(uint16_t)get(p,2); s->rand_num=get(p,8);
    unsigned f=(unsigned)get(p,1); s->ltk_present=!!(f&1U); s->irk_present=!!(f&2U);
    s->csrk_present=!!(f&4U); s->authenticated=!!(f&8U); s->sc=!!(f&16U);
    memcpy(s->ltk,*p,16); *p+=16; memcpy(s->irk,*p,16); *p+=16;
    memcpy(s->csrk,*p,16); *p+=16; s->sign_counter=(uint32_t)get(p,4);
}
static bool valid(const ryz_ble_record_t *r)
{
    if (r->local.addr.type>1 || r->peer.peer_addr.type>1 || r->cccd_count>RYZ_BLE_CCCD_MAX) return false;
    if (!r->bonded) return !r->our_valid && !r->peer_valid && !r->cccd_count && !r->csfc_valid;
    if (!r->local_valid || !r->our_valid || !r->peer_valid ||
        !r->our.authenticated || !r->peer.authenticated || !r->our.sc || !r->peer.sc ||
        r->our.key_size!=16 || r->peer.key_size!=16 || !r->our.ltk_present ||
        !ryz_ble_addr_equal(&r->our.peer_addr,&r->peer.peer_addr)) return false;
    for (unsigned i=0;i<r->cccd_count;++i)
        if (!r->cccd[i].chr_val_handle || !ryz_ble_addr_equal(&r->cccd[i].peer_addr,&r->peer.peer_addr)) return false;
    return true;
}
static esp_err_t encode(const ryz_ble_record_t *r, uint8_t bytes[RECORD_SIZE])
{
    if (!valid(r) || sizeof(r->csfc.csfc)>8) return ESP_ERR_INVALID_ARG;
    memset(bytes,0,RECORD_SIZE); uint8_t *p=bytes;
    put(&p,RECORD_MAGIC,4); put(&p,RECORD_SIZE,2);
    put(&p,(r->enabled?1U:0U)|(r->local_valid?2U:0U)|(r->bonded?4U:0U)|
           (r->our_valid?8U:0U)|(r->peer_valid?16U:0U)|(r->csfc_valid?32U:0U),1);
    put_addr(&p,&r->local.addr); memcpy(p,r->local.irk,16); p+=16;
    put_addr(&p,&r->peer.peer_addr); put_sec(&p,&r->our); put_sec(&p,&r->peer);
    put(&p,r->cccd_count,1);
    for (unsigned i=0;i<RYZ_BLE_CCCD_MAX;++i) {
        put(&p,r->cccd[i].chr_val_handle,2); put(&p,r->cccd[i].flags,2); put(&p,r->cccd[i].value_changed?1U:0U,1);
    }
    put(&p,sizeof(r->csfc.csfc),1); memcpy(p,r->csfc.csfc,sizeof(r->csfc.csfc));
    p=bytes+RECORD_DATA; put(&p,crc32(bytes,RECORD_DATA),4); return ESP_OK;
}
static esp_err_t decode(const uint8_t bytes[RECORD_SIZE], ryz_ble_record_t *r)
{
    const uint8_t *p=bytes+RECORD_DATA;
    if ((uint32_t)get(&p,4)!=crc32(bytes,RECORD_DATA)) return ESP_ERR_INVALID_RESPONSE;
    p=bytes;
    if (get(&p,4)!=RECORD_MAGIC || get(&p,2)!=RECORD_SIZE) return ESP_ERR_INVALID_RESPONSE;
    unsigned f=(unsigned)get(&p,1); if (f&~63U) return ESP_ERR_INVALID_RESPONSE;
    r->enabled=!!(f&1U); r->local_valid=!!(f&2U); r->bonded=!!(f&4U);
    r->our_valid=!!(f&8U); r->peer_valid=!!(f&16U); r->csfc_valid=!!(f&32U);
    get_addr(&p,&r->local.addr); memcpy(r->local.irk,p,16); p+=16;
    ble_addr_t peer={0}; get_addr(&p,&peer); get_sec(&p,&r->our,&peer); get_sec(&p,&r->peer,&peer);
    r->cccd_count=(uint8_t)get(&p,1);
    for (unsigned i=0;i<RYZ_BLE_CCCD_MAX;++i) {
        r->cccd[i].peer_addr=peer; r->cccd[i].chr_val_handle=(uint16_t)get(&p,2);
        r->cccd[i].flags=(uint16_t)get(&p,2); r->cccd[i].value_changed=get(&p,1)!=0;
    }
    if (get(&p,1)!=sizeof(r->csfc.csfc) || sizeof(r->csfc.csfc)>8) return ESP_ERR_INVALID_RESPONSE;
    r->csfc.peer_addr=peer; memcpy(r->csfc.csfc,p,sizeof(r->csfc.csfc));
    return valid(r)?ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
esp_err_t ryz_ble_record_load(ryz_ble_record_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out)); uint8_t bytes[RECORD_SIZE]={0};
    nvs_handle_t h=0; esp_err_t e=nvs_open(STORE_NS,NVS_READONLY,&h);
    if (e==ESP_ERR_NVS_NOT_FOUND) { out->enabled=true; return ESP_OK; }
    if (e!=ESP_OK) return e;
    size_t n=sizeof(bytes); e=nvs_get_blob(h,STORE_KEY,bytes,&n); nvs_close(h);
    if (e==ESP_ERR_NVS_NOT_FOUND) { out->enabled=true; e=ESP_OK; }
    else if (e==ESP_OK) e=n==RECORD_SIZE?decode(bytes,out):ESP_ERR_INVALID_RESPONSE;
    ryz_ble_wipe(bytes,sizeof(bytes));
    if (e!=ESP_OK) memset(out,0,sizeof(*out));
    return e;
}
esp_err_t ryz_ble_record_save(const ryz_ble_record_t *r)
{
    uint8_t bytes[RECORD_SIZE]={0}, check[RECORD_SIZE]={0};
    esp_err_t e=encode(r,bytes); nvs_handle_t h=0;
    if (e==ESP_OK) e=nvs_open(STORE_NS,NVS_READWRITE,&h);
    if (e==ESP_OK) {
        e=nvs_set_blob(h,STORE_KEY,bytes,sizeof(bytes));
        if (e==ESP_OK) e=nvs_commit(h);
        nvs_close(h);
    }
    /* A successful return requires both SDK commit acknowledgement and an
     * independent handle returning exactly the complete expected blob. */
    if (e==ESP_OK) {
        e=nvs_open(STORE_NS,NVS_READONLY,&h);
        if (e==ESP_OK) {
            size_t n=sizeof(check); e=nvs_get_blob(h,STORE_KEY,check,&n); nvs_close(h);
            if (e==ESP_OK && (n!=sizeof(check) || memcmp(bytes,check,sizeof(bytes)))) e=ESP_ERR_INVALID_RESPONSE;
        }
    }
    ryz_ble_wipe(bytes,sizeof(bytes)); ryz_ble_wipe(check,sizeof(check)); return e;
}
void ryz_ble_record_clear_peer(ryz_ble_record_t *r)
{
    r->bonded=r->our_valid=r->peer_valid=r->csfc_valid=false; r->cccd_count=0;
    ryz_ble_wipe(&r->our,sizeof(r->our)); ryz_ble_wipe(&r->peer,sizeof(r->peer));
    ryz_ble_wipe(r->cccd,sizeof(r->cccd)); ryz_ble_wipe(&r->csfc,sizeof(r->csfc));
}
