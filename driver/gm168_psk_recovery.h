// SPDX-License-Identifier: LGPL-2.1-or-later
// gm168_psk_recovery.h — read-only GM168 sealed-PSK recovery (pure offline crypto)
//
// Implements docs/PSK-RECOVERY.md: validates a buffered E4
// area prefix, locates the 104-byte whitebox TLV record directly after the
// DPAPI v1 blob (offset discovered dynamically, never hard-coded), decrypts
// the whitebox blob and verifies SHA256(recovered PSK) against a fresh E4
// 0xbb020001 hash. No USB, no filesystem access, no secrets are ever read:
// the module only processes caller-supplied buffers.
//
// All diagnostics are built from lengths, tags and types only; no raw area
// bytes, blob bytes, PSK material or OpenSSL error data ever leak into GError
// messages.

#pragma once

#include <glib.h>

G_BEGIN_DECLS

// ─── Public bounded-area limits ──────────────────────────────────────────────
#define GM168_PSK_LEN            32
#define GM168_PSK_HASH_LEN       32
#define GM168_PSK_WHITEBOX_LEN   96
#define GM168_PSK_RECORD_LEN     (8 + GM168_PSK_WHITEBOX_LEN)   /* 104 */
#define GM168_PSK_MAX_AREA       4096
#define GM168_PSK_MAX_DPAPI      (GM168_PSK_MAX_AREA - GM168_PSK_RECORD_LEN) /* 3992 */
#define GM168_PSK_AREA_CHUNK     256
#define GM168_PSK_MAX_AREA_READS 16

// ─── E4 TLV tags ────────────────────────────────────────────────────────────
#define GM168_PSK_AREA_TLV_TAG   0xbb010002u
#define GM168_PSK_RECORD_TLV_TAG 0xbb010003u
#define GM168_PSK_HASH_TLV_TAG   0xbb020001u

/* Deterministic 96-byte encryption of the all-zero 32-byte PSK: the unique
 * sensor-side "no PSK" sentinel. A stored blob equal to this constant is
 * rejected at the record gate before any decryption is attempted. */
extern const guint8 gm168_public_zero_psk_whitebox[GM168_PSK_WHITEBOX_LEN];

#define GM168_PSK_RECOVERY_ERROR (gm168_psk_recovery_error_quark ())

typedef enum {
    GM168_PSK_RECOVERY_ERROR_DPAPI = 1,    // DPAPI v1 structure invalid/incomplete
    GM168_PSK_RECOVERY_ERROR_RECORD,       // whitebox TLV record invalid
    GM168_PSK_RECOVERY_ERROR_WHITEBOX,     // whitebox blob invalid (HMAC/padding/IV)
    GM168_PSK_RECOVERY_ERROR_VERIFY,       // semantic gate failed (sentinels/hash)
    GM168_PSK_RECOVERY_ERROR_CRYPTO,       // internal OpenSSL operation failed
} Gm168PskRecoveryError;

GQuark gm168_psk_recovery_error_quark (void);

// ─── DPAPI v1 dynamic total-length parser ───────────────────────────────────
// Parses the verified DPAPI v1 field sequence (dwVersion==1, guidProvider,
// dwMasterKeyVersion, guidMasterKey, dwFlags, dwDescriptionLen+description,
// dwCryptAlgo, dwCryptAlgoLen (bit length, no payload), dwSaltLen+salt,
// dwPromptLen+prompt, dwSignAlgo, dwSignAlgoLen (bit length, no payload),
// dwSignSaltLen+signSalt, dwDataLen+data, dwSignLen+signature) and returns
// the total length in *total_len.
//
// GM168_DPAPI_NEED_MORE: the buffer ends mid-field; no GError is set.
// GM168_DPAPI_INVALID:   a field violates the bounds (GError is set).
// GM168_DPAPI_OK:        *total_len holds the total (<= GM168_PSK_MAX_DPAPI).
// Cumulative bounds are checked against GM168_PSK_MAX_DPAPI before the
// buffer-end check, so impossible sizes are rejected immediately even on an
// incomplete prefix. Never hard-codes any record offset.
typedef enum {
    GM168_DPAPI_NEED_MORE = 0,
    GM168_DPAPI_OK,
    GM168_DPAPI_INVALID,
} Gm168DpapiParseResult;

Gm168DpapiParseResult gm168_dpapi_parse_total_length (const guint8 *data,
                                                      gsize         len,
                                                      guint32      *total_len,
                                                      GError      **error);

// ─── Exact whitebox TLV record parser ───────────────────────────────────────
// Requires exactly GM168_PSK_RECORD_LEN (104) bytes:
// tag LE32 == GM168_PSK_RECORD_TLV_TAG, length LE32 == GM168_PSK_WHITEBOX_LEN
// (96), followed by the 96-byte blob. No payload-only or alternate-offset
// fallback exists. On success blob_out receives the 96-byte blob.
gboolean gm168_psk_parse_whitebox_record (const guint8 *record,
                                          gsize         record_len,
                                          guint8        blob_out[GM168_PSK_WHITEBOX_LEN],
                                          GError      **error);

// ─── Goodix SecWhiteEncrypt primitives (OpenSSL 3, no deprecated HMAC_CTX) ──
// Reconstructs the fixed 16-byte Goodix root without embedding its value
// (ROR8 schedule + SHA256 + AES-CBC pairs + HMAC-SHA256 + CRC32/MPEG-2,
// exactly as the synthetic reference model).
gboolean gm168_wb_derive_root_secret (guint8  root[16],
                                      GError **error);

// Canonical IV: SHA256(LE32(plaintext_len) || "123GOODIX")[:16] with the low
// nibble of byte 15 replaced by plaintext_len & 0x0F.
gboolean gm168_wb_derive_iv (guint32  plaintext_len,
                             guint8   iv[16],
                             GError **error);

// Offline KAT/test helper mirroring sec_white_encrypt(): returns a newly
// allocated IV || AES-128-CBC(PKCS#7 plaintext) || HMAC-SHA256 blob.
gboolean gm168_wb_encrypt (const guint8  *plaintext,
                           gsize          plaintext_len,
                           guint8       **blob_out,
                           gsize         *blob_len_out,
                           GError       **error);

// Whitebox decrypt: blob must be >= 64 bytes and a multiple of 16.
// HMAC-SHA256 is verified before any AES operation (CRYPTO_memcmp), the
// ciphertext is decrypted with AES-128-CBC and PKCS#7-unpadded, and the IV
// must match derive_iv(plaintext_len) (canonical IV). All temporary buffers
// are OPENSSL_cleanse()d; OpenSSL errors never leave a stale error queue.
// On success the caller owns *plaintext_out (g_free()) and *plaintext_len_out.
gboolean gm168_wb_decrypt (const guint8  *blob,
                           gsize          blob_len,
                           guint8       **plaintext_out,
                           gsize         *plaintext_len_out,
                           GError       **error);

// ─── Pure E4 reply validators (no USB transport) ────────────────────────────
// Validates a cmd 0xE4 reply "extra" slice of the form
// [tag_echo:4 LE][declared_len:4 LE][data:N] produced by find_a0_ack().
//
// Hash reply: extra must be exactly 8+GM168_PSK_HASH_LEN bytes, tag
// GM168_PSK_HASH_TLV_TAG, declared length 32. On success hash_out receives
// the 32-byte SHA256(PSK).
gboolean gm168_psk_validate_hash_reply (const guint8 *extra,
                                        gsize         extra_len,
                                        guint8        hash_out[GM168_PSK_HASH_LEN],
                                        GError      **error);

// Reject an all-zero digest and SHA256(00*32) immediately after the fresh hash
// read, before the first E4 area request. The full recovery helper applies the
// same gate again for callers that use it independently.
gboolean gm168_psk_validate_expected_hash (
    const guint8  expected_hash[GM168_PSK_HASH_LEN],
    GError      **error);

// Area reply: extra must be exactly 8+chunk bytes with tag
// GM168_PSK_AREA_TLV_TAG and declared length == chunk, where
// 1 <= chunk <= requested_len <= GM168_PSK_AREA_CHUNK, and
// requested_offset + chunk <= GM168_PSK_MAX_AREA (checked without overflow).
// On success chunk_out receives chunk bytes and *chunk_len_out the chunk size.
gboolean gm168_psk_validate_area_reply (const guint8 *extra,
                                        gsize         extra_len,
                                        guint32       requested_len,
                                        guint32       requested_offset,
                                        guint8       *chunk_out,
                                        guint32      *chunk_len_out,
                                        GError      **error);

// ─── Full read-only recovery over a buffered area prefix ────────────────────
// Gates, in order:
//   1. expected_hash must not be all-zero and must not equal SHA256(00*32)
//      (the sensor-side "no PSK" sentinel);
//   2. the DPAPI v1 total length is parsed dynamically (offset never
//      hard-coded) and the prefix must cover total+GM168_PSK_RECORD_LEN;
//   3. the exact TLV record at that offset must not be the public
//      zero-PSK whitebox sentinel;
//   4. the blob must decrypt (HMAC, PKCS#7, canonical IV) to exactly
//      GM168_PSK_LEN non-zero bytes;
//   5. SHA256(recovered PSK) must match expected_hash (CRYPTO_memcmp).
//
// On success psk_out receives the 32-byte PSK (the only output location that
// ever holds it) and *record_offset_out (optional) the discovered DPAPI
// total. On ANY failure psk_out is fully zeroized and no key material is
// retained anywhere in the module.
gboolean gm168_psk_recover_from_area (const guint8 *area_prefix,
                                      gsize         area_len,
                                      const guint8  expected_hash[GM168_PSK_HASH_LEN],
                                      guint8        psk_out[GM168_PSK_LEN],
                                      guint32      *record_offset_out,
                                      GError      **error);

G_END_DECLS
