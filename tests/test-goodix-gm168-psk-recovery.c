// SPDX-License-Identifier: LGPL-2.1-or-later
/* tests/test-goodix-gm168-psk-recovery.c
 *
 * Synthetic offline unit tests for gm168_psk_recovery (read-only GM168 PSK
 * recovery). No USB, no filesystem access, no real psk.bin / DPAPI / OEM DLL:
 * every vector is built locally or pinned from the synthetic reference KATs.
 * Keys and blobs are never logged.
 */

#include <glib.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <stdio.h>
#include <string.h>

#include "../libfprint/drivers/goodix_gm168/gm168_psk_recovery.h"
#include "../libfprint/drivers/goodix_gm168/goodix_proto.h"

/* helper: count non-zero bytes without branching on each byte's value */
static gint
memcnt_nonzero (const guint8 *data, gsize len)
{
    gint count = 0;
    for (gsize i = 0; i < len; i++)
        count += data[i] != 0;
    return count;
}

// ─── Deterministic non-secret probe vector ──────────────────────────────────

#define PROBE_HASH_HEX "83b7a8ed859053c81d818870fab1f8b1ae44d06a98a9665d369a8fd7d2838ded"
#define ZERO_PSK_HASH_HEX "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925"

/* Whitebox blob of the deterministic probe PSK (pinned from
 * the synthetic reference; its SHA256 is PROBE_HASH_HEX). */
#define PROBE_BLOB_HEX \
    "ec35ae3abb45ed3f12c4751f1e5c2cc0087696546b940861385337e40e64f71" \
    "32cc50c4c503b8cc99f42173aa2493f0d5b2e82759e156ccde2755b9deb4bd6c3" \
    "7b2e06c0280441d35ef4ae7b0dbcefb459a743014477ea07261ec81d0fc6435c"

static void
fill_probe_psk (guint8 out[GM168_PSK_LEN])
{
    for (guint i = 0; i < GM168_PSK_LEN; i++)
        out[i] = (guint8) (i * 37 + 11);
}

static void
fill_kat_pattern (guint8 *out, gsize len)
{
    for (gsize i = 0; i < len; i++)
        out[i] = (guint8) (i * 73 + 19);
}

static void
fill_range (guint8 *out, gsize len, guint start)
{
    for (gsize i = 0; i < len; i++)
        out[i] = (guint8) (start + i);
}

// ─── Hex / digest helpers ───────────────────────────────────────────────────

static void
hex_decode (const gchar *hex, guint8 *out, gsize expected_len)
{
    g_assert_cmpint ((gsize) strlen (hex), ==, expected_len * 2);
    for (gsize i = 0; i < expected_len; i++) {
        guint byte = 0;
        g_assert_cmpint (sscanf (hex + 2 * i, "%2x", &byte), ==, 1);
        out[i] = (guint8) byte;
    }
}

static void
sha256_hex (const guint8 *data, gsize len, gchar out_hex[65])
{
    guint8 digest[32];
    gsize digest_len = sizeof (digest);
    GChecksum *cs = g_checksum_new (G_CHECKSUM_SHA256);

    g_checksum_update (cs, (const guchar *) data, (gssize) len);
    g_checksum_get_digest (cs, digest, &digest_len);
    g_assert_cmpint (digest_len, ==, 32);
    g_checksum_free (cs);

    for (gsize i = 0; i < 32; i++)
        g_snprintf (out_hex + 2 * i, 3, "%02x", digest[i]);
    out_hex[64] = '\0';
}

// ─── Independent synthetic DPAPI v1 builder (port of the Python test builder)

static void
put_le32 (GByteArray *out, guint32 v)
{
    guint8 b[4] = {
        (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24),
    };
    g_byte_array_append (out, b, 4);
}

static void
patch_le32 (GByteArray *out, gsize offset, guint32 v)
{
    g_assert_cmpint (offset + 4, <=, out->len);
    out->data[offset] = (guint8) v;
    out->data[offset + 1] = (guint8) (v >> 8);
    out->data[offset + 2] = (guint8) (v >> 16);
    out->data[offset + 3] = (guint8) (v >> 24);
}

static GByteArray *
build_dpapi (guint32       version,
             const guint8 *desc, gsize desc_len,
             const guint8 *salt, gsize salt_len,
             const guint8 *prompt, gsize prompt_len,
             const guint8 *sign_salt, gsize sign_salt_len,
             const guint8 *data, gsize data_len,
             const guint8 *sig, gsize sig_len,
             guint32       crypt_algo_len,
             guint32       sign_algo_len)
{
    static const guint8 provider_guid[16] = { 0xd0, 0x8c, 0x9d, 0xdf };
    GByteArray *out = g_byte_array_new ();
    guint8 range16[16];

    fill_range (range16, sizeof (range16), 0);

    put_le32 (out, version);
    g_byte_array_append (out, provider_guid, sizeof (provider_guid));
    put_le32 (out, 1);                    // dwMasterKeyVersion
    g_byte_array_append (out, range16, 16); // guidMasterKey
    put_le32 (out, 0);                    // dwFlags
    put_le32 (out, (guint32) desc_len);
    if (desc_len > 0)
        g_byte_array_append (out, desc, desc_len);
    put_le32 (out, 0x00006603);           // dwCryptAlgo
    put_le32 (out, crypt_algo_len);       // dwCryptAlgoLen (bit length)
    put_le32 (out, (guint32) salt_len);
    if (salt_len > 0)
        g_byte_array_append (out, salt, salt_len);
    put_le32 (out, (guint32) prompt_len);
    if (prompt_len > 0)
        g_byte_array_append (out, prompt, prompt_len);
    put_le32 (out, 0x00008004);           // dwSignAlgo
    put_le32 (out, sign_algo_len);        // dwSignAlgoLen (bit length)
    put_le32 (out, (guint32) sign_salt_len);
    if (sign_salt_len > 0)
        g_byte_array_append (out, sign_salt, sign_salt_len);
    put_le32 (out, (guint32) data_len);
    if (data_len > 0)
        g_byte_array_append (out, data, data_len);
    put_le32 (out, (guint32) sig_len);
    if (sig_len > 0)
        g_byte_array_append (out, sig, sig_len);
    return out;
}

/* Fixed-header DPAPI blob with description 64, salt 16, empty prompt and
 * signSalt, data 96, signature 64: total exactly 324 (never hard-coded in
 * the parser under test — only in this builder). */
static GByteArray *
build_reference_324_blob (void)
{
    guint8 desc[64], salt[16], data[96], sig[64];
    GByteArray *out;

    fill_range (desc, sizeof (desc), 0);
    fill_range (salt, sizeof (salt), 0);
    fill_range (data, sizeof (data), 0);
    fill_range (sig, sizeof (sig), 0);

    out = build_dpapi (1, desc, sizeof (desc), salt, sizeof (salt),
                       NULL, 0, NULL, 0, data, sizeof (data), sig,
                       sizeof (sig), 16, 20);
    g_assert_cmpint (out->len, ==, 324);
    return out;
}

static GByteArray *
build_default_dpapi (void)
{
    guint8 salt[16], data[144], sig[40];

    fill_range (salt, sizeof (salt), 0);
    fill_range (data, sizeof (data), 32);
    fill_range (sig, sizeof (sig), 176);

    return build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        data, sizeof (data), sig, sizeof (sig), 16, 20);
}

static GByteArray *
build_record (const guint8 *blob, gsize blob_len, guint32 tag, guint32 declared)
{
    GByteArray *out = g_byte_array_new ();

    put_le32 (out, tag);
    put_le32 (out, declared);
    g_byte_array_append (out, blob, blob_len);
    return out;
}

/* Append a 96-byte whitebox record for blob after the DPAPI blob. */
static GByteArray *
append_record (GByteArray *area, const guint8 blob[GM168_PSK_WHITEBOX_LEN])
{
    GByteArray *record = build_record (blob, GM168_PSK_WHITEBOX_LEN,
                                       GM168_PSK_RECORD_TLV_TAG,
                                       GM168_PSK_WHITEBOX_LEN);
    g_byte_array_append (area, record->data, record->len);
    g_byte_array_free (record, TRUE);
    return area;
}

// ─── Pinned whitebox KAT vectors (the synthetic reference) ────────────────────

typedef enum {
    KAT_PLAINTEXT_SYNTHETIC, /* (i*73+19)&0xff */
    KAT_PLAINTEXT_ZERO,
    KAT_PLAINTEXT_FF,
    KAT_PLAINTEXT_RANGE,
    KAT_PLAINTEXT_DIGEST,    /* SHA256("GM168 synthetic KAT") */
} KatPlaintextSpec;

typedef struct {
    const gchar     *name;
    KatPlaintextSpec spec;
    gsize            plaintext_len;
    const gchar     *blob_hex;
    const gchar     *blob_sha256_hex;
} WbKat;

static const WbKat kat_cases[] = {
    {
        "empty", KAT_PLAINTEXT_SYNTHETIC, 0,
        "277f97e05ef707b2206f1c11bcbee580750427b9c4584b5901d63edfb6f3b7a3"
        "aecca2b8a3a0d0fac793a94ad8256d0b161c3e7fc7b7eed944ea2a141e4054d9",
        "5c3d8f179ccf3aa5c5487b773591704e4398bde9a31f362a5e2d3f2f4da28fca",
    },
    {
        "synthetic-1", KAT_PLAINTEXT_SYNTHETIC, 1,
        "db73b81796f5ceb6b53e114fe77ea6b112ffcf628befe0409c23e18c69120a78"
        "99924bd9fd7cda78c46231665c5a99f65068cf615d29bfa469820d68d97a632b",
        "6cfe513fe2ccf64590b06af9bd5bd2820950943a6fdf0591fafceb9bd3e77063",
    },
    {
        "synthetic-15", KAT_PLAINTEXT_SYNTHETIC, 15,
        "c8ee531887c81083b5b40c298999912f0bf2eaf550cefad536edbc7e9cbefd0d"
        "97e89251375247cb852063e714afac7a01b418d37d0b450836e63ea8c28fc184",
        "94be49f9aadb78dfdde2f62bbf6a7475cef1973e6b4aeb12508763bf535ae97c",
    },
    {
        "synthetic-16", KAT_PLAINTEXT_SYNTHETIC, 16,
        "e64a2f44ff5fb6c6e5ecdbc0363b9b90be08691f203699f2e2a84ec184238232"
        "db2855e2402859268ad59685f414ef65259539cfcb03880f8b72bac540038a1c"
        "e6c7e196c10e01d0d875e4590ef329bb",
        "7443ca39199dee7b42192b7b15c97e6b657c6839037573a474f49e76622cc9e3",
    },
    {
        "synthetic-17", KAT_PLAINTEXT_SYNTHETIC, 17,
        "9603b359596897515dbd6b02352002d1fe29c6b7d64360de5594a40185ba8616"
        "6f634ee7d0b20a3000fc020f896e629250c5d141cf6ce87b3b253180155e9ef2"
        "79fcd96b2017eb9615948e5c0c2d1cac",
        "51661436a935f410ce2b9176b52e9b28164741b4e6a9298186dcfc950d2a574d",
    },
    {
        "synthetic-31", KAT_PLAINTEXT_SYNTHETIC, 31,
        "e47a7425993c476f93267148f63223af244f047ced383494874f3f4d49a3cef7"
        "00aad713f0034c22aca2687b7e8b496ca1d228fbf23aa16a02848382bee2d325"
        "16a5644a72998c9117170d58d9501c64",
        "f8e0048b4606bb85fd56ef24682d26a5bb8ec0788a1689eaf102b3a1af4eb94e",
    },
    {
        "synthetic-32", KAT_PLAINTEXT_SYNTHETIC, 32,
        "ec35ae3abb45ed3f12c4751f1e5c2cc01ef9b24990344971c826749b71d16c84"
        "81add339c27a579f36a6f6c046e3855e6b4f28dfe02b9e77e0a55a2a2bdbdc48"
        "f51b59814ec8e6a54acfce8a2d2558737f055fe8b7008d522086fd8eca486dad",
        "30ea228e5db72d5fbcacf2aa28bc3c520dada447950ca869b445e22acd2f7176",
    },
    {
        "synthetic-33", KAT_PLAINTEXT_SYNTHETIC, 33,
        "211a6a44735b4d11c61297d94bd3d8c14968672fc92fbd00a72a1f15a8fb15fc"
        "7fb5ee2f7ef198c6282ccd26fe24856fa96aa1736f1b0a7c5f7eb32060af5f01"
        "0a058f775ce05babf70320b33e48fa7788e1c1e7cdc51f93f61b9a763e4552ff",
        "54d3d85c51a18fb644760aab2b24e218258329ca7cd917a17fd38558e88493d0",
    },
    {
        "synthetic-48", KAT_PLAINTEXT_SYNTHETIC, 48,
        "81187d465cf9c42717483bdac8dafbe014f66ba9222612a1cd4f37c78a543b57"
        "12122f07cb1efaa35cddef21577996bf32dbf7dccbc2b33d2a3083f406d6c2d8"
        "440329ee5dfcb887526dc4bb7aa24b74ac8c965a202c63294c7f62465f2c53a0"
        "bb4a9d5ba0b9a00eff82df871e61eccf",
        "03190b92c2b4113f30beb9ec1535dea5d94b3dfaafa6fea0eb1931b3bc666e38",
    },
    {
        "synthetic-64", KAT_PLAINTEXT_SYNTHETIC, 64,
        "286a07da28e896676ecf8677816ae0b0440cc0417bd19ed0fce74f3867155b36"
        "2095fa476d9db5594efaf7274dcf4a09cbee8ba36aa90e991dc7314972b3fdc3"
        "526ac160e24a35ee084f5be105f74c8a3064a381b0741952732beffe53923925"
        "25b942513e260f3d2e72011a5ea67c26a35a5af02aa461c8da9fee1993b633f7",
        "41f424795c9739999a0f5f6e3a3e6e0e6e7ff1606b3e0bb6f76e2bbbae13e321",
    },
    {
        "zero-32", KAT_PLAINTEXT_ZERO, 32,
        "ec35ae3abb45ed3f12c4751f1e5c2cc05b3c5452e9104d9f2a3118644f37a04b"
        "6fd66b1d97cf80f1345f76c84f03ff30bb51bf308f2a9875c41e6592cd2a2f9e"
        "60809b17b5316037b69bb2fa5d4c8ac31edb3394046ec06bbdacc57da6a756c5",
        "ba1a86037c1d3c71c3af344955bd69a9a9861d9e911fa24985b677e8dbd72d43",
    },
    {
        "ff-32", KAT_PLAINTEXT_FF, 32,
        "ec35ae3abb45ed3f12c4751f1e5c2cc068adff752ee5f8e0feee97d6a9b9f302"
        "765b3c35db3a89a2cf3a511ea95c4dc14ba182f0b30edf5e1e8d3e329bedc8c1"
        "6900bb8ed769485cfaa17d2daf79b467c9aa63bf9ea11b7953c44602790821d1",
        "eeeadbbf8ffde3d23496f5b733365661b85118348758d1f9728a700dee7ca276",
    },
    {
        "range-32", KAT_PLAINTEXT_RANGE, 32,
        "ec35ae3abb45ed3f12c4751f1e5c2cc019147cbb4d1b672389b25e83c8929855"
        "1a3b4e501275261e16977e5c7f516dbb416b4d71c942e9abe525635e351b65e4"
        "b4abb2eb90f69bc19bae6e08d58d52405eadb22a95e9e1839bebb37bf8aaace2",
        "8108aedfdbeac7791c1ebaa59984a5caa9c0684cd26ebf9b29602a96383fff0b",
    },
    {
        "digest-32", KAT_PLAINTEXT_DIGEST, 32,
        "ec35ae3abb45ed3f12c4751f1e5c2cc06c93eddd2a3ed7ef0b51546770c0788a"
        "28ffd1b171aeb11209ba674ba3473e96ecce9caf47d42d43139cdd4e492db63e"
        "b199c45529e554713f12643479d438c4f47e869d4c6a80771e05a08ff143934a",
        "b20eaa03ff3ac8bb124df250c8dab0d9d59d56dff79ed59062bf9a7685d0e45b",
    },
};

/* Valid HMAC/IV, deterministically invalid PKCS#7 padding (raw 48 bytes whose
 * final byte is 0x00), pinned from the synthetic negative-control reference. */
#define CORRUPT_PADDING_HEX \
    "ec35ae3abb45ed3f12c4751f1e5c2cc05b3c5452e9104d9f2a3118644f37a04b" \
    "6fd66b1d97cf80f1345f76c84f03ff30a4760d17a06c36131ffc4ff0990a2348" \
    "0b68eb9b06808b300d33d24c64c3f0ef184d8590f440f089db1ae67cc1db50e9"

/* Valid HMAC and padding but an IV that does not match the plaintext length
 * (probe PSK encrypted under a mangled IV), pinned from the Python tests. */
#define NONCANONICAL_IV_HEX \
    "1335ae3abb45ed3f12c4751f1e5c2cc0087696546b940861385337e40e64f71" \
    "32cc50c4c503b8cc99f42173aa2493f0d5b2e82759e156ccde2755b9deb4bd6c3" \
    "dfad4d0700c00277fd4176af8ab96074f98eac42d27143780217d0a9452f9970"

static void
kat_plaintext (const WbKat *kat, guint8 *out)
{
    switch (kat->spec) {
    case KAT_PLAINTEXT_SYNTHETIC:
        fill_kat_pattern (out, kat->plaintext_len);
        break;
    case KAT_PLAINTEXT_ZERO:
        memset (out, 0, kat->plaintext_len);
        break;
    case KAT_PLAINTEXT_FF:
        memset (out, 0xff, kat->plaintext_len);
        break;
    case KAT_PLAINTEXT_RANGE:
        fill_range (out, kat->plaintext_len, 0);
        break;
    case KAT_PLAINTEXT_DIGEST: {
        gchar hex[65];
        sha256_hex ((const guint8 *) "GM168 synthetic KAT",
                    strlen ("GM168 synthetic KAT"), hex);
        hex_decode (hex, out, 32);
        break;
    }
    }
}

// ─── Root secret / public zero vector ───────────────────────────────────────

static void
test_root_secret_fingerprint (void)
{
    guint8 root[16];
    guint8 second_root[16];
    gchar hex[65];
    GError *error = NULL;

    g_assert_true (gm168_wb_derive_root_secret (root, &error));
    g_assert_no_error (error);
    g_assert_true (gm168_wb_derive_root_secret (second_root, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (root, 16, second_root, 16);

    sha256_hex (root, sizeof (root), hex);
    g_assert_cmpstr (hex, ==,
                     "493abbe1260fa215e55029364d3df271"
                     "2b33a3844a407b27b86ce73dc5a95342");
}

static void
test_public_zero_vector (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    gchar hex[65];
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    guint8 *encrypted = NULL;
    gsize encrypted_len = 0;
    GError *error = NULL;

    /* The module constant is exactly the pinned public zero-PSK whitebox. */
    hex_decode (kat_cases[10].blob_hex, blob, sizeof (blob));
    g_assert_cmpmem (blob, sizeof (blob), gm168_public_zero_psk_whitebox,
                     GM168_PSK_WHITEBOX_LEN);
    sha256_hex (blob, sizeof (blob), hex);
    g_assert_cmpstr (hex, ==, kat_cases[10].blob_sha256_hex);

    /* Decrypts to the all-zero 32-byte PSK. */
    g_assert_true (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                     &plaintext_len, &error));
    g_assert_no_error (error);
    g_assert_cmpint (plaintext_len, ==, GM168_PSK_LEN);
    g_assert_cmpint (memcnt_nonzero (plaintext, plaintext_len), ==, 0);

    /* And re-encrypting the all-zero PSK reproduces the constant exactly. */
    g_assert_true (gm168_wb_encrypt (plaintext, plaintext_len, &encrypted,
                                     &encrypted_len, &error));
    g_assert_no_error (error);
    g_assert_cmpint (encrypted_len, ==, GM168_PSK_WHITEBOX_LEN);
    g_assert_cmpmem (encrypted, encrypted_len, blob, sizeof (blob));

    g_free (plaintext);
    OPENSSL_cleanse (encrypted, encrypted_len);
    g_free (encrypted);
}

// ─── Roundtrip / KAT decrypt ────────────────────────────────────────────────

static void
test_kat_decrypt_roundtrip (void)
{
    for (gsize c = 0; c < G_N_ELEMENTS (kat_cases); c++) {
        const WbKat *kat = &kat_cases[c];
        gsize blob_len = strlen (kat->blob_hex) / 2;
        guint8 *blob = g_malloc (blob_len);
        guint8 expected[64] = { 0 };
        guint8 *plaintext = NULL;
        gsize plaintext_len = 0;
        gchar hex[65];
        GError *error = NULL;

        hex_decode (kat->blob_hex, blob, blob_len);
        sha256_hex (blob, blob_len, hex);
        g_assert_cmpstr (hex, ==, kat->blob_sha256_hex);

        g_assert_true (gm168_wb_decrypt (blob, blob_len, &plaintext,
                                         &plaintext_len, &error));
        g_assert_no_error (error);
        g_assert_cmpint (plaintext_len, ==, kat->plaintext_len);

        kat_plaintext (kat, expected);
        g_assert_cmpmem (plaintext, plaintext_len, expected,
                         kat->plaintext_len);

        OPENSSL_cleanse (plaintext, plaintext_len);
        g_free (plaintext);
        OPENSSL_cleanse (blob, blob_len);
        g_free (blob);
    }
}

static void
test_encrypt_decrypt_roundtrip (void)
{
    static const gsize lengths[] = { 0, 1, 15, 16, 17, 31, 32, 47, 48, 64, 100 };
    guint8 plaintext[100];

    fill_kat_pattern (plaintext, sizeof (plaintext));

    for (gsize i = 0; i < G_N_ELEMENTS (lengths); i++) {
        gsize len = lengths[i];
        guint8 *blob = NULL;
        gsize blob_len = 0;
        guint8 *recovered = NULL;
        gsize recovered_len = 0;
        GError *error = NULL;
        gsize expected_blob_len = 16 + ((len / 16) + 1) * 16 + 32;

        g_assert_true (gm168_wb_encrypt (plaintext, len, &blob, &blob_len,
                                         &error));
        g_assert_no_error (error);
        g_assert_cmpint (blob_len, ==, expected_blob_len);

        g_assert_true (gm168_wb_decrypt (blob, blob_len, &recovered,
                                         &recovered_len, &error));
        g_assert_no_error (error);
        g_assert_cmpint (recovered_len, ==, len);
        g_assert_cmpmem (recovered, recovered_len, plaintext, len);

        OPENSSL_cleanse (recovered, recovered_len);
        g_free (recovered);
        OPENSSL_cleanse (blob, blob_len);
        g_free (blob);
    }
}

static void
test_malformed_blob_lengths_rejected (void)
{
    static const gsize bad_lengths[] = { 0, 16, 32, 48, 63, 65 };
    guint8 blob[96] = { 0 };
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    GError *error = NULL;

    for (gsize i = 0; i < G_N_ELEMENTS (bad_lengths); i++) {
        error = NULL;
        g_assert_false (gm168_wb_decrypt (blob, bad_lengths[i], &plaintext,
                                          &plaintext_len, &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_WHITEBOX);
        g_clear_error (&error);
    }
    g_assert_null (plaintext);
}

static void
test_hmac_mismatch_rejected (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    GError *error = NULL;
    static const gsize tamper_offsets[] = { 0, 16, GM168_PSK_WHITEBOX_LEN - 1 };

    hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));

    for (gsize i = 0; i < G_N_ELEMENTS (tamper_offsets); i++) {
        blob[tamper_offsets[i]] ^= 0x01;
        g_assert_false (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                          &plaintext_len, &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_WHITEBOX);
        g_clear_error (&error);
        g_assert_null (plaintext);
        hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));
    }
}

static void
test_corrupt_padding_rejected (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    GError *error = NULL;

    hex_decode (CORRUPT_PADDING_HEX, blob, sizeof (blob));
    g_assert_false (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                      &plaintext_len, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_WHITEBOX);
    g_clear_error (&error);
    g_assert_null (plaintext);
}

static void
test_noncanonical_iv_rejected (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    GError *error = NULL;

    hex_decode (NONCANONICAL_IV_HEX, blob, sizeof (blob));
    g_assert_false (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                      &plaintext_len, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_WHITEBOX);
    g_clear_error (&error);
    g_assert_null (plaintext);
}

static void
test_openssl_error_queue_left_clean (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 *plaintext = NULL;
    gsize plaintext_len = 0;
    GError *error = NULL;

    hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));

    /* A failed decrypt must not leave a stale OpenSSL error queue. */
    blob[sizeof (blob) - 1] ^= 0xff;
    g_assert_false (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                      &plaintext_len, &error));
    g_clear_error (&error);
    g_assert_cmpint (ERR_peek_error (), ==, 0);

    /* Neither may a successful one. */
    hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));
    g_assert_true (gm168_wb_decrypt (blob, sizeof (blob), &plaintext,
                                     &plaintext_len, &error));
    g_assert_no_error (error);
    g_assert_cmpint (ERR_peek_error (), ==, 0);

    OPENSSL_cleanse (plaintext, plaintext_len);
    g_free (plaintext);
}

// ─── DPAPI v1 parser ────────────────────────────────────────────────────────

static void
test_dpapi_totals_match_builder (void)
{
    GByteArray *blob = build_default_dpapi ();
    guint32 total = 0;
    GError *error = NULL;

    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_OK);
    g_assert_no_error (error);
    g_assert_cmpint (total, ==, blob->len);
    g_byte_array_free (blob, TRUE);

    blob = build_reference_324_blob ();
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_OK);
    g_assert_no_error (error);
    g_assert_cmpint (total, ==, 324);
    g_byte_array_free (blob, TRUE);

    /* Independent arithmetic: 100 (empty blob incl. 16-byte salt) + 7 prompt
     * + 32 signSalt + 96 data + 64 signature == 299. */
    {
        guint8 salt[16], prompt[7], sign_salt[32], data[96], sig[64];
        fill_range (salt, sizeof (salt), 0);
        fill_range (prompt, sizeof (prompt), 0);
        fill_range (sign_salt, sizeof (sign_salt), 0);
        fill_range (data, sizeof (data), 0);
        fill_range (sig, sizeof (sig), 0);
        blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), prompt,
                            sizeof (prompt), sign_salt, sizeof (sign_salt),
                            data, sizeof (data), sig, sizeof (sig), 16, 20);
        g_assert_cmpint (blob->len, ==, 299);
        g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                       &total, &error) ==
                       GM168_DPAPI_OK);
        g_assert_no_error (error);
        g_assert_cmpint (total, ==, 299);
        g_byte_array_free (blob, TRUE);
    }
}

static void
test_dpapi_progressive_prefixes_need_more (void)
{
    GByteArray *blob = build_default_dpapi ();
    guint32 total = 0;
    GError *error = NULL;

    for (gsize cut = 0; cut < blob->len; cut++) {
        /* Use an exact allocation so ASan can also catch pointer arithmetic or
         * reads beyond a short prefix, not merely logical length mistakes. */
        guint8 *prefix = cut > 0 ? g_malloc (cut) : NULL;

        if (cut > 0)
            memcpy (prefix, blob->data, cut);
        error = NULL;
        g_assert_true (gm168_dpapi_parse_total_length (prefix, cut, &total,
                                                       &error) ==
                       GM168_DPAPI_NEED_MORE);
        g_assert_no_error (error);
        g_free (prefix);
    }
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_OK);
    g_assert_no_error (error);
    g_assert_cmpint (total, ==, blob->len);
    g_byte_array_free (blob, TRUE);
}

static void
test_dpapi_rejects_non_v1 (void)
{
    GByteArray *blob = build_default_dpapi ();
    guint32 total = 0;
    GError *error = NULL;

    patch_le32 (blob, 0, 2);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);
}

static void
test_dpapi_algo_lens_are_bit_lengths (void)
{
    guint8 salt[16];
    GByteArray *blob;
    guint32 total = 0;
    GError *error = NULL;

    fill_range (salt, sizeof (salt), 0);

    /* dwCryptAlgoLen/dwSignAlgoLen are bit lengths: parsed as u32 values,
     * they never advance the byte offset and are not byte-bounded. */
    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        NULL, 0, NULL, 0, 0xffffffffu, 0xffffffffu);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_OK);
    g_assert_no_error (error);
    g_assert_cmpint (total, ==, blob->len);
    g_byte_array_free (blob, TRUE);
}

static void
test_dpapi_rejects_impossible_lengths (void)
{
    guint8 salt[16];
    guint8 *big = g_malloc (5000);
    guint32 total = 0;
    GError *error = NULL;
    GByteArray *blob;

    memset (big, 0xa5, 5000);
    fill_range (salt, sizeof (salt), 0);

    /* Declared description longer than the whole allowed DPAPI area. */
    blob = build_dpapi (1, big, 5000, salt, sizeof (salt), NULL, 0, NULL, 0,
                        NULL, 0, NULL, 0, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    /* Impossible salt size (4000 bytes of actual payload follow). */
    blob = build_dpapi (1, NULL, 0, big, 4000, NULL, 0, NULL, 0,
                        NULL, 0, NULL, 0, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    /* Impossible prompt size. */
    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), big, 4000, NULL, 0,
                        NULL, 0, NULL, 0, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    /* signSalt / data / signature at 3993 bytes: above MAX_AREA-104. */
    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, big, 3993,
                        NULL, 0, NULL, 0, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        big, 3993, NULL, 0, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        NULL, 0, big, 3993, 16, 20);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    g_free (big);
}

static void
test_dpapi_cumulative_bounds_fire_before_buffer_end (void)
{
    guint32 total = 0;
    GError *error = NULL;
    GByteArray *blob;

    /* A declared description whose cumulative end exceeds the maximum is
     * rejected from the header alone, without any description bytes. */
    blob = build_default_dpapi ();
    patch_le32 (blob, 44, 3960);
    g_byte_array_set_size (blob, 48);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    /* description 2500 (legal) followed by a declared 2000-byte salt: the
     * prefix ending just before dwSaltLen still asks for more data, the
     * prefix including dwSaltLen is rejected immediately. */
    {
        guint8 *desc = g_malloc (2500);
        guint8 *salt = g_malloc (2000);
        gsize before_salt_len;

        memset (desc, 0x5a, 2500);
        memset (salt, 0xc3, 2000);
        blob = build_dpapi (1, desc, 2500, salt, 2000, NULL, 0, NULL, 0,
                            NULL, 0, NULL, 0, 16, 20);
        before_salt_len = 48 + 2500 + 8;   // through dwCryptAlgoLen
        g_assert_cmpint (before_salt_len, <, blob->len);

        error = NULL;
        g_assert_true (gm168_dpapi_parse_total_length (blob->data,
                                                       before_salt_len,
                                                       &total, &error) ==
                       GM168_DPAPI_NEED_MORE);
        g_assert_no_error (error);

        error = NULL;
        g_assert_true (gm168_dpapi_parse_total_length (blob->data,
                                                       before_salt_len + 4,
                                                       &total, &error) ==
                       GM168_DPAPI_INVALID);
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_DPAPI);
        g_clear_error (&error);

        g_free (desc);
        g_free (salt);
        g_byte_array_free (blob, TRUE);
    }
}

static void
test_dpapi_maximum_total_accepted_and_overflow_rejected (void)
{
    guint8 *data = g_malloc (3852);
    guint8 salt[16], sig[200];
    GByteArray *blob;
    guint32 total = 0;
    GError *error = NULL;

    memset (data, 0x11, 3852);
    fill_range (salt, sizeof (salt), 0);
    fill_range (sig, sizeof (sig), 0);

    /* Empty blob total is 100 (16-byte salt included); + 3852 data bytes
     * + 40 signature bytes == 3992 == GM168_PSK_MAX_DPAPI. */
    blob = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        data, 3852, sig, 40, 16, 20);
    g_assert_cmpint (blob->len, ==, GM168_PSK_MAX_DPAPI);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_OK);
    g_assert_no_error (error);
    g_assert_cmpint (total, ==, GM168_PSK_MAX_DPAPI);
    g_byte_array_free (blob, TRUE);

    /* A description may end exactly at the DPAPI limit, but there is no room
     * for the mandatory following dwCryptAlgo field. Reject this immediately
     * instead of reporting NEED_MORE at offset 3992. */
    blob = build_default_dpapi ();
    patch_le32 (blob, 44, GM168_PSK_MAX_DPAPI - 48);
    g_byte_array_set_size (blob, GM168_PSK_MAX_DPAPI);
    g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                   &total, &error) ==
                   GM168_DPAPI_INVALID);
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    g_byte_array_free (blob, TRUE);

    /* Every field individually legal, but the cumulative total is not. */
    {
        guint8 *desc = g_malloc (3000);
        guint8 *long_salt = g_malloc (500);
        guint8 data2[400];

        memset (desc, 0x22, 3000);
        memset (long_salt, 0x33, 500);
        memset (data2, 0x44, sizeof (data2));
        blob = build_dpapi (1, desc, 3000, long_salt, 500, NULL, 0, NULL, 0,
                            data2, sizeof (data2), sig, 200, 16, 20);
        g_assert_true (gm168_dpapi_parse_total_length (blob->data, blob->len,
                                                       &total, &error) ==
                       GM168_DPAPI_INVALID);
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_DPAPI);
        g_clear_error (&error);
        g_free (desc);
        g_free (long_salt);
        g_byte_array_free (blob, TRUE);
    }

    g_free (data);
}

// ─── Whitebox TLV record parser ─────────────────────────────────────────────

static void
test_record_parser_round_trip (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 out[GM168_PSK_WHITEBOX_LEN];
    GByteArray *record;
    GError *error = NULL;

    hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));
    record = build_record (blob, sizeof (blob), GM168_PSK_RECORD_TLV_TAG,
                           GM168_PSK_WHITEBOX_LEN);
    g_assert_true (gm168_psk_parse_whitebox_record (record->data, record->len,
                                                    out, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (out, sizeof (out), blob, sizeof (blob));
    g_byte_array_free (record, TRUE);
}

static void
test_record_parser_rejects_bad_layout (void)
{
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    GByteArray *record;
    GError *error = NULL;

    hex_decode (kat_cases[6].blob_hex, blob, sizeof (blob));

    struct {
        guint32 tag;
        guint32 declared;
        gsize blob_len;
        gsize trailing;
    } bad[] = {
        { 0xBB010004u, 96, 96, 0 },   // wrong tag
        { 0xBB010003u, 95, 96, 0 },   // declared too small
        { 0xBB010003u, 97, 96, 0 },   // declared too large
        { 0xBB010003u, 95, 95, 0 },   // short blob
        { 0xBB010003u, 96, 96, 1 },   // trailing byte
        { 0xBB010003u, 96, 0, 0 },    // empty
    };

    for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
        guint8 out[GM168_PSK_WHITEBOX_LEN];
        guint8 scratch[GM168_PSK_WHITEBOX_LEN + 1] = { 0 };

        record = build_record (bad[i].blob_len > 0 ? blob : scratch,
                               bad[i].blob_len, bad[i].tag,
                               bad[i].declared);
        if (bad[i].trailing > 0)
            g_byte_array_append (record, scratch, bad[i].trailing);

        error = NULL;
        g_assert_false (gm168_psk_parse_whitebox_record (record->data,
                                                         record->len, out,
                                                         &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_RECORD);
        g_clear_error (&error);
        g_byte_array_free (record, TRUE);
    }
}

// ─── Full recovery ──────────────────────────────────────────────────────────

static void
decode_hex_to (const gchar *hex, guint8 *out, gsize len)
{
    hex_decode (hex, out, len);
}

static void
test_recovery_reference_324 (void)
{
    guint8 probe[GM168_PSK_LEN];
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    guint32 record_offset = 0;
    GByteArray *area;
    GError *error = NULL;

    fill_probe_psk (probe);
    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));

    area = build_reference_324_blob ();
    append_record (area, blob);

    memset (psk, 0xaa, sizeof (psk));
    g_assert_true (gm168_psk_recover_from_area (area->data, area->len,
                                                expected_hash, psk,
                                                &record_offset, &error));
    g_assert_no_error (error);
    g_assert_cmpint (record_offset, ==, 324);
    g_assert_cmpmem (psk, sizeof (psk), probe, sizeof (probe));

    /* record_offset_out is optional. */
    memset (psk, 0xaa, sizeof (psk));
    g_assert_true (gm168_psk_recover_from_area (area->data, area->len,
                                                expected_hash, psk, NULL,
                                                &error));
    g_assert_no_error (error);
    g_assert_cmpmem (psk, sizeof (psk), probe, sizeof (probe));

    g_byte_array_free (area, TRUE);
}

static void
test_recovery_max_area (void)
{
    guint8 probe[GM168_PSK_LEN];
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    guint8 sig[40];
    guint8 salt[16];
    guint32 record_offset = 0;
    guint8 *data = g_malloc (3852);
    GByteArray *area;
    GError *error = NULL;

    fill_probe_psk (probe);
    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));
    memset (data, 0x11, 3852);
    fill_range (sig, sizeof (sig), 0);
    fill_range (salt, sizeof (salt), 0);

    /* DPAPI 3992 (incl. the 16-byte salt) + record 104 == 4096 exactly. */
    area = build_dpapi (1, NULL, 0, salt, sizeof (salt), NULL, 0, NULL, 0,
                        data, 3852, sig, sizeof (sig), 16, 20);
    append_record (area, blob);
    g_assert_cmpint (area->len, ==, GM168_PSK_MAX_AREA);

    g_assert_true (gm168_psk_recover_from_area (area->data, area->len,
                                                expected_hash, psk,
                                                &record_offset, &error));
    g_assert_no_error (error);
    g_assert_cmpint (record_offset, ==, GM168_PSK_MAX_DPAPI);
    g_assert_cmpmem (psk, sizeof (psk), probe, sizeof (probe));

    g_free (data);
    g_byte_array_free (area, TRUE);
}

static void
test_recovery_rejects_hash_mismatch_and_zeroizes (void)
{
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    guint32 record_offset = 0xdeadbeef;
    GByteArray *area;
    GError *error = NULL;

    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));

    area = build_reference_324_blob ();
    append_record (area, blob);

    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    expected_hash[GM168_PSK_HASH_LEN - 1] ^= 0xff;

    memset (psk, 0xaa, sizeof (psk));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk,
                                                 &record_offset, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);

    /* The out buffer stays fully zeroized and the offset is reset. */
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);
    g_assert_cmpint (record_offset, ==, 0);

    g_byte_array_free (area, TRUE);
}

static void
test_recovery_rejects_non_psk_plaintext (void)
{
    guint8 plaintext[33];
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 psk[GM168_PSK_LEN];
    guint8 *encrypted = NULL;
    gsize encrypted_len = 0;
    gchar *hash_hex;
    GByteArray *area;
    GError *error = NULL;

    /* 33 plaintext bytes still produce a structurally valid 96-byte whitebox
     * blob, but recovery must accept exactly one 32-byte PSK, not merely a
     * valid canonical decryption. */
    fill_range (plaintext, sizeof (plaintext), 9);
    g_assert_true (gm168_wb_encrypt (plaintext, sizeof (plaintext),
                                     &encrypted, &encrypted_len, &error));
    g_assert_no_error (error);
    g_assert_cmpint (encrypted_len, ==, GM168_PSK_WHITEBOX_LEN);

    hash_hex = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                            plaintext, sizeof (plaintext));
    decode_hex_to (hash_hex, expected_hash, sizeof (expected_hash));
    g_free (hash_hex);

    area = build_reference_324_blob ();
    append_record (area, encrypted);
    memset (psk, 0xaa, sizeof (psk));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);

    OPENSSL_cleanse (plaintext, sizeof (plaintext));
    OPENSSL_cleanse (encrypted, encrypted_len);
    g_free (encrypted);
    g_byte_array_free (area, TRUE);
}

static void
test_recovery_rejects_zero_sentinels (void)
{
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    GByteArray *area;
    GError *error = NULL;

    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));

    area = build_reference_324_blob ();
    append_record (area, blob);

    /* All-zero expected digest: malformed device state. The standalone gate
     * is what the USB integration calls before issuing any area request. */
    memset (expected_hash, 0, sizeof (expected_hash));
    g_assert_false (gm168_psk_validate_expected_hash (expected_hash, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    memset (psk, 0xaa, sizeof (psk));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);

    /* SHA256(all-zero PSK): the sensor holds no provisioned PSK. */
    decode_hex_to (ZERO_PSK_HASH_HEX, expected_hash, sizeof (expected_hash));
    g_assert_false (gm168_psk_validate_expected_hash (expected_hash, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    memset (psk, 0xaa, sizeof (psk));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);

    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    g_assert_true (gm168_psk_validate_expected_hash (expected_hash, &error));
    g_assert_no_error (error);

    g_byte_array_free (area, TRUE);
}

static void
test_recovery_rejects_public_zero_record (void)
{
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 psk[GM168_PSK_LEN];
    GByteArray *area;
    GError *error = NULL;

    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));

    /* A stored blob equal to the public zero-PSK sentinel means the device
     * never received a real PSK; the record gate must stop before decrypt. */
    area = build_reference_324_blob ();
    append_record (area, gm168_public_zero_psk_whitebox);

    memset (psk, 0xaa, sizeof (psk));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_clear_error (&error);
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);

    g_byte_array_free (area, TRUE);
}

static void
test_recovery_rejects_truncated_area (void)
{
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    GByteArray *area;
    GError *error = NULL;

    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));

    area = build_reference_324_blob ();
    append_record (area, blob);

    /* Every strict prefix of the area is incomplete and clears the output. */
    memset (psk, 0xaa, sizeof (psk));
    for (gsize cut = 0; cut < area->len; cut += 17) {
        error = NULL;
        g_assert_false (gm168_psk_recover_from_area (area->data, cut,
                                                     expected_hash, psk, NULL,
                                                     &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_DPAPI);
        g_clear_error (&error);
        for (gsize i = 0; i < sizeof (psk); i++)
            g_assert_cmpint (psk[i], ==, 0);
        memset (psk, 0xaa, sizeof (psk));
    }

    /* One byte short of the record is still incomplete. */
    error = NULL;
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len - 1,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_DPAPI);
    g_clear_error (&error);
    for (gsize i = 0; i < sizeof (psk); i++)
        g_assert_cmpint (psk[i], ==, 0);

    g_byte_array_free (area, TRUE);
}

static void
test_recovery_failure_leaves_no_openssl_errors (void)
{
    guint8 expected_hash[GM168_PSK_HASH_LEN];
    guint8 blob[GM168_PSK_WHITEBOX_LEN];
    guint8 psk[GM168_PSK_LEN];
    GByteArray *area;
    GError *error = NULL;

    hex_decode (PROBE_BLOB_HEX, blob, sizeof (blob));

    area = build_reference_324_blob ();
    blob[0] ^= 0xff;   // valid record, undecryptable blob
    append_record (area, blob);

    decode_hex_to (PROBE_HASH_HEX, expected_hash, sizeof (expected_hash));
    g_assert_false (gm168_psk_recover_from_area (area->data, area->len,
                                                 expected_hash, psk, NULL,
                                                 &error));
    g_clear_error (&error);
    g_assert_cmpint (ERR_peek_error (), ==, 0);

    g_byte_array_free (area, TRUE);
}

// ─── E4 reply validators ────────────────────────────────────────────────────

static GByteArray *
build_hash_extra (const guint8 hash[GM168_PSK_HASH_LEN], guint32 tag,
                  guint32 declared, gsize data_len)
{
    GByteArray *out = g_byte_array_new ();
    put_le32 (out, tag);
    put_le32 (out, declared);
    g_byte_array_append (out, hash, data_len);
    return out;
}

static void
test_hash_reply_validator_ok (void)
{
    guint8 hash[GM168_PSK_HASH_LEN];
    guint8 out[GM168_PSK_HASH_LEN];
    GByteArray *extra;
    GError *error = NULL;

    fill_probe_psk (hash);
    extra = build_hash_extra (hash, GM168_PSK_HASH_TLV_TAG,
                              GM168_PSK_HASH_LEN, GM168_PSK_HASH_LEN);
    g_assert_true (gm168_psk_validate_hash_reply (extra->data, extra->len,
                                                  out, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (out, sizeof (out), hash, sizeof (hash));
    g_byte_array_free (extra, TRUE);
}

static void
test_hash_reply_validator_rejects (void)
{
    guint8 hash[GM168_PSK_HASH_LEN + 1] = { 0 };
    GError *error = NULL;

    fill_probe_psk (hash);

    /* A successful A0 decode with no payload yields NULL/0. It must produce a
     * normal protocol error, never a critical or an unset GError. */
    {
        guint8 out[GM168_PSK_HASH_LEN];

        g_assert_false (gm168_psk_validate_hash_reply (NULL, 0, out, &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_VERIFY);
        g_clear_error (&error);
    }

    struct {
        guint32 tag;
        guint32 declared;
        gsize data_len;
    } bad[] = {
        { GM168_PSK_HASH_TLV_TAG, GM168_PSK_HASH_LEN, 31 },  // short extra
        { GM168_PSK_HASH_TLV_TAG, GM168_PSK_HASH_LEN, 33 },  // long extra
        { 0xbb020002u, GM168_PSK_HASH_LEN, 32 },             // wrong tag
        { GM168_PSK_HASH_TLV_TAG, 31, 32 },                  // declared small
        { GM168_PSK_HASH_TLV_TAG, 33, 32 },                  // declared large
    };

    for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
        guint8 out[GM168_PSK_HASH_LEN];
        GByteArray *extra = build_hash_extra (hash, bad[i].tag,
                                              bad[i].declared,
                                              bad[i].data_len);

        error = NULL;
        g_assert_false (gm168_psk_validate_hash_reply (extra->data,
                                                       extra->len, out,
                                                       &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_VERIFY);
        g_clear_error (&error);
        g_byte_array_free (extra, TRUE);
    }
}

static GByteArray *
build_area_extra (const guint8 *chunk, gsize chunk_len, guint32 tag,
                  guint32 declared)
{
    GByteArray *out = g_byte_array_new ();
    put_le32 (out, tag);
    put_le32 (out, declared);
    g_byte_array_append (out, chunk, chunk_len);
    return out;
}

static void
test_area_reply_validator_ok (void)
{
    guint8 chunk[100];
    guint8 out[GM168_PSK_AREA_CHUNK];
    guint32 chunk_len = 0;
    GByteArray *extra;
    GError *error = NULL;

    fill_range (chunk, sizeof (chunk), 7);

    extra = build_area_extra (chunk, sizeof (chunk), GM168_PSK_AREA_TLV_TAG,
                              sizeof (chunk));
    g_assert_true (gm168_psk_validate_area_reply (extra->data, extra->len,
                                                  256, 0, out, &chunk_len,
                                                  &error));
    g_assert_no_error (error);
    g_assert_cmpint (chunk_len, ==, sizeof (chunk));
    g_assert_cmpmem (out, chunk_len, chunk, sizeof (chunk));
    g_byte_array_free (extra, TRUE);

    /* A full 256-byte chunk at the very end of the bounded area. */
    {
        guint8 full[GM168_PSK_AREA_CHUNK];

        memset (full, 0x42, sizeof (full));
        extra = build_area_extra (full, sizeof (full), GM168_PSK_AREA_TLV_TAG,
                                  sizeof (full));
        g_assert_true (gm168_psk_validate_area_reply (extra->data,
                                                      extra->len, 256,
                                                      GM168_PSK_MAX_AREA - 256,
                                                      out, &chunk_len,
                                                      &error));
        g_assert_no_error (error);
        g_assert_cmpint (chunk_len, ==, 256);
        g_byte_array_free (extra, TRUE);
    }
}

static void
test_area_reply_validator_rejects (void)
{
    guint8 chunk[256];
    guint8 out[GM168_PSK_AREA_CHUNK];
    guint32 chunk_len = 0;
    GError *error = NULL;

    fill_range (chunk, sizeof (chunk), 3);

    chunk_len = 99;
    g_assert_false (gm168_psk_validate_area_reply (NULL, 0, 256, 0,
                                                   out, &chunk_len, &error));
    g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                    GM168_PSK_RECOVERY_ERROR_VERIFY);
    g_assert_cmpint (chunk_len, ==, 0);
    g_clear_error (&error);

    struct {
        guint32 tag;
        guint32 declared;
        gsize data_len;
        guint32 requested;
        guint32 offset;
    } bad[] = {
        { GM168_PSK_AREA_TLV_TAG, 0, 0, 256, 0 },          // no progress
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 100, 0 },      // chunk > requested
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 0, 0 },        // requested 0
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 257, 0 },      // requested > chunk max
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 256, 4096 },   // offset at area end
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 256, 3996 },   // offset+chunk > 4096
        { GM168_PSK_AREA_TLV_TAG, 101, 101, 256, 0xffffffffu }, // overflow offset
        { 0xbb010003u, 101, 101, 256, 0 },                 // wrong tag
        { GM168_PSK_AREA_TLV_TAG, 100, 101, 256, 0 },      // declared mismatch
    };

    for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
        GByteArray *extra = build_area_extra (chunk, bad[i].data_len,
                                              bad[i].tag, bad[i].declared);

        error = NULL;
        g_assert_false (gm168_psk_validate_area_reply (extra->data,
                                                       extra->len,
                                                       bad[i].requested,
                                                       bad[i].offset,
                                                       out, &chunk_len,
                                                       &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_VERIFY);
        g_clear_error (&error);
        g_assert_cmpint (chunk_len, ==, 0);
        g_byte_array_free (extra, TRUE);
    }

    /* extra shorter than the 8-byte TLV header. */
    {
        GByteArray *extra = g_byte_array_new ();
        put_le32 (extra, GM168_PSK_AREA_TLV_TAG);
        put_le32 (extra, 0);
        g_byte_array_set_size (extra, 7);

        error = NULL;
        g_assert_false (gm168_psk_validate_area_reply (extra->data,
                                                       extra->len, 256, 0,
                                                       out, &chunk_len,
                                                       &error));
        g_assert_error (error, GM168_PSK_RECOVERY_ERROR,
                        GM168_PSK_RECOVERY_ERROR_VERIFY);
        g_clear_error (&error);
        g_byte_array_free (extra, TRUE);
    }
}

// ─── Strict A0 ACK framing used by the E4 recovery path ─────────────────────

static GByteArray *
build_a0_ack (guint8 echo, guint8 status, const guint8 *extra,
              gsize extra_len)
{
    GByteArray *frame = g_byte_array_new ();
    gsize payload_len = 1 + extra_len;  /* status + extra */
    guint16 plen = (guint16) (payload_len + 1);
    guint16 inner_len = (guint16) (3 + plen);
    guint32 sum;
    guint8 body_sum;

    g_byte_array_set_size (frame, 4 + inner_len);
    frame->data[0] = GOODIX_GM168_PKT_CMD;
    frame->data[1] = (guint8) inner_len;
    frame->data[2] = (guint8) (inner_len >> 8);
    frame->data[3] = (guint8) (frame->data[0] + frame->data[1] +
                               frame->data[2]);
    frame->data[4] = echo;
    frame->data[5] = (guint8) plen;
    frame->data[6] = (guint8) (plen >> 8);
    frame->data[7] = status;
    if (extra_len > 0)
        memcpy (frame->data + 8, extra, extra_len);

    sum = echo + frame->data[5] + frame->data[6] + status;
    for (gsize i = 0; i < extra_len; i++)
        sum += extra[i];
    body_sum = (guint8) (0xaa - sum);
    frame->data[7 + payload_len] = body_sum;
    return frame;
}

static void
test_strict_a0_ack_decoder (void)
{
    static const guint8 touch_packet[GOODIX_GM168_TOUCH_PKT_LEN] = {
        0xa0, 0x14, 0x00, 0xb4, 0x32, 0x11, 0x00, 0x02,
        0x00, 0x3f, 0x00, 0xb9, 0x00, 0xc0, 0x00, 0x91,
        0x00, 0x9d, 0x00, 0x86, 0x00, 0xb7, 0x00, 0x42,
    };
    guint8 extra_data[GM168_PSK_HASH_LEN + 8] = { 0 };
    guint8 echo = 0, status = 0;
    guint8 *extra = NULL;
    guint16 extra_len = 0;
    GByteArray *frame;

    fill_range (extra_data, sizeof (extra_data), 4);

    /* Minimal 9-byte ACK has status but no extra payload. */
    frame = build_a0_ack (GOODIX_GM168_CMD_SESSION_INIT,
                          GOODIX_GM168_STATUS_OK, NULL, 0);
    g_assert_cmpint (frame->len, ==, 9);
    g_assert_true (goodix_gm168_decode_ack (frame->data, frame->len,
                                            &echo, &status, &extra, &extra_len));
    g_assert_cmpint (echo, ==, GOODIX_GM168_CMD_SESSION_INIT);
    g_assert_cmpint (status, ==, GOODIX_GM168_STATUS_OK);
    g_assert_null (extra);
    g_assert_cmpint (extra_len, ==, 0);
    g_byte_array_free (frame, TRUE);

    /* E4 response retains the exact extra slice. */
    frame = build_a0_ack (GOODIX_GM168_CMD_SPEC_DATA, GOODIX_GM168_STATUS_OK,
                          extra_data, sizeof (extra_data));
    g_assert_true (goodix_gm168_decode_ack (frame->data, frame->len,
                                            &echo, &status, &extra, &extra_len));
    g_assert_cmpint (echo, ==, GOODIX_GM168_CMD_SPEC_DATA);
    g_assert_cmpint (extra_len, ==, sizeof (extra_data));
    g_assert_cmpmem (extra, extra_len, extra_data, sizeof (extra_data));
    g_byte_array_free (frame, TRUE);

    /* Public protocol example: normal 24-byte FDT touch packet remains valid. */
    g_assert_true (goodix_gm168_decode_ack (
        touch_packet, sizeof (touch_packet), &echo, &status, &extra, &extra_len));
    g_assert_cmpint (echo, ==, GOODIX_GM168_CMD_FDT_SETUP);
    g_assert_cmpint (status, ==, GOODIX_GM168_STATUS_TOUCH);
    g_assert_cmpint (extra_len, ==, 15);

    frame = build_a0_ack (GOODIX_GM168_CMD_SPEC_DATA, GOODIX_GM168_STATUS_OK,
                          extra_data, sizeof (extra_data));
    frame->data[3] ^= 0x01;
    g_assert_false (goodix_gm168_decode_ack (frame->data, frame->len,
                                             &echo, &status, &extra, &extra_len));
    frame->data[3] ^= 0x01;  /* restore header, then corrupt body checksum */
    frame->data[frame->len - 1] ^= 0x01;
    g_assert_false (goodix_gm168_decode_ack (frame->data, frame->len,
                                             &echo, &status, &extra, &extra_len));
    frame->data[frame->len - 1] ^= 0x01;
    frame->data[5] ^= 0x01;  /* PLen must remain inner_len - 3 */
    g_assert_false (goodix_gm168_decode_ack (frame->data, frame->len,
                                             &echo, &status, &extra, &extra_len));
    g_byte_array_free (frame, TRUE);

    frame = build_a0_ack (GOODIX_GM168_CMD_SPEC_DATA, GOODIX_GM168_STATUS_OK,
                          extra_data, sizeof (extra_data));
    g_byte_array_append (frame, (const guint8 *) "", 1);  /* trailing byte */
    g_assert_false (goodix_gm168_decode_ack (frame->data, frame->len,
                                             &echo, &status, &extra, &extra_len));
    g_byte_array_free (frame, TRUE);
}

// ─── main ───────────────────────────────────────────────────────────────────

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/psk_recovery/root_secret_fingerprint",
                     test_root_secret_fingerprint);
    g_test_add_func ("/psk_recovery/public_zero_vector",
                     test_public_zero_vector);
    g_test_add_func ("/psk_recovery/kat_decrypt_roundtrip",
                     test_kat_decrypt_roundtrip);
    g_test_add_func ("/psk_recovery/encrypt_decrypt_roundtrip",
                     test_encrypt_decrypt_roundtrip);
    g_test_add_func ("/psk_recovery/malformed_blob_lengths_rejected",
                     test_malformed_blob_lengths_rejected);
    g_test_add_func ("/psk_recovery/hmac_mismatch_rejected",
                     test_hmac_mismatch_rejected);
    g_test_add_func ("/psk_recovery/corrupt_padding_rejected",
                     test_corrupt_padding_rejected);
    g_test_add_func ("/psk_recovery/noncanonical_iv_rejected",
                     test_noncanonical_iv_rejected);
    g_test_add_func ("/psk_recovery/openssl_error_queue_left_clean",
                     test_openssl_error_queue_left_clean);

    g_test_add_func ("/psk_recovery/dpapi_totals_match_builder",
                     test_dpapi_totals_match_builder);
    g_test_add_func ("/psk_recovery/dpapi_progressive_prefixes_need_more",
                     test_dpapi_progressive_prefixes_need_more);
    g_test_add_func ("/psk_recovery/dpapi_rejects_non_v1",
                     test_dpapi_rejects_non_v1);
    g_test_add_func ("/psk_recovery/dpapi_algo_lens_are_bit_lengths",
                     test_dpapi_algo_lens_are_bit_lengths);
    g_test_add_func ("/psk_recovery/dpapi_rejects_impossible_lengths",
                     test_dpapi_rejects_impossible_lengths);
    g_test_add_func ("/psk_recovery/dpapi_cumulative_bounds_fire_early",
                     test_dpapi_cumulative_bounds_fire_before_buffer_end);
    g_test_add_func ("/psk_recovery/dpapi_maximum_total_and_overflow",
                     test_dpapi_maximum_total_accepted_and_overflow_rejected);

    g_test_add_func ("/psk_recovery/record_parser_round_trip",
                     test_record_parser_round_trip);
    g_test_add_func ("/psk_recovery/record_parser_rejects_bad_layout",
                     test_record_parser_rejects_bad_layout);

    g_test_add_func ("/psk_recovery/recovery_reference_324",
                     test_recovery_reference_324);
    g_test_add_func ("/psk_recovery/recovery_max_area",
                     test_recovery_max_area);
    g_test_add_func ("/psk_recovery/recovery_rejects_hash_mismatch",
                     test_recovery_rejects_hash_mismatch_and_zeroizes);
    g_test_add_func ("/psk_recovery/recovery_rejects_non_psk_plaintext",
                     test_recovery_rejects_non_psk_plaintext);
    g_test_add_func ("/psk_recovery/recovery_rejects_zero_sentinels",
                     test_recovery_rejects_zero_sentinels);
    g_test_add_func ("/psk_recovery/recovery_rejects_public_zero_record",
                     test_recovery_rejects_public_zero_record);
    g_test_add_func ("/psk_recovery/recovery_rejects_truncated_area",
                     test_recovery_rejects_truncated_area);
    g_test_add_func ("/psk_recovery/recovery_no_stale_openssl_errors",
                     test_recovery_failure_leaves_no_openssl_errors);

    g_test_add_func ("/psk_recovery/hash_reply_validator_ok",
                     test_hash_reply_validator_ok);
    g_test_add_func ("/psk_recovery/hash_reply_validator_rejects",
                     test_hash_reply_validator_rejects);
    g_test_add_func ("/psk_recovery/area_reply_validator_ok",
                     test_area_reply_validator_ok);
    g_test_add_func ("/psk_recovery/area_reply_validator_rejects",
                     test_area_reply_validator_rejects);
    g_test_add_func ("/psk_recovery/strict_a0_ack_decoder",
                     test_strict_a0_ack_decoder);

    return g_test_run ();
}
