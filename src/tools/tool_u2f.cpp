/* tool_u2f.cpp — U2F (FIDO CTAP1) security key.
 *
 * Self-contained by design: U2F has no imp_* API family in the frozen
 * contract, so the whole engine — CTAPHID transport on the framework's
 * raw HID, CTAP1 APDUs, P-256 crypto via mbedTLS — lives in this one
 * tool file. No other tool touches USB/HID.
 *
 * Transport: a USBHIDDevice with the FIDO report descriptor (usage
 * page 0xF1D0, usage 1, 64-byte IN/OUT, NO report ID — CTAPHID hosts
 * expect raw 64-byte packets; SendReport(0, ...) sends unprefixed).
 * _onOutput() runs in USB-task context and only copies the packet
 * into a ring; all processing happens in the tool loop.
 *
 * Keys: per-registration P-256 keypair. The key handle wraps the
 * private scalar: handle = (priv XOR HMAC(secret,"mask"|appId)) ||
 * HMAC(secret,"tag"|masked|appId), 64 bytes. The device secret is
 * generated on first use and stored on SD at /u2f/secret.bin.
 * KNOWN V1 LIMITATIONS (bring-up hardening items, stated honestly):
 *  - XOR+HMAC wrap is a v1 construction, not AES-GCM key wrapping.
 *  - Without a card the secret is RAM-only: handles die at reboot.
 *  - The attestation certificate is a real self-signed X.509 cert,
 *    but hand-encoded below (this SDK ships no mbedtls x509write),
 *    with a fixed 2025-2045 validity window; strict FIDO attestation
 *    checkers may still reject a self-signed root, which is normal
 *    for a homebrew key.
 * Counters persist on SD (/u2f/state.bin) when a card is fitted.
 * User presence: REGISTER and sign AUTHENTICATE park the request;
 * the tool loop blinks the LED and waits for the encoder OK press
 * (10 s timeout -> SW 0x6985). BACK cancels a parked request, and
 * exits when none is parked. The HID device persists until reboot.
 */
#include "imp_api.h"
#include "USB.h"
#include "USBHID.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"

/* ================================================================== */
/* RNG                                                                 */
/* ================================================================== */
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static bool g_rng_ready = false;

static bool rng_init(void) {
    if (g_rng_ready) return true;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    const char *pers = "imp1-u2f";
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return false;
    g_rng_ready = true;
    return true;
}

static int rng_random(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    return mbedtls_ctr_drbg_random(&g_drbg, buf, len);
}

/* ================================================================== */
/* Crypto helpers                                                      */
/* ================================================================== */
static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_ret(data, len, out, 0);
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len, uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) == 0) {
        mbedtls_md_hmac_starts(&ctx, key, key_len);
        mbedtls_md_hmac_update(&ctx, data, data_len);
        mbedtls_md_hmac_finish(&ctx, out);
    }
    mbedtls_md_free(&ctx);
}

/* DER-encode an ECDSA signature from raw r,s. Returns DER length. */
static size_t ecdsa_der_encode(const mbedtls_mpi *r, const mbedtls_mpi *s, uint8_t *out) {
    uint8_t rb[33], sb[33];
    size_t rn = 0, sn = 0;
    /* strip to minimal big-endian, add 0x00 pad if top bit set */
    uint8_t tmp[32];
    mbedtls_mpi_write_binary(r, tmp, 32);
    int i = 0; while (i < 31 && tmp[i] == 0) i++;
    if (tmp[i] & 0x80) rb[rn++] = 0;
    memcpy(rb + rn, tmp + i, 32 - i); rn += 32 - i;
    mbedtls_mpi_write_binary(s, tmp, 32);
    i = 0; while (i < 31 && tmp[i] == 0) i++;
    if (tmp[i] & 0x80) sb[sn++] = 0;
    memcpy(sb + sn, tmp + i, 32 - i); sn += 32 - i;
    size_t body = (2 + rn) + (2 + sn);
    out[0] = 0x30;
    out[1] = (uint8_t)body;
    out[2] = 0x02; out[3] = (uint8_t)rn; memcpy(out + 4, rb, rn);
    out[4 + rn] = 0x02; out[5 + rn] = (uint8_t)sn; memcpy(out + 6 + rn, sb, sn);
    return 2 + body;
}

/* Sign a 32-byte hash with a raw P-256 private scalar. DER out. */
static size_t ecdsa_sign_hash(const uint8_t priv[32], const uint8_t hash[32], uint8_t *der_out) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    size_t der_len = 0;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, priv, 32) == 0 &&
        mbedtls_ecdsa_sign(&grp, &r, &s, &d, hash, 32, rng_random, &g_drbg) == 0) {
        der_len = ecdsa_der_encode(&r, &s, der_out);
    }
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);
    return der_len;
}

static bool ec_genkey(uint8_t priv[32], uint8_t pub65[65]) {
    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    bool ok = false;
    if (mbedtls_ecdsa_genkey(&ctx, MBEDTLS_ECP_DP_SECP256R1, rng_random, &g_drbg) == 0) {
        size_t olen = 0;
        if (mbedtls_mpi_write_binary(&ctx.d, priv, 32) == 0 &&
            mbedtls_ecp_point_write_binary(&ctx.grp, &ctx.Q,
                                           MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, pub65, 65) == 0 && olen == 65)
            ok = true;
    }
    mbedtls_ecdsa_free(&ctx);
    return ok;
}

static bool ec_pub_from_priv(const uint8_t priv[32], uint8_t pub65[65]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi d;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&d);
    bool ok = false;
    size_t olen = 0;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, priv, 32) == 0 &&
        mbedtls_ecp_mul(&grp, &q, &d, &grp.G, rng_random, &g_drbg) == 0 &&
        mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, pub65, 65) == 0 && olen == 65)
        ok = true;
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

/* ================================================================== */
/* Device secret, attestation key, counters (SD-backed when fitted)    */
/* ================================================================== */
static uint8_t g_secret[32];
static uint8_t g_attest_priv[32];
static uint8_t g_attest_cert[384];
static size_t g_attest_cert_len = 0;
static uint32_t g_sig_counter = 0;
static uint32_t g_reg_count = 0;
static bool g_persistent = false;

static bool sd_read_file(const char *path, uint8_t *buf, size_t len) {
    imp_file_t *f = imp_storage_open(path, "r");
    if (!f) return false;
    size_t n = imp_storage_read(f, buf, len);
    imp_storage_close(f);
    return n == len;
}

static bool sd_write_file(const char *path, const uint8_t *buf, size_t len) {
    imp_file_t *f = imp_storage_open(path, "w");
    if (!f) return false;
    size_t n = imp_storage_write(f, buf, len);
    imp_storage_close(f);
    return n == len;
}

static void state_save(void) {
    if (!g_persistent) return;
    uint8_t buf[8];
    buf[0] = (g_sig_counter >> 24) & 0xFF; buf[1] = (g_sig_counter >> 16) & 0xFF;
    buf[2] = (g_sig_counter >> 8) & 0xFF;  buf[3] = g_sig_counter & 0xFF;
    buf[4] = (g_reg_count >> 24) & 0xFF;   buf[5] = (g_reg_count >> 16) & 0xFF;
    buf[6] = (g_reg_count >> 8) & 0xFF;    buf[7] = g_reg_count & 0xFF;
    sd_write_file("/u2f/state.bin", buf, 8);
}

/* ---- tiny DER writer for the attestation certificate ---- */
typedef struct { uint8_t *buf; size_t cap, len; bool err; } derw_t;
static void derw_byte(derw_t *w, uint8_t b) {
    if (w->len >= w->cap) { w->err = true; return; }
    w->buf[w->len++] = b;
}
static void derw_bytes(derw_t *w, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) derw_byte(w, p[i]);
}
static void derw_len(derw_t *w, size_t n) {
    if (n < 128) derw_byte(w, (uint8_t)n);
    else if (n < 256) { derw_byte(w, 0x81); derw_byte(w, (uint8_t)n); }
    else { derw_byte(w, 0x82); derw_byte(w, (uint8_t)(n >> 8)); derw_byte(w, (uint8_t)n); }
}
/* Build the self-signed attestation cert for g_attest_priv.
 * TBS content is appended field by field into a scratch buffer, then
 * wrapped, hashed, signed, and wrapped again into the certificate. */
static bool build_attest_cert(void) {
    uint8_t pub[65];
    if (!ec_pub_from_priv(g_attest_priv, pub)) return false;
    static const uint8_t oid_ecdsa_sha256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
    static const uint8_t oid_ec_pubkey[]    = {0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
    static const uint8_t oid_p256[]         = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    static const uint8_t oid_cn[]           = {0x06, 0x03, 0x55, 0x04, 0x03};
    static const char cn[] = "IMP-1 U2F";
    static const char t0[] = "250101000000Z";
    static const char t1[] = "450101000000Z";

    uint8_t scratch[640];
    derw_t w = {scratch, sizeof(scratch), 0, false};
    /* TBS content, in order */
    derw_bytes(&w, (const uint8_t *)"\xA0\x03\x02\x01\x02", 5);      /* version v3 */
    derw_bytes(&w, (const uint8_t *)"\x02\x01\x01", 3);              /* serial 1 */
    derw_byte(&w, 0x30); derw_len(&w, sizeof(oid_ecdsa_sha256));     /* algId */
    derw_bytes(&w, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256));
    /* issuer = subject = CN name */
    for (int rep = 0; rep < 2; rep++) {
        size_t rdn_len = sizeof(oid_cn) + 2 + strlen(cn);
        derw_byte(&w, 0x30); derw_len(&w, rdn_len + 2);
        derw_byte(&w, 0x31); derw_len(&w, rdn_len);
        derw_bytes(&w, oid_cn, sizeof(oid_cn));
        derw_byte(&w, 0x0C); derw_len(&w, strlen(cn));
        derw_bytes(&w, (const uint8_t *)cn, strlen(cn));
        if (rep == 0) {
            /* validity between issuer and subject */
            derw_byte(&w, 0x30); derw_len(&w, 2 * (2 + 13));
            derw_byte(&w, 0x17); derw_len(&w, 13); derw_bytes(&w, (const uint8_t *)t0, 13);
            derw_byte(&w, 0x17); derw_len(&w, 13); derw_bytes(&w, (const uint8_t *)t1, 13);
        }
    }
    /* SPKI */
    {
        size_t alg_len = sizeof(oid_ec_pubkey) + sizeof(oid_p256);
        size_t spki_content = (2 + alg_len) + (2 + 1 + 65);
        derw_byte(&w, 0x30); derw_len(&w, spki_content);
        derw_byte(&w, 0x30); derw_len(&w, alg_len);
        derw_bytes(&w, oid_ec_pubkey, sizeof(oid_ec_pubkey));
        derw_bytes(&w, oid_p256, sizeof(oid_p256));
        derw_byte(&w, 0x03); derw_len(&w, 1 + 65);
        derw_byte(&w, 0x00);
        derw_bytes(&w, pub, 65);
    }
    if (w.err) return false;
    size_t tbs_content = w.len;
    /* Sign the TBS (wrapped in its SEQ header). */
    uint8_t tbs[400];
    derw_t tw = {tbs, sizeof(tbs), 0, false};
    derw_byte(&tw, 0x30); derw_len(&tw, tbs_content);
    derw_bytes(&tw, scratch, tbs_content);
    if (tw.err) return false;
    uint8_t hash[32];
    sha256(tbs, tw.len, hash);
    uint8_t sig[80];
    size_t sig_len = ecdsa_sign_hash(g_attest_priv, hash, sig);
    if (!sig_len) return false;
    /* Certificate = SEQ(TBS || algId || BITSTRING(sig)) */
    derw_t cw = {g_attest_cert, sizeof(g_attest_cert), 0, false};
    size_t cert_content = tw.len + (2 + sizeof(oid_ecdsa_sha256)) + (2 + 1 + sig_len);
    derw_byte(&cw, 0x30); derw_len(&cw, cert_content);
    derw_bytes(&cw, tbs, tw.len);
    derw_byte(&cw, 0x30); derw_len(&cw, sizeof(oid_ecdsa_sha256));
    derw_bytes(&cw, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256));
    derw_byte(&cw, 0x03); derw_len(&cw, 1 + sig_len);
    derw_byte(&cw, 0x00);
    derw_bytes(&cw, sig, sig_len);
    if (cw.err) return false;
    g_attest_cert_len = cw.len;
    return true;
}

static bool u2f_store_init(void) {
    if (!rng_init()) return false;
    g_persistent = imp_storage_sd_ready();
    if (g_persistent && sd_read_file("/u2f/secret.bin", g_secret, 32)) {
        /* have a secret */
    } else {
        if (mbedtls_ctr_drbg_random(&g_drbg, g_secret, 32) != 0) return false;
        if (g_persistent) sd_write_file("/u2f/secret.bin", g_secret, 32);
    }
    if (g_persistent && sd_read_file("/u2f/attest.key", g_attest_priv, 32)) {
        /* have an attestation key */
    } else {
        uint8_t pub[65];
        if (!ec_genkey(g_attest_priv, pub)) return false;
        if (g_persistent) sd_write_file("/u2f/attest.key", g_attest_priv, 32);
    }
    if (g_persistent) {
        uint8_t buf[8];
        if (sd_read_file("/u2f/state.bin", buf, 8)) {
            g_sig_counter = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) | buf[3];
            g_reg_count = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                          ((uint32_t)buf[6] << 8) | buf[7];
        }
    }
    return build_attest_cert();
}

/* ---- key handle wrap / unwrap (v1: XOR mask + HMAC tag) ---- */
#define KH_LEN 64
static void kh_wrap(const uint8_t priv[32], const uint8_t appId[32], uint8_t handle[KH_LEN]) {
    uint8_t in[10 + 32 + KH_LEN];
    uint8_t mask[32], tag[32];
    memcpy(in, "wrap-mask", 9);
    memcpy(in + 9, appId, 32);
    hmac_sha256(g_secret, 32, in, 9 + 32, mask);
    for (int i = 0; i < 32; i++) handle[i] = priv[i] ^ mask[i];
    memcpy(in, "wrap-tag!", 9);
    memcpy(in + 9, handle, 32);
    memcpy(in + 9 + 32, appId, 32);
    hmac_sha256(g_secret, 32, in, 9 + 32 + 32, tag);
    memcpy(handle + 32, tag, 32);
}

static bool kh_unwrap(const uint8_t handle[KH_LEN], const uint8_t appId[32], uint8_t priv[32]) {
    uint8_t in[10 + 32 + KH_LEN];
    uint8_t mask[32], tag[32];
    memcpy(in, "wrap-tag!", 9);
    memcpy(in + 9, handle, 32);
    memcpy(in + 9 + 32, appId, 32);
    hmac_sha256(g_secret, 32, in, 9 + 32 + 32, tag);
    if (memcmp(tag, handle + 32, 32) != 0) return false;
    memcpy(in, "wrap-mask", 9);
    memcpy(in + 9, appId, 32);
    hmac_sha256(g_secret, 32, in, 9 + 32, mask);
    for (int i = 0; i < 32; i++) priv[i] = handle[i] ^ mask[i];
    return true;
}

/* ================================================================== */
/* CTAPHID transport                                                   */
/* ================================================================== */
#define CTAPHID_PING      0x01
#define CTAPHID_MSG       0x03
#define CTAPHID_LOCK      0x04
#define CTAPHID_INIT      0x06
#define CTAPHID_WINK      0x08
#define CTAPHID_CBOR      0x10
#define CTAPHID_CANCEL    0x11
#define CTAPHID_KEEPALIVE 0x3B
#define CTAPHID_ERROR     0x3F
#define CTAPHID_BROADCAST 0xFFFFFFFFu
#define ERR_INVALID_CMD   0x01
#define ERR_INVALID_PAR   0x02
#define ERR_INVALID_LEN   0x03
#define ERR_INVALID_SEQ   0x04
#define ERR_INVALID_CHAN  0x0B

static const uint8_t fido_descriptor[] = {
    0x06, 0xD0, 0xF1,       /* Usage Page (FIDO Alliance) */
    0x09, 0x01,             /* Usage (1) */
    0xA1, 0x01,             /* Collection (Application) */
    0x09, 0x20,             /*   Usage (32): input data */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x40,             /*   Report Count (64) */
    0x81, 0x02,             /*   Input (Data, Var, Abs) */
    0x09, 0x21,             /*   Usage (33): output data */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x40,             /*   Report Count (64) */
    0x91, 0x02,             /*   Output (Data, Var, Abs) */
    0xC0                    /* End Collection */
};

/* RX ring: written from _onOutput (USB task), drained by the tool loop. */
#define RXQ_SLOTS 16
static uint8_t rxq[RXQ_SLOTS][64];
static volatile uint8_t rxq_head = 0, rxq_tail = 0;

class FidoDevice : public USBHIDDevice {
public:
    USBHID hid;
    FidoDevice() {
        static bool registered = false;
        if (!registered) {
            registered = true;
            hid.addDevice(this, sizeof(fido_descriptor));
        }
    }
    void begin() { hid.begin(); }
    bool send64(const uint8_t pkt[64]) { return hid.SendReport(0, pkt, 64, 100); }
    uint16_t _onGetDescriptor(uint8_t *buffer) override {
        memcpy(buffer, fido_descriptor, sizeof(fido_descriptor));
        return sizeof(fido_descriptor);
    }
    void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) override {
        (void)report_id;
        if (len < 64) return;
        uint8_t next = (uint8_t)((rxq_head + 1) % RXQ_SLOTS);
        if (next == rxq_tail) return; /* full: drop, host retries */
        memcpy(rxq[rxq_head], buffer, 64);
        rxq_head = next;
    }
};
static FidoDevice fido_dev;

/* ---- engine state ---- */
static uint32_t next_cid = 1;
/* assembly of one inbound message */
static uint32_t asm_cid = 0;
static uint8_t asm_cmd = 0;
static uint16_t asm_total = 0, asm_have = 0;
static int asm_seq = -1;
static uint8_t asm_buf[2048];
static bool asm_active = false;
/* parked user-presence request */
typedef struct {
    bool active;
    uint32_t cid;
    uint8_t ins;
    uint8_t p1;
    uint8_t challenge[32];
    uint8_t appId[32];
    uint8_t key_handle[KH_LEN];
    uint32_t started_iter; /* tool-loop iterations, for timeout/keepalive */
} pending_t;
static pending_t pending;
/* tool-loop communication */
static bool flag_presence_ok = false;
static bool flag_presence_cancel = false;
static bool flag_wink = false;
static uint32_t loop_iter = 0;

static void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len) {
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (cid >> 24) & 0xFF; pkt[1] = (cid >> 16) & 0xFF;
    pkt[2] = (cid >> 8) & 0xFF;  pkt[3] = cid & 0xFF;
    pkt[4] = 0x80 | cmd;
    pkt[5] = (len >> 8) & 0xFF;  pkt[6] = len & 0xFF;
    uint16_t first = len < 57 ? len : 57;
    if (first) memcpy(pkt + 7, data, first);
    fido_dev.send64(pkt);
    uint16_t off = first;
    uint8_t seq = 0;
    while (off < len) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = (cid >> 24) & 0xFF; pkt[1] = (cid >> 16) & 0xFF;
        pkt[2] = (cid >> 8) & 0xFF;  pkt[3] = cid & 0xFF;
        pkt[4] = seq++;
        uint16_t chunk = (len - off) < 59 ? (len - off) : 59;
        memcpy(pkt + 5, data + off, chunk);
        fido_dev.send64(pkt);
        off += chunk;
    }
}

static void ctaphid_error(uint32_t cid, uint8_t code) {
    ctaphid_send(cid, CTAPHID_ERROR, &code, 1);
}

/* ================================================================== */
/* U2F APDUs                                                           */
/* ================================================================== */
static uint8_t apdu_resp[1024];
static size_t apdu_finish(uint32_t cid, size_t data_len, uint16_t sw) {
    apdu_resp[data_len] = (sw >> 8) & 0xFF;
    apdu_resp[data_len + 1] = sw & 0xFF;
    ctaphid_send(cid, CTAPHID_MSG, apdu_resp, (uint16_t)(data_len + 2));
    return data_len + 2;
}

static void do_register(uint32_t cid, const uint8_t *challenge, const uint8_t *appId) {
    uint8_t priv[32], pub[65], handle[KH_LEN];
    if (!ec_genkey(priv, pub)) { apdu_finish(cid, 0, 0x6F00); return; }
    kh_wrap(priv, appId, handle);
    /* attestation signature over 0x00|appId|challenge|handle|pub */
    uint8_t blob[1 + 32 + 32 + KH_LEN + 65];
    blob[0] = 0x00;
    memcpy(blob + 1, appId, 32);
    memcpy(blob + 33, challenge, 32);
    memcpy(blob + 65, handle, KH_LEN);
    memcpy(blob + 65 + KH_LEN, pub, 65);
    uint8_t hash[32];
    sha256(blob, sizeof(blob), hash);
    uint8_t sig[80];
    size_t sig_len = ecdsa_sign_hash(g_attest_priv, hash, sig);
    if (!sig_len) { apdu_finish(cid, 0, 0x6F00); return; }
    size_t n = 0;
    apdu_resp[n++] = 0x05;
    memcpy(apdu_resp + n, pub, 65); n += 65;
    apdu_resp[n++] = KH_LEN;
    memcpy(apdu_resp + n, handle, KH_LEN); n += KH_LEN;
    memcpy(apdu_resp + n, g_attest_cert, g_attest_cert_len); n += g_attest_cert_len;
    memcpy(apdu_resp + n, sig, sig_len); n += sig_len;
    g_reg_count++;
    state_save();
    apdu_finish(cid, n, 0x9000);
}

static void do_sign(uint32_t cid, const pending_t *req) {
    uint8_t priv[32];
    if (!kh_unwrap(req->key_handle, req->appId, priv)) {
        apdu_finish(cid, 0, 0x6A80);
        return;
    }
    g_sig_counter++;
    state_save();
    uint8_t presence = (req->p1 == 0x08) ? 0x00 : 0x01;
    uint8_t blob[32 + 1 + 4 + 32];
    memcpy(blob, req->appId, 32);
    blob[32] = presence;
    blob[33] = (g_sig_counter >> 24) & 0xFF; blob[34] = (g_sig_counter >> 16) & 0xFF;
    blob[35] = (g_sig_counter >> 8) & 0xFF;  blob[36] = g_sig_counter & 0xFF;
    memcpy(blob + 37, req->challenge, 32);
    uint8_t hash[32];
    sha256(blob, sizeof(blob), hash);
    uint8_t sig[80];
    size_t sig_len = ecdsa_sign_hash(priv, hash, sig);
    if (!sig_len) { apdu_finish(cid, 0, 0x6F00); return; }
    size_t n = 0;
    apdu_resp[n++] = presence;
    apdu_resp[n++] = blob[33]; apdu_resp[n++] = blob[34];
    apdu_resp[n++] = blob[35]; apdu_resp[n++] = blob[36];
    memcpy(apdu_resp + n, sig, sig_len); n += sig_len;
    apdu_finish(cid, n, 0x9000);
}

/* Handle one complete CTAP1 APDU. May park a presence request. */
static void u2f_handle_apdu(uint32_t cid, const uint8_t *data, uint16_t len) {
    if (len < 4) { apdu_finish(cid, 0, 0x6700); return; }
    uint8_t ins = data[1], p1 = data[2];
    const uint8_t *body = nullptr;
    uint16_t body_len = 0;
    if (len > 4) {
        if (data[4] == 0 && len >= 7) {
            body_len = ((uint16_t)data[5] << 8) | data[6];
            body = data + 7;
            if (body_len > len - 7) body_len = len - 7;
        } else {
            body_len = data[4];
            body = data + 5;
            if (body_len > len - 5) body_len = len - 5;
        }
    }
    if (ins == 0x01) { /* REGISTER */
        if (!body || body_len < 64) { apdu_finish(cid, 0, 0x6700); return; }
        memset(&pending, 0, sizeof(pending));
        pending.active = true;
        pending.cid = cid;
        pending.ins = ins;
        pending.p1 = p1;
        memcpy(pending.challenge, body, 32);
        memcpy(pending.appId, body + 32, 32);
        pending.started_iter = loop_iter;
        return;
    }
    if (ins == 0x03) {
        if (body_len == 0) { /* VERSION shares INS 0x03, no data */
            memcpy(apdu_resp, "U2F_V2", 6);
            apdu_finish(cid, 6, 0x9000);
            return;
        }
        /* AUTHENTICATE */
        if (!body || body_len < 65) { apdu_finish(cid, 0, 0x6700); return; }
        uint8_t kh_len = body[64];
        if (kh_len != KH_LEN || body_len < 65 + KH_LEN) {
            apdu_finish(cid, 0, 0x6A80);
            return;
        }
        uint8_t priv[32];
        if (!kh_unwrap(body + 65, body + 32, priv)) {
            apdu_finish(cid, 0, 0x6A80);
            return;
        }
        if (p1 == 0x07) { /* check-only: valid handle -> 0x6985 */
            apdu_finish(cid, 0, 0x6985);
            return;
        }
        if (p1 == 0x08) { /* sign without user presence */
            memset(&pending, 0, sizeof(pending));
            pending.cid = cid; pending.ins = ins; pending.p1 = p1;
            memcpy(pending.challenge, body, 32);
            memcpy(pending.appId, body + 32, 32);
            memcpy(pending.key_handle, body + 65, KH_LEN);
            do_sign(cid, &pending);
            return;
        }
        if (p1 != 0x03) { apdu_finish(cid, 0, 0x6A80); return; }
        memset(&pending, 0, sizeof(pending));
        pending.active = true;
        pending.cid = cid;
        pending.ins = ins;
        pending.p1 = p1;
        memcpy(pending.challenge, body, 32);
        memcpy(pending.appId, body + 32, 32);
        memcpy(pending.key_handle, body + 65, KH_LEN);
        pending.started_iter = loop_iter;
        return;
    }
    apdu_finish(cid, 0, 0x6D00); /* INS not supported */
}

static void ctaphid_handle_message(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len) {
    switch (cmd) {
        case CTAPHID_PING:
            ctaphid_send(cid, CTAPHID_PING, data, len);
            break;
        case CTAPHID_INIT: {
            if (len < 8) { ctaphid_error(cid, ERR_INVALID_LEN); break; }
            uint32_t resp_cid = cid;
            uint32_t new_cid = cid;
            if (cid == CTAPHID_BROADCAST || cid == 0) {
                new_cid = next_cid++;
                if (new_cid == 0 || new_cid == CTAPHID_BROADCAST) new_cid = next_cid++;
            }
            uint8_t resp[17];
            memcpy(resp, data, 8); /* nonce echo */
            resp[8] = (new_cid >> 24) & 0xFF; resp[9] = (new_cid >> 16) & 0xFF;
            resp[10] = (new_cid >> 8) & 0xFF; resp[11] = new_cid & 0xFF;
            resp[12] = 2;    /* CTAPHID protocol version */
            resp[13] = 1;    /* device version major */
            resp[14] = 0;    /* minor */
            resp[15] = 0;    /* build */
            resp[16] = 0x01; /* capabilities: WINK */
            ctaphid_send(resp_cid, CTAPHID_INIT, resp, sizeof(resp));
            break;
        }
        case CTAPHID_WINK:
            flag_wink = true;
            imp_system_set_led(0x202000);
            ctaphid_send(cid, CTAPHID_WINK, nullptr, 0);
            break;
        case CTAPHID_MSG:
            if (pending.active) { ctaphid_error(cid, 0x06 /* busy-ish: use msg timeout slot */); break; }
            u2f_handle_apdu(cid, data, len);
            break;
        case CTAPHID_CANCEL:
            if (pending.active && pending.cid == cid) pending.active = false;
            break;
        default:
            ctaphid_error(cid, ERR_INVALID_CMD);
            break;
    }
}

/* Drain the RX ring, run assembly, service the parked request. */
static void u2f_engine_poll(void) {
    while (rxq_tail != rxq_head) {
        uint8_t pkt[64];
        memcpy(pkt, rxq[rxq_tail], 64);
        rxq_tail = (uint8_t)((rxq_tail + 1) % RXQ_SLOTS);
        uint32_t cid = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                       ((uint32_t)pkt[2] << 8) | pkt[3];
        if (pkt[4] & 0x80) { /* initialisation packet */
            uint8_t cmd = pkt[4] & 0x7F;
            uint16_t bcnt = ((uint16_t)pkt[5] << 8) | pkt[6];
            if (bcnt > sizeof(asm_buf)) { ctaphid_error(cid, ERR_INVALID_LEN); asm_active = false; continue; }
            asm_cid = cid; asm_cmd = cmd; asm_total = bcnt;
            asm_have = bcnt < 57 ? bcnt : 57;
            memcpy(asm_buf, pkt + 7, asm_have);
            asm_seq = 0;
            asm_active = true;
            if (asm_have >= asm_total) {
                asm_active = false;
                ctaphid_handle_message(cid, cmd, asm_buf, asm_total);
            }
        } else { /* continuation packet */
            if (!asm_active || cid != asm_cid) continue;
            if (pkt[4] != (uint8_t)asm_seq) {
                ctaphid_error(cid, ERR_INVALID_SEQ);
                asm_active = false;
                continue;
            }
            asm_seq++;
            uint16_t want = asm_total - asm_have;
            uint16_t chunk = want < 59 ? want : 59;
            memcpy(asm_buf + asm_have, pkt + 5, chunk);
            asm_have += chunk;
            if (asm_have >= asm_total) {
                asm_active = false;
                ctaphid_handle_message(asm_cid, asm_cmd, asm_buf, asm_total);
            }
        }
    }
    if (pending.active) {
        if (flag_presence_cancel) {
            flag_presence_cancel = false;
            pending.active = false;
            apdu_finish(pending.cid, 0, 0x6985);
        } else if (flag_presence_ok) {
            flag_presence_ok = false;
            pending_t req = pending;
            pending.active = false;
            if (req.ins == 0x01) do_register(req.cid, req.challenge, req.appId);
            else do_sign(req.cid, &req);
        } else if (loop_iter - pending.started_iter > 400) { /* ~10 s at 25 ms */
            pending.active = false;
            apdu_finish(pending.cid, 0, 0x6985);
        } else if (((loop_iter - pending.started_iter) % 40) == 39) {
            uint8_t ka = 0x01; /* KEEPALIVE: processing */
            ctaphid_send(pending.cid, CTAPHID_KEEPALIVE, &ka, 1);
        }
    }
}

/* ================================================================== */
/* Tool                                                                */
/* ================================================================== */
typedef struct {
    imp_ui_t *ui;
    bool running;
    bool was_pending;
    uint32_t led_tick;
} u2f_tool_t;

static void u2f_on_input(void *ctx, imp_input_t ev) {
    u2f_tool_t *s = (u2f_tool_t *)ctx;
    if (ev == IMP_INPUT_OK && pending.active) {
        flag_presence_ok = true;
    } else if (ev == IMP_INPUT_BACK) {
        if (pending.active) flag_presence_cancel = true;
        else s->running = false;
    }
}

int u2f_main(void) {
    u2f_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("U2F");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, u2f_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_u2f");
    if (!u2f_store_init()) {
        imp_ui_text(s.ui, 0, "Crypto setup failed.");
        imp_ui_text(s.ui, 1, "BACK exits.");
        imp_ui_status(s.ui, "U2F not available");
        while (s.running) imp_delay_ms(30);
        imp_ui_close(s.ui);
        return -1;
    }
    fido_dev.begin();
    USB.begin();
    imp_ui_text(s.ui, 0, "U2F key active.");
    imp_ui_text(s.ui, 1, "Plug into the host USB.");
    if (!g_persistent) {
        imp_ui_text(s.ui, 2, "No microSD: keys are");
        imp_ui_text(s.ui, 3, "session-only (RAM).");
    }
    imp_ui_text(s.ui, 5, "USB stays on until");
    imp_ui_text(s.ui, 6, "reboot after exit.");
    imp_ui_status(s.ui, "Waiting for host");
    while (s.running) {
        loop_iter++;
        u2f_engine_poll();
        if (flag_wink) {
            flag_wink = false;
            s.led_tick = loop_iter + 12; /* ~300 ms flash */
            imp_system_set_led(0x202000);
        }
        if (pending.active != s.was_pending) {
            s.was_pending = pending.active;
            if (pending.active) {
                imp_ui_set_pose(s.ui, "pose_working");
                imp_ui_status(s.ui, pending.ins == 0x01 ? "Register: press OK = touch"
                                                        : "Sign: press OK = touch");
            } else {
                imp_ui_set_pose(s.ui, "pose_u2f");
                imp_ui_status(s.ui, "Waiting for host");
                imp_system_set_led(0);
            }
        }
        if (pending.active) {
            /* blink the status LED while a touch is wanted */
            if ((loop_iter % 16) == 0) imp_system_set_led(0x202000);
            if ((loop_iter % 16) == 8) imp_system_set_led(0);
        } else if (s.led_tick && loop_iter > s.led_tick) {
            s.led_tick = 0;
            imp_system_set_led(0);
        }
        imp_ui_value(s.ui, "Registered", (int32_t)g_reg_count, "");
        imp_ui_value(s.ui, "Signatures", (int32_t)g_sig_counter, "");
        imp_delay_ms(25);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t u2f_app = {
    "u2f", "U2F", "USB", "u2f_10px", "pose_u2f", "u2f", 8192, u2f_main
};
IMP_APP_REGISTER(u2f_app);
