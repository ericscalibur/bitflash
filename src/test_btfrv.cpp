// Standalone loopback test of the rendezvous transport + echan on top.
// A relay, a service and a client all run in one process on 127.0.0.1 (a unit
// test of the protocol logic -- NOT a network/LAN test). Proves: service
// registers, client pairs by pubkey through the relay, and they exchange data
// end-to-end encrypted so the relay only ever forwards ciphertext.
//
// Build (UCRT64):
//   g++ -std=gnu++14 test_btfrv.cpp btfrv.cpp btfchan.cpp -lsodium -lws2_32 -o test_btfrv.exe

#include "btfrv.h"
#include "btfchan.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace btf;
static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  ok   %s\n", name); } \
    else      { printf("  FAIL %s\n", name); g_fail++; } } while(0)

static const unsigned short PORT = 18443;

// echan framing over a rendezvous socket
static bool WriteFrame(RvSocket s, const unsigned char shared[32], const std::string& pt)
{
    std::vector<unsigned char> f = ChanEncryptFrame(shared, (const unsigned char*)pt.data(), pt.size());
    return RvWriteN(s, f.data(), (int)f.size());
}
static bool ReadFrame(RvSocket s, const unsigned char shared[32], std::string& out)
{
    unsigned char len[4];
    if (!RvReadN(s, len, 4)) return false;
    size_t n = ((size_t)len[0]<<24)|((size_t)len[1]<<16)|((size_t)len[2]<<8)|len[3];
    if (n == 0 || n > (1u<<20)) return false;
    std::vector<unsigned char> body(n);
    if (!RvReadN(s, body.data(), (int)n)) return false;
    std::vector<unsigned char> pt;
    if (!ChanDecryptBody(shared, body.data(), (int)n, pt)) return false;
    out.assign((char*)pt.data(), pt.size());
    return true;
}

static std::string g_svcGot;
static bool g_svcOk = false;

static void ServiceThread(std::vector<unsigned char> pairing_id, std::vector<unsigned char> svc_enc_sk)
{
    RvSocket s = RvServiceRegister("127.0.0.1", PORT, pairing_id.data());
    if (s == RV_INVALID) return;
    // read the client's ephemeral x25519 public key
    unsigned char eph[32];
    if (!RvReadN(s, eph, 32)) { RvClose(s); return; }
    unsigned char shared[32];
    if (!ChanEndpointHandshake(eph, svc_enc_sk.data(), shared)) { RvClose(s); return; }
    std::string msg;
    if (!ReadFrame(s, shared, msg)) { RvClose(s); return; }
    g_svcGot = msg;
    WriteFrame(s, shared, std::string("block accepted: ") + msg);
    g_svcOk = true;
    RvClose(s);
}

int main()
{
    if (!ChanInit() || !RvInit()) { printf("init failed\n"); return 2; }

    // service identity: a 32-byte .btf pairing id + an x25519 enc keypair
    unsigned char svc_enc_pk[32], svc_enc_sk[32];
    ChanKeypair(svc_enc_pk, svc_enc_sk);
    std::vector<unsigned char> pairing_id(32);
    for (int i = 0; i < 32; i++) pairing_id[i] = (unsigned char)(i * 7 + 1);

    printf("rendezvous_relay_pairs_and_forwards_encrypted\n");

    std::thread relay([]{ RvRelayRun(PORT); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it bind

    std::thread svc(ServiceThread, pairing_id, std::vector<unsigned char>(svc_enc_sk, svc_enc_sk+32));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let service register

    // client dials the pairing id through the relay
    RvSocket c = RvClientConnect("127.0.0.1", PORT, pairing_id.data());
    CHECK(c != RV_INVALID, "client paired to service via relay");
    if (c != RV_INVALID)
    {
        unsigned char eph_pub[32], shared_client[32];
        CHECK(ChanClientHandshake(svc_enc_pk, eph_pub, shared_client), "client handshake");
        CHECK(RvWriteN(c, eph_pub, 32), "client sends ephemeral pubkey");
        CHECK(WriteFrame(c, shared_client, "hello one-cpu-one-vote"), "client sends encrypted frame");
        std::string reply;
        CHECK(ReadFrame(c, shared_client, reply), "client reads encrypted reply");
        CHECK(reply == "block accepted: hello one-cpu-one-vote", "end-to-end message round-trips through relay");
        RvClose(c);
    }

    svc.join();
    CHECK(g_svcOk && g_svcGot == "hello one-cpu-one-vote", "service received the decrypted plaintext");

    printf("unknown_pubkey_is_rejected\n");
    {
        unsigned char other[32];
        for (int i = 0; i < 32; i++) other[i] = 0xEE;
        RvSocket bad = RvClientConnect("127.0.0.1", PORT, other);
        CHECK(bad == RV_INVALID, "client for unregistered .btf is rejected");
    }

    RvRelayStop();
    relay.join();

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
