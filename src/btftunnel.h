// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Transparent anonymous tunnel. Gives the existing CNode a plain local socket
// while, behind it, a pair of pump threads bridge plaintext <-> echan <->
// rendezvous. CNode reads/writes normally and never knows its bytes travel
// end-to-end encrypted through a meeting node (the Itzal `wrap()` pattern).
//
// Client side: resolve a `.btf` -> meeting node + service enc key (done by the
// caller), then BtfClientTunnel connects through the relay, runs the echan
// handshake, and returns a local SOCKET to hand a new outbound CNode.
//
// Service side: after RvServiceRegister pairs us with a client, BtfServiceWrap
// completes the echan handshake and returns a local SOCKET for an inbound CNode.

#ifndef BITFLASH_BTFTUNNEL_H
#define BITFLASH_BTFTUNNEL_H

#include "btfrv.h"

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET btf_socket_t;
#else
typedef int btf_socket_t;
#define INVALID_SOCKET (-1)
#endif

namespace btf
{

// Initialize (sockets + libsodium). Idempotent.
bool TunnelInit();

// Client: dial the service registered under `target_pubkey` at the meeting node
// (host:port), box to its static x25519 `service_enc_pub`, and return a local
// plaintext SOCKET tunneled end-to-end to the service. INVALID_SOCKET on error.
btf_socket_t BtfClientTunnel(const char* meeting_host, unsigned short port,
                             const unsigned char target_pubkey[32],
                             const unsigned char service_enc_pub[32]);

// Service: wrap an already-paired rendezvous socket (from RvServiceRegister)
// with echan using my static x25519 secret, returning a local plaintext SOCKET
// for an inbound CNode. Consumes `rv` (closed when the tunnel tears down).
btf_socket_t BtfServiceWrap(RvSocket rv, const unsigned char my_enc_sk[32]);

} // namespace btf

#endif
