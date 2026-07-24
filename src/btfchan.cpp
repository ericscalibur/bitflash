// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Bitflash end-to-end encrypted channel. Ported from Itzal echan.rs.
// Uses libsodium crypto_box_curve25519xchacha20poly1305 (X25519 + XChaCha20-Poly1305),
// the same primitive as Rust's crypto_box ChaChaBox.

#include "btfchan.h"
#include <sodium.h>
#include <cstring>

namespace btf
{

// Compile-time sanity: our constants match libsodium's.
static_assert(CHAN_NONCEBYTES == crypto_box_curve25519xchacha20poly1305_NONCEBYTES, "nonce");
static_assert(CHAN_MACBYTES   == crypto_box_curve25519xchacha20poly1305_MACBYTES,   "mac");
static_assert(CHAN_KEYBYTES   == crypto_box_curve25519xchacha20poly1305_BEFORENMBYTES, "key");

bool ChanInit()
{
    return sodium_init() >= 0;
}

void ChanKeypair(unsigned char pk[32], unsigned char sk[32])
{
    crypto_box_curve25519xchacha20poly1305_keypair(pk, sk);
}

void ChanPublicFromSecret(unsigned char pk[32], const unsigned char sk[32])
{
    // X25519 public = scalarmult of the secret with the curve25519 base point.
    crypto_scalarmult_base(pk, sk);
}

bool ChanClientHandshake(const unsigned char endpoint_static_pub[32],
                         unsigned char eph_pub_out[32], unsigned char shared_out[32])
{
    unsigned char eph_sk[32];
    crypto_box_curve25519xchacha20poly1305_keypair(eph_pub_out, eph_sk);
    // shared = X25519(eph_sk, endpoint_static_pub) -> hashed to a box key
    int r = crypto_box_curve25519xchacha20poly1305_beforenm(shared_out, endpoint_static_pub, eph_sk);
    sodium_memzero(eph_sk, sizeof(eph_sk));
    return r == 0;
}

bool ChanEndpointHandshake(const unsigned char client_eph_pub[32],
                           const unsigned char my_static_sk[32], unsigned char shared_out[32])
{
    // shared = X25519(my_static_sk, client_eph_pub) -> same box key as the client computed
    return crypto_box_curve25519xchacha20poly1305_beforenm(shared_out, client_eph_pub, my_static_sk) == 0;
}

std::vector<unsigned char> ChanEncryptFrame(const unsigned char shared[32],
                                            const unsigned char* pt, size_t n)
{
    unsigned char nonce[CHAN_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    size_t ctlen = n + CHAN_MACBYTES;
    size_t bodylen = CHAN_NONCEBYTES + ctlen;

    std::vector<unsigned char> out(4 + bodylen);
    out[0] = (unsigned char)((bodylen >> 24) & 0xff);
    out[1] = (unsigned char)((bodylen >> 16) & 0xff);
    out[2] = (unsigned char)((bodylen >> 8) & 0xff);
    out[3] = (unsigned char)(bodylen & 0xff);
    memcpy(&out[4], nonce, CHAN_NONCEBYTES);

    crypto_box_curve25519xchacha20poly1305_easy_afternm(
        &out[4 + CHAN_NONCEBYTES], pt, n, nonce, shared);
    return out;
}

bool ChanDecryptBody(const unsigned char shared[32],
                     const unsigned char* body, size_t n, std::vector<unsigned char>& out)
{
    if (n < (size_t)(CHAN_NONCEBYTES + CHAN_MACBYTES))
        return false;
    const unsigned char* nonce = body;
    const unsigned char* ct = body + CHAN_NONCEBYTES;
    size_t ctlen = n - CHAN_NONCEBYTES;
    out.resize(ctlen - CHAN_MACBYTES);
    int r = crypto_box_curve25519xchacha20poly1305_open_easy_afternm(
        out.empty() ? NULL : &out[0], ct, ctlen, nonce, shared);
    if (r != 0)
    {
        out.clear();
        return false;
    }
    return true;
}

} // namespace btf
