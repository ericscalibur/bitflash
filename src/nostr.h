// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Peer discovery over Nostr relays -- replaces the old ThreadIRCSeed. Each
// node publishes a replaceable event (NIP-78, kind 30078) containing its
// "ip:port", signed with Schnorr/secp256k1 (BIP340, the same scheme used by
// Nostr), and subscribes to receive the announcements of other nodes, feeding
// them into the Bitflash address manager.

#ifndef BITFLASH_NOSTR_H
#define BITFLASH_NOSTR_H

#include <string>

// Thread entry point (signature compatible with _beginthread)
void ThreadNostrSeed(void* parg);

// Public relays used for discovery. Tunable.
extern const char* pszNostrRelays[];
extern const int nNostrRelays;

// Rendezvous meeting relay ("host:port") this node registers at and advertises
// in its .btf descriptor. Overridable with /rvrelay=host:port.
extern std::string strBtfMeetingRelay;

// Copy this node's .btf identity (lazily loading or generating it): the x-only
// pubkey (= the .btf address) and the x25519 secret for the end-to-end channel.
bool BtfGetIdentity(unsigned char pubkey[32], unsigned char enc_sk[32]);

// This node's printable .btf address, or "" if the identity failed to load.
std::string BtfLocalAddress();

// Resolve a .btf address via the public discovery relays: fetch the owner's
// self-certified descriptor and return its meeting node ("host:port") and
// x25519 public key for the end-to-end channel.
bool BtfResolve(const std::string& btfAddr, std::string& meetingHostPort,
                unsigned char enc_pub[32]);

#endif
