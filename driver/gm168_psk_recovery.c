// SPDX-License-Identifier: LGPL-2.1-or-later
// gm168_psk_recovery.c — read-only GM168 sealed-PSK recovery (pure offline crypto)
//
// Offline implementation of the model documented in docs/PSK-RECOVERY.md.
// Pure C: no USB, no filesystem access, no fprintd, no psk.bin / DPAPI /
// OEM DLL access. All test vectors are synthetic. Diagnostics never carry
// raw area bytes, blob bytes, PSK material or OpenSSL error strings.

#include "gm168_psk_recovery.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <string.h>

GQuark
gm168_psk_recovery_error_quark (void)
{
    static gsize quark = 0;

    if (g_once_init_enter (&quark)) {
        gsize domain = (gsize) g_quark_from_static_string (
            "gm168-psk-recovery-error");
        g_once_init_leave (&quark, domain);
    }
    return (GQuark) quark;
}

const guint8 gm168_public_zero_psk_whitebox[GM168_PSK_WHITEBOX_LEN] = {
    0xec, 0x35, 0xae, 0x3a, 0xbb, 0x45, 0xed, 0x3f,
    0x12, 0xc4, 0x75, 0x1f, 0x1e, 0x5c, 0x2c, 0xc0, 0x5b, 0x3c, 0x54, 0x52,
    0xe9, 0x10, 0x4d, 0x9f, 0x2a, 0x31, 0x18, 0x64, 0x4f, 0x37, 0xa0, 0x4b,
    0x6f, 0xd6, 0x6b, 0x1d, 0x97, 0xcf, 0x80, 0xf1, 0x34, 0x5f, 0x76, 0xc8,
    0x4f, 0x03, 0xff, 0x30, 0xbb, 0x51, 0xbf, 0x30, 0x8f, 0x2a, 0x98, 0x75,
    0xc4, 0x1e, 0x65, 0x92, 0xcd, 0x2a, 0x2f, 0x9e, 0x60, 0x80, 0x9b, 0x17,
    0xb5, 0x31, 0x60, 0x37, 0xb6, 0x9b, 0xb2, 0xfa, 0x5d, 0x4c, 0x8a, 0xc3,
    0x1e, 0xdb, 0x33, 0x94, 0x04, 0x6e, 0xc0, 0x6b, 0xbd, 0xac, 0xc5, 0x7d,
    0xa6, 0xa7, 0x56, 0xc5,
};

// ─── Small helpers ──────────────────────────────────────────────────────────

static gboolean set_error (GError **error, Gm168PskRecoveryError code,
                           const char *format, ...) G_GNUC_PRINTF (3, 4);

static gboolean
set_error (GError **error, Gm168PskRecoveryError code, const char *format, ...)
{
    va_list args;
    gchar *message = NULL;

    if (error == NULL)
        return FALSE;

    va_start (args, format);
    message = g_strdup_vprintf (format, args);
    va_end (args);

    g_set_error_literal (error, GM168_PSK_RECOVERY_ERROR, code, message);
    g_free (message);
    return FALSE;
}

/* Drain the OpenSSL error queue so a failed operation never leaves stale
 * entries behind for an unrelated caller. */
static void
crypto_drain_errors (void)
{
    ERR_clear_error ();
}

/* g_propagate_error() rejects a NULL source in recent GLib. */
static void
propagate_local (GError **error, GError *local)
{
    if (local != NULL)
        g_propagate_error (error, local);
}

static gboolean
crypto_failed (GError **error)
{
    crypto_drain_errors ();
    return set_error (error, GM168_PSK_RECOVERY_ERROR_CRYPTO,
                      "internal cryptographic operation failed");
}

static gboolean
sha256_digest (const guint8 *data, gsize len, guint8 out[32], GError **error)
{
    unsigned int out_len = 0;

    crypto_drain_errors ();
    if (EVP_Digest (data, len, out, &out_len, EVP_sha256 (), NULL) != 1 ||
        out_len != 32)
        return crypto_failed (error);
    return TRUE;
}

static gboolean
hmac_sha256 (const guint8 *key, gsize key_len,
             const guint8 *data, gsize data_len,
             guint8        out[32],
             GError      **error)
{
    unsigned int out_len = 0;

    if (key_len > G_MAXINT)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                          "whitebox key exceeds supported size");

    crypto_drain_errors ();
    if (HMAC (EVP_sha256 (), key, (int) key_len, data, data_len, out,
              &out_len) == NULL || out_len != 32)
        return crypto_failed (error);
    return TRUE;
}

// ─── DPAPI v1 dynamic total-length parser ───────────────────────────────────

typedef struct {
    const guint8 *data;
    gsize         len;
    guint64       offset;
} Gm168DpapiReader;

/* NEED_MORE when the buffer ends before the u32; the caller maps that. */
static Gm168DpapiParseResult
dpapi_take_u32 (Gm168DpapiReader *reader, guint32 *value, GError **error)
{
    const guint8 *p;

    if (reader->offset > GM168_PSK_MAX_DPAPI ||
        GM168_PSK_MAX_DPAPI - reader->offset < 4) {
        set_error (error, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "DPAPI fixed field exceeds maximum %d",
                   GM168_PSK_MAX_DPAPI);
        return GM168_DPAPI_INVALID;
    }
    if (reader->offset > reader->len ||
        reader->len - (gsize) reader->offset < 4)
        return GM168_DPAPI_NEED_MORE;

    p = reader->data + reader->offset;
    *value = (guint32) p[0]        |
             (guint32) p[1] <<  8 |
             (guint32) p[2] << 16 |
             (guint32) p[3] << 24;
    reader->offset += 4;
    return GM168_DPAPI_OK;
}

/* Variable byte arrays: bounded individually and cumulatively against
 * GM168_PSK_MAX_DPAPI *before* the buffer-end check, so impossible sizes are
 * rejected immediately even on an incomplete prefix. */
static Gm168DpapiParseResult
dpapi_take_bytes (Gm168DpapiReader *reader,
                  guint32           length,
                  const char       *field,
                  GError          **error)
{
    if (length > GM168_PSK_MAX_DPAPI ||
        reader->offset + length > GM168_PSK_MAX_DPAPI) {
        set_error (error, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "DPAPI field '%s' length %u exceeds maximum %d",
                   field, length, GM168_PSK_MAX_DPAPI);
        return GM168_DPAPI_INVALID;
    }
    if (reader->offset + length > reader->len)
        return GM168_DPAPI_NEED_MORE;
    reader->offset += length;
    return GM168_DPAPI_OK;
}

Gm168DpapiParseResult
gm168_dpapi_parse_total_length (const guint8 *data,
                                gsize         len,
                                guint32      *total_len,
                                GError      **error)
{
    Gm168DpapiReader reader = { data, len, 0 };
    guint32 value = 0, desc_len = 0, salt_len = 0, prompt_len = 0;
    guint32 sign_salt_len = 0, data_len = 0, sign_len = 0;
    Gm168DpapiParseResult res;

    g_return_val_if_fail (data != NULL || len == 0, GM168_DPAPI_INVALID);
    g_return_val_if_fail (total_len != NULL, GM168_DPAPI_INVALID);

    // dwVersion — must be 1.
    res = dpapi_take_u32 (&reader, &value, error);
    if (res != GM168_DPAPI_OK)
        return res;
    if (value != 1) {
        set_error (error, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "DPAPI blob is not v1");
        return GM168_DPAPI_INVALID;
    }

    // guidProvider, dwMasterKeyVersion, guidMasterKey, dwFlags.
    res = dpapi_take_bytes (&reader, 16, "guidProvider", error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_u32 (&reader, &value, error);   // dwMasterKeyVersion
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, 16, "guidMasterKey", error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_u32 (&reader, &value, error);   // dwFlags
    if (res != GM168_DPAPI_OK)
        return res;

    // dwDescriptionLen + description.
    res = dpapi_take_u32 (&reader, &desc_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, desc_len, "description", error);
    if (res != GM168_DPAPI_OK)
        return res;

    // dwCryptAlgo, dwCryptAlgoLen (bit length: parsed, no payload follows).
    res = dpapi_take_u32 (&reader, &value, error);   // dwCryptAlgo
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_u32 (&reader, &value, error);   // dwCryptAlgoLen
    if (res != GM168_DPAPI_OK)
        return res;

    // dwSaltLen + salt.
    res = dpapi_take_u32 (&reader, &salt_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, salt_len, "salt", error);
    if (res != GM168_DPAPI_OK)
        return res;

    // dwPromptLen + prompt.
    res = dpapi_take_u32 (&reader, &prompt_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, prompt_len, "prompt", error);
    if (res != GM168_DPAPI_OK)
        return res;

    // dwSignAlgo, dwSignAlgoLen (bit length: parsed, no payload follows).
    res = dpapi_take_u32 (&reader, &value, error);   // dwSignAlgo
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_u32 (&reader, &value, error);   // dwSignAlgoLen
    if (res != GM168_DPAPI_OK)
        return res;

    // dwSignSaltLen + signSalt.
    res = dpapi_take_u32 (&reader, &sign_salt_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, sign_salt_len, "signSalt", error);
    if (res != GM168_DPAPI_OK)
        return res;

    // dwDataLen + data.
    res = dpapi_take_u32 (&reader, &data_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, data_len, "data", error);
    if (res != GM168_DPAPI_OK)
        return res;

    // dwSignLen + signature (explicit; never derived from dwSignAlgoLen).
    res = dpapi_take_u32 (&reader, &sign_len, error);
    if (res != GM168_DPAPI_OK)
        return res;
    res = dpapi_take_bytes (&reader, sign_len, "signature", error);
    if (res != GM168_DPAPI_OK)
        return res;

    if (reader.offset > GM168_PSK_MAX_DPAPI) {
        set_error (error, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "DPAPI total length %llu exceeds maximum %d",
                   (unsigned long long) reader.offset, GM168_PSK_MAX_DPAPI);
        return GM168_DPAPI_INVALID;
    }

    *total_len = (guint32) reader.offset;
    return GM168_DPAPI_OK;
}

// ─── Exact whitebox TLV record parser ───────────────────────────────────────

gboolean
gm168_psk_parse_whitebox_record (const guint8 *record,
                                 gsize         record_len,
                                 guint8        blob_out[GM168_PSK_WHITEBOX_LEN],
                                 GError      **error)
{
    guint32 tag, length;

    g_return_val_if_fail (record != NULL || record_len == 0, FALSE);
    g_return_val_if_fail (blob_out != NULL, FALSE);

    if (record_len != GM168_PSK_RECORD_LEN)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_RECORD,
                          "whitebox record must be exactly %d bytes",
                          GM168_PSK_RECORD_LEN);

    tag = (guint32) record[0]        |
          (guint32) record[1] <<  8 |
          (guint32) record[2] << 16 |
          (guint32) record[3] << 24;
    length = (guint32) record[4]        |
             (guint32) record[5] <<  8 |
             (guint32) record[6] << 16 |
             (guint32) record[7] << 24;

    if (tag != GM168_PSK_RECORD_TLV_TAG)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_RECORD,
                          "unexpected record TLV tag 0x%08x", tag);
    if (length != GM168_PSK_WHITEBOX_LEN)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_RECORD,
                          "whitebox record length must be %d, got %u",
                          GM168_PSK_WHITEBOX_LEN, length);

    memcpy (blob_out, record + 8, GM168_PSK_WHITEBOX_LEN);
    return TRUE;
}

// ─── Goodix SecWhiteEncrypt primitives ──────────────────────────────────────

static guint8
ror8 (guint8 value, guint count)
{
    count &= 7;
    return (guint8) ((value >> count) | (value << (8 - count)));
}

/* CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no final xor). */
static guint32
crc32_mpeg_update (const guint8 *data, gsize len)
{
    guint32 crc = 0xFFFFFFFF;

    for (gsize i = 0; i < len; i++) {
        crc ^= (guint32) data[i] << 24;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

/* One AES-CBC block with an all-zero key/IV, padding disabled; keeps the
 * first two output bytes exactly like the synthetic reference KAT model. */
static gboolean
aes_cbc_pair2 (const guint8 *block16,
               guint         key_len,
               gboolean      decrypt,
               guint8        out2[2],
               GError      **error)
{
    static const guint8 zero_key[32] = { 0 };
    static const guint8 zero_iv[16] = { 0 };
    const EVP_CIPHER *cipher = NULL;
    EVP_CIPHER_CTX *ctx = NULL;
    guint8 tmp[32] = { 0 };
    int len = 0, final_len = 0;
    gboolean ok = FALSE;

    if (key_len == 16)
        cipher = EVP_aes_128_cbc ();
    else if (key_len == 24)
        cipher = EVP_aes_192_cbc ();
    else
        cipher = EVP_aes_256_cbc ();

    crypto_drain_errors ();
    ctx = EVP_CIPHER_CTX_new ();
    if (ctx == NULL)
        goto out;
    if (EVP_CipherInit_ex (ctx, cipher, NULL, zero_key, zero_iv,
                           decrypt ? 0 : 1) != 1)
        goto out;
    if (EVP_CIPHER_CTX_set_padding (ctx, 0) != 1)
        goto out;
    if (EVP_CipherUpdate (ctx, tmp, &len, block16, 16) != 1)
        goto out;
    if (EVP_CipherFinal_ex (ctx, tmp + len, &final_len) != 1)
        goto out;
    len += final_len;
    if (len < 2)
        goto out;

    memcpy (out2, tmp, 2);
    ok = TRUE;

out:
    if (ctx != NULL)
        EVP_CIPHER_CTX_free (ctx);
    OPENSSL_cleanse (tmp, sizeof (tmp));
    if (!ok)
        crypto_failed (error);
    return ok;
}

gboolean
gm168_wb_derive_root_secret (guint8 root[16], GError **error)
{
    static const guint8 root_source[] = "Goodix";   /* 6 bytes, no NUL */
    static const guint8 rotations[5] = { 1, 3, 5, 7, 9 };
    static const guint8 key_sizes[4] = { 16, 16, 32, 24 };
    static const guint8 hmac_key[16] = { '1', '2', '3', '4', '5', '6' };
    guint8 transformed[5 * 6];
    guint8 digest[32];
    guint8 pair[2];
    guint8 block[16];
    guint32 crc;
    GError *local = NULL;

    g_return_val_if_fail (root != NULL, FALSE);

    for (guint r = 0; r < 5; r++)
        for (guint i = 0; i < 6; i++)
            transformed[r * 6 + i] = ror8 (root_source[i], rotations[r]);

    memset (root, 0, 16);

    if (!sha256_digest (transformed, 3, digest, &local))
        goto fail;
    memcpy (root, digest, 2);

    for (guint index = 0; index < 4; index++) {
        guint triple_offset = 3 + 3 * index;

        memcpy (block, transformed + triple_offset, 3);
        memset (block + 3, 0xcc, 13);
        if (!aes_cbc_pair2 (block, key_sizes[index], (index % 2) == 0,
                            pair, &local))
            goto fail;
        memcpy (root + 2 + 2 * index, pair, 2);
    }

    /* HMAC-SHA256("123456" || 10 zero pad, transformed[15:18])[:2] */
    if (!hmac_sha256 (hmac_key, sizeof (hmac_key), transformed + 15, 3,
                      digest, &local))
        goto fail;
    memcpy (root + 10, digest, 2);

    /* CRC-32/MPEG-2(transformed[18:21]), big-endian high half. */
    crc = crc32_mpeg_update (transformed + 18, 3);
    root[12] = (guint8) (crc >> 24);
    root[13] = (guint8) (crc >> 16);

    if (!sha256_digest (transformed + 21, 3, digest, &local))
        goto fail;
    memcpy (root + 14, digest, 2);

    OPENSSL_cleanse (transformed, sizeof (transformed));
    OPENSSL_cleanse (digest, sizeof (digest));
    OPENSSL_cleanse (pair, sizeof (pair));
    OPENSSL_cleanse (block, sizeof (block));
    return TRUE;

fail:
    OPENSSL_cleanse (transformed, sizeof (transformed));
    OPENSSL_cleanse (digest, sizeof (digest));
    OPENSSL_cleanse (pair, sizeof (pair));
    OPENSSL_cleanse (block, sizeof (block));
    OPENSSL_cleanse (root, 16);
    propagate_local (error, local);
    return FALSE;
}

gboolean
gm168_wb_derive_iv (guint32  plaintext_len,
                    guint8   iv[16],
                    GError **error)
{
    static const guint8 kdf_label[] = "123GOODIX";  /* 9 bytes, no NUL */
    guint8 input[4 + sizeof (kdf_label) - 1];
    guint8 digest[32];

    g_return_val_if_fail (iv != NULL, FALSE);

    input[0] = (guint8) plaintext_len;
    input[1] = (guint8) (plaintext_len >> 8);
    input[2] = (guint8) (plaintext_len >> 16);
    input[3] = (guint8) (plaintext_len >> 24);
    memcpy (input + 4, kdf_label, sizeof (kdf_label) - 1);

    if (!sha256_digest (input, sizeof (input), digest, error)) {
        OPENSSL_cleanse (input, sizeof (input));
        return FALSE;
    }

    memcpy (iv, digest, 16);
    iv[15] = (iv[15] & 0xF0) | (plaintext_len & 0x0F);

    OPENSSL_cleanse (input, sizeof (input));
    OPENSSL_cleanse (digest, sizeof (digest));
    return TRUE;
}

/* key_material = SHA256(iv || 48 zero bytes || root_secret) */
static gboolean
derive_key_material (const guint8 iv[16],
                     const guint8 root[16],
                     guint8       key_material[32],
                     GError     **error)
{
    guint8 input[16 + 48 + 16];
    gboolean ok;

    memcpy (input, iv, 16);
    memset (input + 16, 0, 48);
    memcpy (input + 64, root, 16);

    ok = sha256_digest (input, sizeof (input), key_material, error);
    OPENSSL_cleanse (input, sizeof (input));
    return ok;
}

gboolean
gm168_wb_encrypt (const guint8  *plaintext,
                  gsize          plaintext_len,
                  guint8       **blob_out,
                  gsize         *blob_len_out,
                  GError       **error)
{
    guint8 root[16] = { 0 };
    guint8 iv[16] = { 0 };
    guint8 key_material[32] = { 0 };
    guint8 *padded = NULL;
    guint8 *blob = NULL;
    gsize padded_len, blob_len;
    EVP_CIPHER_CTX *ctx = NULL;
    GError *local = NULL;
    int len = 0, final_len = 0;
    gboolean ok = FALSE;

    g_return_val_if_fail (plaintext != NULL || plaintext_len == 0, FALSE);
    g_return_val_if_fail (blob_out != NULL && blob_len_out != NULL, FALSE);
    if (plaintext_len > G_MAXINT - 16)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                          "plaintext exceeds the OpenSSL EVP size limit");

    *blob_out = NULL;
    *blob_len_out = 0;

    if (!gm168_wb_derive_root_secret (root, &local))
        goto out;
    if (!gm168_wb_derive_iv ((guint32) plaintext_len, iv, &local))
        goto out;
    if (!derive_key_material (iv, root, key_material, &local))
        goto out;

    padded_len = ((plaintext_len / 16) + 1) * 16;
    blob_len = 16 + padded_len + 32;
    padded = g_malloc (padded_len);
    blob = g_malloc (blob_len);

    memset (padded + plaintext_len, (int)(padded_len - plaintext_len),
            padded_len - plaintext_len);
    if (plaintext_len > 0)
        memcpy (padded, plaintext, plaintext_len);

    crypto_drain_errors ();
    ctx = EVP_CIPHER_CTX_new ();
    if (ctx == NULL)
        goto crypto_fail;
    if (EVP_EncryptInit_ex (ctx, EVP_aes_128_cbc (), NULL, key_material,
                            iv) != 1)
        goto crypto_fail;
    /* PKCS#7 padding is applied manually above; keep the cipher raw. */
    if (EVP_CIPHER_CTX_set_padding (ctx, 0) != 1)
        goto crypto_fail;
    if (EVP_EncryptUpdate (ctx, blob + 16, &len, padded,
                           (int) padded_len) != 1)
        goto crypto_fail;
    if (EVP_EncryptFinal_ex (ctx, blob + 16 + len, &final_len) != 1)
        goto crypto_fail;
    if ((gsize) (len + final_len) != padded_len)
        goto crypto_fail;

    memcpy (blob, iv, 16);
    if (!hmac_sha256 (key_material, 32, blob + 16, padded_len, blob + 16 + padded_len,
                      &local))
        goto out;

    *blob_out = blob;
    *blob_len_out = blob_len;
    blob = NULL;
    ok = TRUE;
    goto out;

crypto_fail:
    g_clear_error (&local);
    crypto_failed (&local);
    goto out;

out:
    if (ctx != NULL)
        EVP_CIPHER_CTX_free (ctx);
    if (padded != NULL) {
        OPENSSL_cleanse (padded, padded_len);
        g_free (padded);
    }
    if (blob != NULL) {
        OPENSSL_cleanse (blob, blob_len);
        g_free (blob);
    }
    OPENSSL_cleanse (root, sizeof (root));
    OPENSSL_cleanse (iv, sizeof (iv));
    OPENSSL_cleanse (key_material, sizeof (key_material));
    if (!ok)
        crypto_drain_errors ();
    propagate_local (error, local);
    return ok;
}

gboolean
gm168_wb_decrypt (const guint8  *blob,
                  gsize          blob_len,
                  guint8       **plaintext_out,
                  gsize         *plaintext_len_out,
                  GError       **error)
{
    guint8 root[16] = { 0 };
    guint8 iv[16] = { 0 };
    guint8 derived_iv[16] = { 0 };
    guint8 key_material[32] = { 0 };
    guint8 expected_tag[32] = { 0 };
    guint8 *padded = NULL;
    guint8 *plaintext = NULL;
    gsize ct_len, padded_len, plaintext_len, pad;
    EVP_CIPHER_CTX *ctx = NULL;
    GError *local = NULL;
    int len = 0, final_len = 0;
    gboolean ok = FALSE;

    g_return_val_if_fail (blob != NULL, FALSE);
    g_return_val_if_fail (plaintext_out != NULL && plaintext_len_out != NULL,
                          FALSE);

    *plaintext_out = NULL;
    *plaintext_len_out = 0;

    if (blob_len < 64 || blob_len % 16 != 0 || blob_len - 48 > G_MAXINT)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                          "invalid or unsupported whitebox blob length");

    ct_len = blob_len - 16 - 32;

    if (!gm168_wb_derive_root_secret (root, &local))
        goto out;
    memcpy (iv, blob, 16);
    if (!derive_key_material (iv, root, key_material, &local))
        goto out;

    /* HMAC-SHA256 over the ciphertext is verified BEFORE any AES operation. */
    if (!hmac_sha256 (key_material, 32, blob + 16, ct_len, expected_tag,
                      &local))
        goto out;
    if (CRYPTO_memcmp (blob + blob_len - 32, expected_tag, 32) != 0) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                   "whitebox HMAC mismatch");
        goto out;
    }

    padded = g_malloc (ct_len);
    crypto_drain_errors ();
    ctx = EVP_CIPHER_CTX_new ();
    if (ctx == NULL)
        goto crypto_fail;
    if (EVP_DecryptInit_ex (ctx, EVP_aes_128_cbc (), NULL, key_material,
                            iv) != 1)
        goto crypto_fail;
    if (EVP_CIPHER_CTX_set_padding (ctx, 0) != 1)
        goto crypto_fail;
    if (EVP_DecryptUpdate (ctx, padded, &len, blob + 16, (int) ct_len) != 1)
        goto crypto_fail;
    padded_len = (gsize) len;
    if (EVP_DecryptFinal_ex (ctx, padded + padded_len, &final_len) != 1)
        goto crypto_fail;
    padded_len += (gsize) final_len;

    /* Manual PKCS#7 unpad (block size 16). */
    if (padded_len == 0 || padded_len % 16 != 0) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                   "invalid whitebox padding length");
        goto out;
    }
    pad = padded[padded_len - 1];
    if (pad < 1 || pad > 16 || pad > padded_len) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                   "invalid whitebox padding");
        goto out;
    }
    for (gsize i = 0; i < pad; i++) {
        if (padded[padded_len - 1 - i] != pad) {
            set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                       "invalid whitebox padding");
            goto out;
        }
    }
    plaintext_len = padded_len - pad;

    /* The IV must be canonical for the recovered plaintext length. */
    if (plaintext_len > G_MAXUINT32) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                   "recovered plaintext length is out of range");
        goto out;
    }
    if (!gm168_wb_derive_iv ((guint32) plaintext_len, derived_iv, &local))
        goto out;
    if (CRYPTO_memcmp (iv, derived_iv, 16) != 0) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_WHITEBOX,
                   "non-canonical whitebox IV for plaintext length");
        goto out;
    }

    plaintext = g_malloc (plaintext_len > 0 ? plaintext_len : 1);
    if (plaintext_len > 0)
        memcpy (plaintext, padded, plaintext_len);

    *plaintext_out = plaintext;
    *plaintext_len_out = plaintext_len;
    plaintext = NULL;
    ok = TRUE;
    goto out;

crypto_fail:
    g_clear_error (&local);
    crypto_failed (&local);
    goto out;

out:
    if (ctx != NULL)
        EVP_CIPHER_CTX_free (ctx);
    if (padded != NULL) {
        OPENSSL_cleanse (padded, ct_len);
        g_free (padded);
    }
    if (plaintext != NULL) {
        OPENSSL_cleanse (plaintext, plaintext_len);
        g_free (plaintext);
    }
    OPENSSL_cleanse (root, sizeof (root));
    OPENSSL_cleanse (iv, sizeof (iv));
    OPENSSL_cleanse (derived_iv, sizeof (derived_iv));
    OPENSSL_cleanse (key_material, sizeof (key_material));
    OPENSSL_cleanse (expected_tag, sizeof (expected_tag));
    if (!ok)
        crypto_drain_errors ();
    propagate_local (error, local);
    return ok;
}

// ─── Pure E4 reply validators ───────────────────────────────────────────────

static guint32
read_le32 (const guint8 *p)
{
    return (guint32) p[0]        |
           (guint32) p[1] <<  8 |
           (guint32) p[2] << 16 |
           (guint32) p[3] << 24;
}

gboolean
gm168_psk_validate_hash_reply (const guint8 *extra,
                               gsize         extra_len,
                               guint8        hash_out[GM168_PSK_HASH_LEN],
                               GError      **error)
{
    guint32 tag, declared;

    g_return_val_if_fail (extra != NULL || extra_len == 0, FALSE);
    g_return_val_if_fail (hash_out != NULL, FALSE);

    memset (hash_out, 0, GM168_PSK_HASH_LEN);

    if (extra_len != 8 + GM168_PSK_HASH_LEN)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "PSK hash reply has an unexpected size");

    tag = read_le32 (extra);
    declared = read_le32 (extra + 4);
    if (tag != GM168_PSK_HASH_TLV_TAG)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "unexpected PSK hash reply tag 0x%08x", tag);
    if (declared != GM168_PSK_HASH_LEN)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "PSK hash reply length mismatch: declared %u",
                          declared);

    memcpy (hash_out, extra + 8, GM168_PSK_HASH_LEN);
    return TRUE;
}

gboolean
gm168_psk_validate_area_reply (const guint8 *extra,
                               gsize         extra_len,
                               guint32       requested_len,
                               guint32       requested_offset,
                               guint8       *chunk_out,
                               guint32      *chunk_len_out,
                               GError      **error)
{
    guint32 tag, declared;
    guint64 chunk_end;

    g_return_val_if_fail (extra != NULL || extra_len == 0, FALSE);
    g_return_val_if_fail (chunk_out != NULL && chunk_len_out != NULL, FALSE);

    *chunk_len_out = 0;

    if (requested_len == 0 || requested_len > GM168_PSK_AREA_CHUNK)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "area read request length %u is out of range",
                          requested_len);
    if (requested_offset >= GM168_PSK_MAX_AREA)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "area read offset %u is out of range",
                          requested_offset);

    if (extra_len < 8)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "short E4 area reply");
    if (extra_len - 8 > requested_len)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "E4 area reply exceeds the requested length");

    chunk_end = (guint64) requested_offset + (guint64) (extra_len - 8);
    if (chunk_end > GM168_PSK_MAX_AREA)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "E4 area reply exceeds the bounded area");

    tag = read_le32 (extra);
    declared = read_le32 (extra + 4);
    if (tag != GM168_PSK_AREA_TLV_TAG)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "unexpected E4 area reply tag 0x%08x", tag);
    if (declared != extra_len - 8)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "E4 area reply length mismatch: declared %u",
                          declared);
    if (extra_len - 8 == 0)
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "E4 area reply made no progress");

    memcpy (chunk_out, extra + 8, extra_len - 8);
    *chunk_len_out = (guint32) (extra_len - 8);
    return TRUE;
}

// ─── Full read-only recovery over a buffered area prefix ────────────────────

static gboolean
buffer_is_all_zero (const guint8 *data, gsize len)
{
    guint8 acc = 0;

    for (gsize i = 0; i < len; i++)
        acc |= data[i];
    return acc == 0;
}

gboolean
gm168_psk_validate_expected_hash (
    const guint8  expected_hash[GM168_PSK_HASH_LEN],
    GError      **error)
{
    guint8 zero_psk[GM168_PSK_LEN] = { 0 };
    guint8 zero_psk_hash[GM168_PSK_HASH_LEN] = { 0 };
    gboolean valid = FALSE;

    g_return_val_if_fail (expected_hash != NULL, FALSE);

    if (buffer_is_all_zero (expected_hash, GM168_PSK_HASH_LEN))
        return set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                          "expected PSK hash digest is all-zero");

    if (!sha256_digest (zero_psk, sizeof (zero_psk), zero_psk_hash, error))
        goto out;
    if (CRYPTO_memcmp (expected_hash, zero_psk_hash, GM168_PSK_HASH_LEN) == 0) {
        set_error (error, GM168_PSK_RECOVERY_ERROR_VERIFY,
                   "expected PSK hash matches the all-zero PSK sentinel");
        goto out;
    }
    valid = TRUE;

out:
    OPENSSL_cleanse (zero_psk, sizeof (zero_psk));
    OPENSSL_cleanse (zero_psk_hash, sizeof (zero_psk_hash));
    return valid;
}

gboolean
gm168_psk_recover_from_area (const guint8 *area_prefix,
                             gsize         area_len,
                             const guint8  expected_hash[GM168_PSK_HASH_LEN],
                             guint8        psk_out[GM168_PSK_LEN],
                             guint32      *record_offset_out,
                             GError      **error)
{
    guint32 total_len = 0, record_offset = 0;
    guint8 blob[GM168_PSK_WHITEBOX_LEN] = { 0 };
    guint8 recovered_hash[GM168_PSK_HASH_LEN] = { 0 };
    guint8 *recovered = NULL;
    gsize recovered_len = 0;
    Gm168DpapiParseResult parse_res;
    GError *local = NULL;
    gboolean ok = FALSE;

    g_return_val_if_fail (area_prefix != NULL || area_len == 0, FALSE);
    g_return_val_if_fail (expected_hash != NULL, FALSE);
    g_return_val_if_fail (psk_out != NULL, FALSE);

    /* Fail-closed: the PSK output is zeroized up front and only filled on
     * full success, so no error path can leave key material behind. */
    OPENSSL_cleanse (psk_out, GM168_PSK_LEN);
    if (record_offset_out != NULL)
        *record_offset_out = 0;

    /* 1. Repeat the fresh expected-hash gate for standalone callers. The USB
     *    integration invokes it before the first area read as well. */
    if (!gm168_psk_validate_expected_hash (expected_hash, &local))
        goto out;

    /* 2. Dynamic DPAPI v1 total length — the record offset is the parsed
     *    total, never a hard-coded constant. */
    parse_res = gm168_dpapi_parse_total_length (area_prefix, area_len,
                                                &total_len, &local);
    if (parse_res == GM168_DPAPI_NEED_MORE) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "DPAPI area prefix is incomplete");
        goto out;
    }
    if (parse_res != GM168_DPAPI_OK)
        goto out;

    if ((guint64) total_len + GM168_PSK_RECORD_LEN > area_len) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_DPAPI,
                   "area prefix does not contain the complete whitebox record");
        goto out;
    }
    record_offset = total_len;

    /* 3. Exact TLV record + public zero-PSK whitebox sentinel gate. */
    if (!gm168_psk_parse_whitebox_record (area_prefix + record_offset,
                                          GM168_PSK_RECORD_LEN, blob,
                                          &local))
        goto out;
    if (CRYPTO_memcmp (blob, gm168_public_zero_psk_whitebox,
                       GM168_PSK_WHITEBOX_LEN) == 0) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_VERIFY,
                   "stored whitebox blob is the public zero-PSK sentinel");
        goto out;
    }

    /* 4. Whitebox decrypt (HMAC before AES, PKCS#7, canonical IV). */
    if (!gm168_wb_decrypt (blob, GM168_PSK_WHITEBOX_LEN, &recovered,
                           &recovered_len, &local))
        goto out;

    /* 5. Shape and sentinel checks on the recovered plaintext. */
    if (recovered_len != GM168_PSK_LEN) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_VERIFY,
                   "recovered PSK length does not match the expected size");
        goto out;
    }
    if (buffer_is_all_zero (recovered, recovered_len)) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_VERIFY,
                   "recovered PSK is all-zero");
        goto out;
    }

    /* 6. SHA256(recovered PSK) must match the fresh E4 hash. */
    if (!sha256_digest (recovered, recovered_len, recovered_hash, &local))
        goto out;
    if (CRYPTO_memcmp (recovered_hash, expected_hash, GM168_PSK_HASH_LEN) != 0) {
        set_error (&local, GM168_PSK_RECOVERY_ERROR_VERIFY,
                   "recovered PSK hash does not match the expected hash");
        goto out;
    }

    memcpy (psk_out, recovered, GM168_PSK_LEN);
    if (record_offset_out != NULL)
        *record_offset_out = record_offset;
    ok = TRUE;

out:
    if (recovered != NULL) {
        OPENSSL_cleanse (recovered, recovered_len);
        g_free (recovered);
    }
    OPENSSL_cleanse (blob, sizeof (blob));
    OPENSSL_cleanse (recovered_hash, sizeof (recovered_hash));
    if (!ok) {
        OPENSSL_cleanse (psk_out, GM168_PSK_LEN);
        if (record_offset_out != NULL)
            *record_offset_out = 0;
    }
    propagate_local (error, local);
    return ok;
}
