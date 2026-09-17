/* Real durable-record codec; NVS is a controlled external Adapter. A successful
 * transaction here is not real flash power-loss or physical radio evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ryz_ble_store.h"

enum { BLOB_SIZE = 256, DATA_SIZE = 252 };
typedef enum { FAIL_NONE, FAIL_OPEN_READ, FAIL_OPEN_WRITE, FAIL_SET, FAIL_COMMIT,
               FAIL_COMMIT_AFTER, FAIL_READ, FAIL_READBACK, FAIL_READBACK_OPEN,
               FAIL_READBACK_SHORT, FAIL_READBACK_DIFFERENT } failure_t;
static failure_t failure;
static bool namespace_present, key_present;
static uint8_t durable[BLOB_SIZE];
static size_t durable_size;
static unsigned opens, closes, sets, commits, blob_reads, next_handle, live;
static struct { bool open, pending; int mode; uint8_t staged[BLOB_SIZE]; } handles[128];

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "ryz_ble") && out && (mode == NVS_READONLY || mode == NVS_READWRITE));
    if ((mode == NVS_READWRITE && failure == FAIL_OPEN_WRITE) ||
        (mode == NVS_READONLY && (failure == FAIL_OPEN_READ ||
         (failure == FAIL_READBACK_OPEN && commits)))) return ESP_ERR_NO_MEM;
    if (mode == NVS_READONLY && !namespace_present) return ESP_ERR_NVS_NOT_FOUND;
    assert(!live && next_handle + 1U < sizeof(handles) / sizeof(handles[0]));
    namespace_present = true;
    *out = ++next_handle;
    handles[*out].open = true; handles[*out].mode = mode;
    ++opens; ++live;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    assert(h <= next_handle && handles[h].open && handles[h].mode == NVS_READWRITE);
    assert(!strcmp(key, "record") && data && size == BLOB_SIZE);
    ++sets;
    if (failure == FAIL_SET) return ESP_ERR_NO_MEM;
    memcpy(handles[h].staged, data, size); handles[h].pending = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h <= next_handle && handles[h].open && handles[h].mode == NVS_READWRITE && handles[h].pending);
    ++commits;
    if (failure == FAIL_COMMIT) return ESP_FAIL;
    memcpy(durable, handles[h].staged, BLOB_SIZE); durable_size = BLOB_SIZE; key_present = true;
    handles[h].pending = false;
    return failure == FAIL_COMMIT_AFTER ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    assert(h <= next_handle && handles[h].open && handles[h].mode == NVS_READONLY);
    assert(!strcmp(key, "record") && data && size && *size == BLOB_SIZE);
    ++blob_reads;
    if (failure == FAIL_READ || (failure == FAIL_READBACK && commits)) {
        memset(data, 0x55, *size); /* Failed reads must never expose dirty output. */
        return ESP_FAIL;
    }
    if (!key_present) return ESP_ERR_NVS_NOT_FOUND;
    if (durable_size > *size) { *size = durable_size; return ESP_ERR_INVALID_SIZE; }
    memcpy(data, durable, durable_size); *size = durable_size;
    if (failure == FAIL_READBACK_SHORT && commits) --*size;
    if (failure == FAIL_READBACK_DIFFERENT && commits) ((uint8_t *)data)[17] ^= 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    assert(h <= next_handle && handles[h].open && live == 1);
    handles[h].open = handles[h].pending = false;
    memset(handles[h].staged, 0, sizeof(handles[h].staged));
    ++closes; --live;
}

static void reset(void)
{
    assert(!live && opens == closes);
    failure = FAIL_NONE; namespace_present = key_present = false;
    memset(durable, 0, sizeof(durable)); durable_size = 0;
    memset(handles, 0, sizeof(handles)); opens = closes = sets = commits = blob_reads = next_handle = 0;
}

static void clean_handles(void)
{ assert(!live && opens == closes); }

static void bytes_zero(const void *value, size_t size)
{
    const uint8_t *p = value;
    for (size_t i = 0; i < size; ++i) assert(p[i] == 0);
}

static ryz_ble_record_t full_record(void)
{
    ryz_ble_record_t r = {.enabled = true, .local_valid = true, .bonded = true,
        .our_valid = true, .peer_valid = true, .cccd_count = RYZ_BLE_CCCD_MAX, .csfc_valid = true};
    r.local.addr = (ble_addr_t){.type = BLE_ADDR_PUBLIC, .val = {1,2,3,4,5,6}};
    const ble_addr_t peer = {.type = BLE_ADDR_RANDOM, .val = {9,8,7,6,5,0xc4}};
    for (unsigned i = 0; i < 16; ++i) r.local.irk[i] = (uint8_t)(0x10 + i);
    struct ble_store_value_sec *keys[] = {&r.our, &r.peer};
    for (unsigned side = 0; side < 2; ++side) {
        struct ble_store_value_sec *s = keys[side];
        s->peer_addr = peer; s->key_size = 16; s->authenticated = s->sc = 1;
        s->ltk_present = s->irk_present = s->csrk_present = 1;
        s->ediv = (uint16_t)(0x4321 + side); s->rand_num = UINT64_C(0xfedcba9876543210) + side;
        s->sign_counter = UINT32_C(0xf1234567) + side;
        for (unsigned i = 0; i < 16; ++i) {
            s->ltk[i] = (uint8_t)(0x20 + i + side); s->irk[i] = (uint8_t)(0x50 + i + side);
            s->csrk[i] = (uint8_t)(0x90 + i + side);
        }
    }
    for (unsigned i = 0; i < r.cccd_count; ++i) {
        r.cccd[i].peer_addr = peer; r.cccd[i].chr_val_handle = (uint16_t)(0x3412 + i);
        r.cccd[i].flags = (uint16_t)(0x2340 + i); r.cccd[i].value_changed = i & 1U;
    }
    r.csfc.peer_addr = peer; memset(r.csfc.csfc, 0xa5, sizeof(r.csfc.csfc));
    return r;
}

static void same_sec(const struct ble_store_value_sec *a, const struct ble_store_value_sec *b)
{
    assert(ryz_ble_addr_equal(&a->peer_addr, &b->peer_addr));
    assert(a->key_size == b->key_size && a->ediv == b->ediv && a->rand_num == b->rand_num);
    assert(a->ltk_present == b->ltk_present && a->irk_present == b->irk_present && a->csrk_present == b->csrk_present);
    assert(a->authenticated == b->authenticated && a->sc == b->sc && a->sign_counter == b->sign_counter);
    assert(!memcmp(a->ltk, b->ltk, 16) && !memcmp(a->irk, b->irk, 16) && !memcmp(a->csrk, b->csrk, 16));
}

static void same_record(const ryz_ble_record_t *a, const ryz_ble_record_t *b)
{
    assert(a->enabled == b->enabled && a->local_valid == b->local_valid && a->bonded == b->bonded);
    assert(a->our_valid == b->our_valid && a->peer_valid == b->peer_valid && a->csfc_valid == b->csfc_valid);
    assert(ryz_ble_addr_equal(&a->local.addr, &b->local.addr) && !memcmp(a->local.irk, b->local.irk, 16));
    same_sec(&a->our, &b->our); same_sec(&a->peer, &b->peer);
    assert(a->cccd_count == b->cccd_count);
    for (unsigned i = 0; i < a->cccd_count; ++i) {
        assert(ryz_ble_addr_equal(&a->cccd[i].peer_addr, &b->cccd[i].peer_addr));
        assert(a->cccd[i].chr_val_handle == b->cccd[i].chr_val_handle);
        assert(a->cccd[i].flags == b->cccd[i].flags && a->cccd[i].value_changed == b->cccd[i].value_changed);
    }
    if (a->csfc_valid) assert(ryz_ble_addr_equal(&a->csfc.peer_addr, &b->csfc.peer_addr));
    assert(!memcmp(a->csfc.csfc, b->csfc.csfc, sizeof(a->csfc.csfc)));
}

static void rewrite_crc(void)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < DATA_SIZE; ++i) {
        crc ^= durable[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) durable[DATA_SIZE + i] = (uint8_t)(crc >> (8U * i));
}

static void roundtrip(void)
{
    ryz_ble_record_t r = full_record(), out;
    assert(ryz_ble_record_save(&r) == ESP_OK && durable_size == BLOB_SIZE);
    assert(opens == 2 && closes == 2 && commits == 1 && sets == 1 && blob_reads == 1);
    assert(durable[0] == 0x52 && durable[1] == 0x42 && durable[2] == 0x5a && durable[3] == 0x31);
    assert(durable[4] == 0 && durable[5] == 1); /* Wire size 256, little endian. */
    assert(ryz_ble_record_load(&out) == ESP_OK); same_record(&r, &out);
    uint8_t original[BLOB_SIZE]; memcpy(original, durable, sizeof(original));
    assert(ryz_ble_record_save(&out) == ESP_OK && !memcmp(original, durable, sizeof(original)));
    r.enabled = false;
    assert(ryz_ble_record_save(&r) == ESP_OK && ryz_ble_record_load(&out) == ESP_OK);
    same_record(&r, &out); assert(out.bonded && !out.enabled);
    r.our.irk_present = r.peer.csrk_present = 0; /* Optional flag bits round-trip independently. */
    assert(ryz_ble_record_save(&r) == ESP_OK && ryz_ble_record_load(&out) == ESP_OK); same_record(&r, &out);
    ryz_ble_record_clear_peer(&r);
    assert(!r.enabled && r.local_valid && !r.bonded && !r.our_valid && !r.peer_valid && !r.csfc_valid && !r.cccd_count);
    bytes_zero(&r.our, sizeof(r.our)); bytes_zero(&r.peer, sizeof(r.peer));
    bytes_zero(r.cccd, sizeof(r.cccd)); bytes_zero(&r.csfc, sizeof(r.csfc));
    assert(ryz_ble_record_save(&r) == ESP_OK && ryz_ble_record_load(&out) == ESP_OK); same_record(&r, &out);
    for (unsigned i = 0; i < 16; ++i) assert(out.local.irk[i] == 0x10 + i);
    clean_handles();
}

static void invalid_records(void)
{
    for (unsigned which = 0; which < 12; ++which) {
        ryz_ble_record_t r = full_record();
        switch (which) {
        case 0: r.our.key_size = 15; break;
        case 1: r.peer.key_size = 7; break;
        case 2: r.our.authenticated = 0; break;
        case 3: r.peer.authenticated = 0; break;
        case 4: r.our.sc = 0; break;
        case 5: r.peer.sc = 0; break;
        case 6: r.our.ltk_present = 0; break;
        case 7: r.local_valid = false; break;
        case 8: r.peer.peer_addr.val[1] ^= 1; break;
        case 9: r.cccd_count = RYZ_BLE_CCCD_MAX + 1; break;
        case 10: r.cccd[0].chr_val_handle = 0; break;
        case 11: r.cccd[0].peer_addr.val[1] ^= 1; break;
        }
        assert(ryz_ble_record_save(&r) == ESP_ERR_INVALID_ARG);
        assert(!opens && !sets && !commits && !blob_reads); clean_handles();
    }
}

static void corruption(void)
{
    for (unsigned which = 0; which < 10; ++which) {
        reset(); ryz_ble_record_t r = full_record(), out;
        assert(ryz_ble_record_save(&r) == ESP_OK);
        switch (which) {
        case 0: durable[20] ^= 1; break; /* CRC mismatch. */
        case 1: durable[3] = 0x32; rewrite_crc(); break; /* Unknown version/magic. */
        case 2: durable[4] ^= 1; rewrite_crc(); break;
        case 3: durable[6] |= 0x80; rewrite_crc(); break;
        case 4: durable[37] = 15; rewrite_crc(); break; /* OUR key size. */
        case 5: durable[48] &= (uint8_t)~8U; rewrite_crc(); break; /* OUR authentication. */
        case 6: durable[112] &= (uint8_t)~16U; rewrite_crc(); break; /* PEER SC. */
        case 7: durable[165] = RYZ_BLE_CCCD_MAX + 1; rewrite_crc(); break;
        case 8: durable[186] ^= 1; rewrite_crc(); break; /* SDK CSFC width. */
        case 9: --durable_size; break;
        }
        memset(&out, 0x55, sizeof(out));
        assert(ryz_ble_record_load(&out) == ESP_ERR_INVALID_RESPONSE);
        bytes_zero(&out, sizeof(out)); clean_handles();
    }
}

static void load_failures(void)
{
    ryz_ble_record_t out;
    assert(ryz_ble_record_load(NULL) == ESP_ERR_INVALID_ARG && !opens);
    assert(ryz_ble_record_load(&out) == ESP_OK && out.enabled && !out.local_valid && !out.bonded);
    assert(!opens && !closes);
    namespace_present = true;
    assert(ryz_ble_record_load(&out) == ESP_OK && out.enabled && !out.bonded); clean_handles();
    failure = FAIL_OPEN_READ; memset(&out, 0x55, sizeof(out));
    assert(ryz_ble_record_load(&out) == ESP_ERR_NO_MEM); bytes_zero(&out, sizeof(out)); clean_handles();
    failure = FAIL_READ; memset(&out, 0x55, sizeof(out));
    assert(ryz_ble_record_load(&out) == ESP_FAIL); bytes_zero(&out, sizeof(out)); clean_handles();
}

static void save_failures(void)
{
    const failure_t faults[] = {FAIL_OPEN_WRITE, FAIL_SET, FAIL_COMMIT, FAIL_COMMIT_AFTER,
        FAIL_READBACK_OPEN, FAIL_READBACK, FAIL_READBACK_SHORT, FAIL_READBACK_DIFFERENT};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        reset(); ryz_ble_record_t previous = full_record();
        assert(ryz_ble_record_save(&previous) == ESP_OK);
        const unsigned old_commits = commits;
        ryz_ble_record_t desired = previous; desired.enabled = false;
        failure = faults[i];
        const esp_err_t error = ryz_ble_record_save(&desired);
        assert(error != ESP_OK && !desired.enabled && desired.bonded); clean_handles();
        if (failure == FAIL_OPEN_WRITE || failure == FAIL_SET) assert(commits == old_commits);
        const bool persisted = failure == FAIL_COMMIT_AFTER || failure == FAIL_READBACK_OPEN ||
            failure == FAIL_READBACK || failure == FAIL_READBACK_SHORT || failure == FAIL_READBACK_DIFFERENT;
        failure = FAIL_NONE;
        ryz_ble_record_t actual;
        assert(ryz_ble_record_load(&actual) == ESP_OK);
        same_record(persisted ? &desired : &previous, &actual);
        clean_handles();
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "roundtrip")) roundtrip();
    else if (!strcmp(argv[1], "invalid")) invalid_records();
    else if (!strcmp(argv[1], "corruption")) corruption();
    else if (!strcmp(argv[1], "load-errors")) load_failures();
    else if (!strcmp(argv[1], "save-errors")) save_failures();
    else assert(!"unknown test case");
    clean_handles();
    printf("BLE_STORE_CODEC_PASS %s\n", argv[1]);
    return 0;
}
