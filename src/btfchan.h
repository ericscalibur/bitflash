// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// End-to-end encrypted channel for Bitflash (ported from the Itzal echan.rs).
// Wraps a raw pipe in an authenticated stream so relays/rendezvous nodes see
// only ciphertext and cannot MITM: only the endpoint holding the static secret
// can derive the key.
//
// Handshake: the client makes an ephemeral X25519 key and sends its public half
// to the endpoint. Both sides compute the same box (X25519 ECDH -> shared key
// for XChaCha20-Poly1305, i.e. libsodium crypto_box). Wire frame:
//   [len:4 BE][nonce:24][ciphertext || 16-byte tag]
// with a fresh random 24-byte nonce per frame (safe to pick at random).

#ifndef BITFLASH_BTFCHAN_H
#define BITFLASH_BTFCHAN_H

#include <vector>
#include <stdint.h>
#include <stddef.h>

namespace btf
{

static const int CHAN_KEYBYTES   = 32; // X25519 public/secret and shared key
static const int CHAN_NONCEBYTES = 24; // XChaCha20-Poly1305 nonce
static const int CHAN_MACBYTES   = 16; // Poly1305 tag

// Must be called once before any channel use (initializes libsodium). Idempotent.
bool ChanInit();

// Generate a static X25519 keypair (e.g. the endpoint's long-term key).
void ChanKeypair(unsigned char pk[32], unsigned char sk[32]);

// Derive the X25519 public key from a stored secret key (X25519 base mult).
void ChanPublicFromSecret(unsigned char pk[32], const unsigned char sk[32]);

// Client side: given the endpoint's static public key, produce an ephemeral
// public key (to hand the endpoint) and the shared key. Returns false on error.
bool ChanClientHandshake(const unsigned char endpoint_static_pub[32],
                         unsigned char eph_pub_out[32], unsigned char shared_out[32]);

// Endpoint side: rebuild the same shared key from the client's ephemeral public
// key and my static secret. Only the static-secret holder can do this.
bool ChanEndpointHandshake(const unsigned char client_eph_pub[32],
                           const unsigned char my_static_sk[32], unsigned char shared_out[32]);

// Encrypt a plaintext chunk into a full wire frame ([len][nonce][ct||tag]).
std::vector<unsigned char> ChanEncryptFrame(const unsigned char shared[32],
                                            const unsigned char* pt, size_t n);

// Decrypt a frame BODY (nonce||ciphertext||tag, i.e. without the 4-byte length
// prefix). Returns true and fills `out` on success; false on auth failure.
bool ChanDecryptBody(const unsigned char shared[32],
                     const unsigned char* body, size_t n, std::vector<unsigned char>& out);

} // namespace btf

#endif
