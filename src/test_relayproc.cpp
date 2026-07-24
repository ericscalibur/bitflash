// Tests the tunnel against a SEPARATE running bitflashd relay process (not an
// in-process relay) -- i.e. exactly the VPS deployment shape, on loopback.
// Usage: test_relayproc.exe <port>   (a bitflashd must already listen there)
//
// Build: g++ -std=gnu++14 -O2 -pthread test_relayproc.cpp btftunnel.cpp btfrv.cpp btfchan.cpp -lsodium -lws2_32 -o test_relayproc.exe

#include "btftunnel.h"
#include "btfrv.h"
#include "btfchan.h"
#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace btf;
static int g_fail = 0;
#define CHECK(c,n) do{ if(c){printf("  ok   %s\n",n);} else {printf("  FAIL %s\n",n); g_fail++;} }while(0)

static unsigned short PORT = 18446;
static const char* HOST = "127.0.0.1";
static bool RecvN(SOCKET s, void* b, int n){char*p=(char*)b;int o=0;while(o<n){int r=recv(s,p+o,n-o,0);if(r<=0)return false;o+=r;}return true;}
static bool SendN(SOCKET s, const void* b, int n){const char*p=(const char*)b;int o=0;while(o<n){int r=send(s,p+o,n-o,0);if(r<=0)return false;o+=r;}return true;}

static bool g_ok=false; static std::string g_got;
static void Service(std::vector<unsigned char> id, std::vector<unsigned char> sk, int len)
{
    RvSocket rv = RvServiceRegister(HOST, PORT, id.data());
    if (rv==RV_INVALID) return;
    btf_socket_t app = BtfServiceWrap(rv, sk.data());
    if (app==INVALID_SOCKET) return;
    std::vector<char> buf(len);
    if(!RecvN((SOCKET)app, buf.data(), len)){closesocket((SOCKET)app);return;}
    g_got.assign(buf.data(), len);
    g_ok = SendN((SOCKET)app, buf.data(), len);
    closesocket((SOCKET)app);
}

int main(int argc, char* argv[])
{
    if (argc>=2) HOST=argv[1]; if (argc>=3) PORT=(unsigned short)atoi(argv[2]);
    if(!TunnelInit()){printf("init failed\n");return 2;}
    unsigned char pk[32], sk[32]; ChanKeypair(pk,sk);
    std::vector<unsigned char> id(32); for(int i=0;i<32;i++) id[i]=(unsigned char)(i*11+3);
    std::string msg="anonymous hop through a real bitflashd relay process";
    int len=(int)msg.size();

    printf("tunnel_through_external_relay_process (port %u)\n", PORT);
    std::thread svc(Service, id, std::vector<unsigned char>(sk,sk+32), len);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    btf_socket_t app = BtfClientTunnel(HOST, PORT, id.data(), pk);
    CHECK(app!=INVALID_SOCKET, "client tunneled through the running bitflashd");
    if(app!=INVALID_SOCKET){
        CHECK(SendN((SOCKET)app,msg.data(),len), "app writes plaintext");
        std::vector<char> echo(len);
        CHECK(RecvN((SOCKET)app,echo.data(),len), "app reads echo");
        CHECK(std::string(echo.data(),len)==msg, "round-trips end-to-end via the relay process");
        closesocket((SOCKET)app);
    }
    svc.join();
    CHECK(g_ok && g_got==msg, "service saw correct plaintext");
    printf("\n%s (%d failures)\n", g_fail==0?"ALL TESTS PASSED":"TESTS FAILED", g_fail);
    return g_fail==0?0:1;
}
