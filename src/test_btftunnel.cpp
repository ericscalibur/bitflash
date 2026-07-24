// Full-stack loopback test of the anonymous tunnel: relay + service + client in
// one process on 127.0.0.1 (unit test of the logic, NOT a LAN/network test).
// The app-facing sockets are plain TCP; the wire between them is echan over the
// rendezvous relay. Proves CNode can use a .btf peer as an ordinary socket.
//
// Build (UCRT64):
//   g++ -std=gnu++14 test_btftunnel.cpp btftunnel.cpp btfrv.cpp btfchan.cpp -lsodium -lws2_32 -o test_btftunnel.exe

#include "btftunnel.h"
#include "btfrv.h"
#include "btfchan.h"
#include <winsock2.h>
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

static const unsigned short PORT = 18444;

static bool RecvN(btf_socket_t s, void* buf, int n)
{
    char* p = (char*)buf; int off = 0;
    while (off < n) { int r = recv((SOCKET)s, p + off, n - off, 0); if (r <= 0) return false; off += r; }
    return true;
}
static bool SendN(btf_socket_t s, const void* buf, int n)
{
    const char* p = (const char*)buf; int off = 0;
    while (off < n) { int r = send((SOCKET)s, p + off, n - off, 0); if (r <= 0) return false; off += r; }
    return true;
}

static bool g_svcOk = false;
static std::string g_svcGot;

static void ServiceThread(std::vector<unsigned char> pairing_id, std::vector<unsigned char> enc_sk, int msglen)
{
    RvSocket rv = RvServiceRegister("127.0.0.1", PORT, pairing_id.data());
    if (rv == RV_INVALID) return;
    btf_socket_t app = BtfServiceWrap(rv, enc_sk.data());
    if (app == INVALID_SOCKET) return;
    // Read the client's plaintext message and echo it back (as a normal socket).
    std::vector<char> buf(msglen);
    if (!RecvN(app, buf.data(), msglen)) { closesocket((SOCKET)app); return; }
    g_svcGot.assign(buf.data(), msglen);
    g_svcOk = SendN(app, buf.data(), msglen);
    closesocket((SOCKET)app);
}

int main()
{
    if (!TunnelInit()) { printf("init failed\n"); return 2; }

    unsigned char svc_enc_pk[32], svc_enc_sk[32];
    ChanKeypair(svc_enc_pk, svc_enc_sk);
    std::vector<unsigned char> pairing_id(32);
    for (int i = 0; i < 32; i++) pairing_id[i] = (unsigned char)(i * 3 + 5);

    std::string msg = "hello one-cpu-one-vote via anonymous .btf tunnel";
    int msglen = (int)msg.size();

    printf("anonymous_tunnel_full_stack\n");

    std::thread relay([]{ RvRelayRun(PORT); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::thread svc(ServiceThread, pairing_id,
                    std::vector<unsigned char>(svc_enc_sk, svc_enc_sk+32), msglen);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    btf_socket_t app = BtfClientTunnel("127.0.0.1", PORT, pairing_id.data(), svc_enc_pk);
    CHECK(app != INVALID_SOCKET, "client built anonymous tunnel to service");
    if (app != INVALID_SOCKET)
    {
        CHECK(SendN(app, msg.data(), msglen), "app writes plaintext to the tunnel socket");
        std::vector<char> echo(msglen);
        CHECK(RecvN(app, echo.data(), msglen), "app reads echo from the tunnel socket");
        CHECK(std::string(echo.data(), msglen) == msg, "plaintext round-trips end-to-end through the relay");
        closesocket((SOCKET)app);
    }

    svc.join();
    CHECK(g_svcOk && g_svcGot == msg, "service saw the correct decrypted plaintext");

    RvRelayStop();
    relay.join();

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
