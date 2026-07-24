// Standalone tests for btfchan (ported from Itzal echan.rs behavior).
// Build (UCRT64):
//   g++ -std=gnu++14 test_btfchan.cpp btfchan.cpp -lsodium -o test_btfchan.exe

#include "btfchan.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace btf;
static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  ok   %s\n", name); } \
    else      { printf("  FAIL %s\n", name); g_fail++; } } while(0)

int main()
{
    if (!ChanInit()) { printf("sodium init failed\n"); return 2; }

    // Endpoint (service) has a static keypair; client only knows the static pub.
    unsigned char ep_pk[32], ep_sk[32];
    ChanKeypair(ep_pk, ep_sk);

    printf("handshake_derives_same_key\n");
    unsigned char eph_pub[32], k_client[32], k_endpoint[32];
    CHECK(ChanClientHandshake(ep_pk, eph_pub, k_client), "client handshake ok");
    CHECK(ChanEndpointHandshake(eph_pub, ep_sk, k_endpoint), "endpoint handshake ok");
    CHECK(memcmp(k_client, k_endpoint, 32) == 0, "both sides derive the SAME shared key");

    printf("encrypt_decrypt_roundtrip\n");
    {
        std::string msg = "one CPU one vote -- blinded over the wire";
        std::vector<unsigned char> frame = ChanEncryptFrame(k_client,
            (const unsigned char*)msg.data(), msg.size());
        // frame = [len:4][nonce:24][ct||tag]
        size_t bodylen = (frame[0]<<24)|(frame[1]<<16)|(frame[2]<<8)|frame[3];
        CHECK(bodylen == frame.size() - 4, "length prefix correct");
        std::vector<unsigned char> pt;
        CHECK(ChanDecryptBody(k_endpoint, &frame[4], bodylen, pt), "endpoint decrypts");
        CHECK(pt.size() == msg.size() && memcmp(pt.data(), msg.data(), msg.size()) == 0, "plaintext matches");
    }

    printf("tampered_ciphertext_fails\n");
    {
        std::string msg = "secret block";
        std::vector<unsigned char> frame = ChanEncryptFrame(k_client,
            (const unsigned char*)msg.data(), msg.size());
        size_t bodylen = frame.size() - 4;
        frame[frame.size()-1] ^= 0x01; // flip a bit in the tag/ciphertext
        std::vector<unsigned char> pt;
        CHECK(!ChanDecryptBody(k_endpoint, &frame[4], bodylen, pt), "tampered frame rejected");
    }

    printf("wrong_key_fails\n");
    {
        unsigned char other_pk[32], other_sk[32];
        ChanKeypair(other_pk, other_sk);
        unsigned char eph2[32], kbad[32];
        ChanClientHandshake(other_pk, eph2, kbad); // box to a DIFFERENT endpoint
        std::string msg = "for someone else";
        std::vector<unsigned char> frame = ChanEncryptFrame(kbad,
            (const unsigned char*)msg.data(), msg.size());
        size_t bodylen = frame.size() - 4;
        std::vector<unsigned char> pt;
        CHECK(!ChanDecryptBody(k_endpoint, &frame[4], bodylen, pt), "our endpoint can't open someone else's box");
    }

    printf("empty_and_large_payloads\n");
    {
        std::vector<unsigned char> empty;
        std::vector<unsigned char> f0 = ChanEncryptFrame(k_client, NULL, 0);
        std::vector<unsigned char> o0;
        CHECK(ChanDecryptBody(k_endpoint, &f0[4], f0.size()-4, o0) && o0.empty(), "empty payload roundtrips");
        std::vector<unsigned char> big(200000, 0xAB);
        std::vector<unsigned char> fb = ChanEncryptFrame(k_client, big.data(), big.size());
        std::vector<unsigned char> ob;
        CHECK(ChanDecryptBody(k_endpoint, &fb[4], fb.size()-4, ob) && ob == big, "200KB payload roundtrips");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
