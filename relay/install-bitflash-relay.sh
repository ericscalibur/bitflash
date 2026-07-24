#!/usr/bin/env bash
# Bitflash rendezvous relay -- one-shot installer for Linux.
# Anyone can run this on a machine with a PUBLIC IP (a cheap VPS) to become a
# meeting node that lets Bitflash users behind CGNAT reach each other.
# The relay holds NO keys and reads NOTHING -- it only forwards ciphertext.
#
# Usage:
#   sudo bash install-bitflash-relay.sh [PORT]
# PORT defaults to 8434. Remember to open that port in your firewall/panel.

set -e
PORT="${1:-8434}"
DIR="$HOME/bitflash-relay"
mkdir -p "$DIR" && cd "$DIR"

echo ">> writing sources to $DIR"

cat > btfrv.h <<'BTFRV_H_EOF'
// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
#ifndef BITFLASH_BTFRV_H
#define BITFLASH_BTFRV_H
#include <stdint.h>
namespace btf
{
typedef intptr_t RvSocket;
static const RvSocket RV_INVALID = (RvSocket)-1;
bool RvInit();
bool RvRelayRun(unsigned short port);
void RvRelayStop();
RvSocket RvServiceRegister(const char* relay_host, unsigned short port,
                           const unsigned char my_pubkey[32]);
RvSocket RvClientConnect(const char* relay_host, unsigned short port,
                         const unsigned char target_pubkey[32]);
bool RvReadN(RvSocket s, void* buf, int n);
bool RvWriteN(RvSocket s, const void* buf, int n);
void RvClose(RvSocket s);
} // namespace btf
#endif
BTFRV_H_EOF

cat > btfrv.cpp <<'BTFRV_CPP_EOF'
// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
#include "btfrv.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <csignal>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK ::close
#ifndef SD_BOTH
#define SD_BOTH SHUT_RDWR
#endif
#endif
#include <cstring>
#include <cstdio>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
namespace btf
{
static std::atomic<bool> g_stop(false);
static SOCKET            g_listen = INVALID_SOCKET;
static std::map<std::string, SOCKET> g_waiting;
static std::mutex                    g_mtx;
bool RvInit()
{
    static bool done = false;
    if (done) return true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    done = true;
    return true;
}
static bool ReadN(SOCKET s, void* buf, int n)
{
    char* p = (char*)buf;
    int off = 0;
    while (off < n)
    {
        int r = recv(s, p + off, n - off, 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}
static bool WriteN(SOCKET s, const void* buf, int n)
{
    const char* p = (const char*)buf;
    int off = 0;
    while (off < n)
    {
        int r = send(s, p + off, n - off, 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}
static SOCKET ConnectTo(const char* host, unsigned short port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; sprintf(portstr, "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0)
    {
        CLOSESOCK(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}
static void Forward(SOCKET from, SOCKET to)
{
    char buf[32 * 1024];
    for (;;)
    {
        int r = recv(from, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (!WriteN(to, buf, r)) break;
    }
    shutdown(to, SD_BOTH);
    CLOSESOCK(from);
}
static void HandleIncoming(SOCKET s)
{
    unsigned char hdr[33];
    if (!ReadN(s, hdr, 33)) { CLOSESOCK(s); return; }
    char role = (char)hdr[0];
    std::string pubkey((char*)hdr + 1, 32);
    if (role == 'S')
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        std::map<std::string, SOCKET>::iterator it = g_waiting.find(pubkey);
        if (it != g_waiting.end()) CLOSESOCK(it->second);
        g_waiting[pubkey] = s;
    }
    else if (role == 'C')
    {
        SOCKET svc = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            std::map<std::string, SOCKET>::iterator it = g_waiting.find(pubkey);
            if (it != g_waiting.end()) { svc = it->second; g_waiting.erase(it); }
        }
        if (svc == INVALID_SOCKET)
        {
            unsigned char no = 0x00;
            WriteN(s, &no, 1);
            CLOSESOCK(s);
            return;
        }
        unsigned char ok = 0x01;
        bool a = WriteN(svc, &ok, 1);
        bool b = WriteN(s, &ok, 1);
        if (!a || !b) { CLOSESOCK(svc); CLOSESOCK(s); return; }
        std::thread(Forward, s, svc).detach();
        std::thread(Forward, svc, s).detach();
    }
    else
    {
        CLOSESOCK(s);
    }
}
bool RvRelayRun(unsigned short port)
{
    if (!RvInit()) return false;
    g_stop = false;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0) { CLOSESOCK(g_listen); return false; }
    if (listen(g_listen, 16) != 0) { CLOSESOCK(g_listen); return false; }
    while (!g_stop)
    {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        SOCKET s = accept(g_listen, (struct sockaddr*)&cli, &len);
        if (s == INVALID_SOCKET)
        {
            if (g_stop) break;
            continue;
        }
        std::thread(HandleIncoming, s).detach();
    }
    CLOSESOCK(g_listen);
    g_listen = INVALID_SOCKET;
    return true;
}
void RvRelayStop()
{
    g_stop = true;
    if (g_listen != INVALID_SOCKET)
        CLOSESOCK(g_listen);
}
RvSocket RvServiceRegister(const char* relay_host, unsigned short port,
                           const unsigned char my_pubkey[32])
{
    if (!RvInit()) return RV_INVALID;
    SOCKET s = ConnectTo(relay_host, port);
    if (s == INVALID_SOCKET) return RV_INVALID;
    unsigned char hdr[33];
    hdr[0] = (unsigned char)'S';
    memcpy(hdr + 1, my_pubkey, 32);
    if (!WriteN(s, hdr, 33)) { CLOSESOCK(s); return RV_INVALID; }
    unsigned char paired = 0;
    if (!ReadN(s, &paired, 1) || paired != 0x01) { CLOSESOCK(s); return RV_INVALID; }
    return (RvSocket)s;
}
RvSocket RvClientConnect(const char* relay_host, unsigned short port,
                         const unsigned char target_pubkey[32])
{
    if (!RvInit()) return RV_INVALID;
    SOCKET s = ConnectTo(relay_host, port);
    if (s == INVALID_SOCKET) return RV_INVALID;
    unsigned char hdr[33];
    hdr[0] = (unsigned char)'C';
    memcpy(hdr + 1, target_pubkey, 32);
    if (!WriteN(s, hdr, 33)) { CLOSESOCK(s); return RV_INVALID; }
    unsigned char paired = 0;
    if (!ReadN(s, &paired, 1) || paired != 0x01) { CLOSESOCK(s); return RV_INVALID; }
    return (RvSocket)s;
}
bool RvReadN(RvSocket s, void* buf, int n)  { return ReadN((SOCKET)s, buf, n); }
bool RvWriteN(RvSocket s, const void* buf, int n) { return WriteN((SOCKET)s, buf, n); }
void RvClose(RvSocket s) { if (s != RV_INVALID) CLOSESOCK((SOCKET)s); }
} // namespace btf
BTFRV_CPP_EOF

cat > bitflashd.cpp <<'BITFLASHD_EOF'
// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
#include "btfrv.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char* argv[])
{
    unsigned short port = 8434;
    if (argc >= 2)
        port = (unsigned short)atoi(argv[1]);
    printf("bitflashd -- Bitflash rendezvous relay\n");
    printf("listening on 0.0.0.0:%u (open this port in your firewall)\n", port);
    fflush(stdout);
    if (!btf::RvInit())
    {
        fprintf(stderr, "bitflashd: socket init failed\n");
        return 1;
    }
    if (!btf::RvRelayRun(port))
    {
        fprintf(stderr, "bitflashd: could not bind/listen on port %u\n", port);
        return 1;
    }
    return 0;
}
BITFLASHD_EOF

# g++ (Debian/Ubuntu: build-essential; RHEL/Alma: gcc-c++)
if ! command -v g++ >/dev/null 2>&1; then
  echo ">> installing g++"
  if   command -v apt-get >/dev/null 2>&1; then apt-get update && apt-get install -y g++
  elif command -v dnf     >/dev/null 2>&1; then dnf install -y gcc-c++
  elif command -v yum     >/dev/null 2>&1; then yum install -y gcc-c++
  else echo "!! install a C++ compiler (g++) manually and re-run"; exit 1
  fi
fi

echo ">> building bitflashd"
g++ -std=gnu++14 -O2 -pthread bitflashd.cpp btfrv.cpp -o bitflashd
echo ">> build OK: $DIR/bitflashd"

# Basic anti-flood firewall rules (idempotent). These soak up single-source
# connection floods; volumetric DDoS still needs provider-level protection.
# Limits are generous so many users behind one CGNAT public IP aren't blocked.
if command -v iptables >/dev/null 2>&1; then
  echo ">> applying connection rate-limit on port $PORT"
  # Cap concurrent connections per source IP (stops one IP hogging thousands)
  iptables -C INPUT -p tcp --dport "$PORT" -m connlimit --connlimit-above 200 --connlimit-mask 32 -j REJECT --reject-with tcp-reset 2>/dev/null || \
    iptables -A INPUT -p tcp --dport "$PORT" -m connlimit --connlimit-above 200 --connlimit-mask 32 -j REJECT --reject-with tcp-reset
  # Cap NEW-connection rate per source IP (stops connection floods)
  iptables -C INPUT -p tcp --dport "$PORT" -m conntrack --ctstate NEW -m hashlimit \
      --hashlimit-name btfrelay --hashlimit-mode srcip --hashlimit-above 60/min --hashlimit-burst 100 -j DROP 2>/dev/null || \
    iptables -A INPUT -p tcp --dport "$PORT" -m conntrack --ctstate NEW -m hashlimit \
      --hashlimit-name btfrelay --hashlimit-mode srcip --hashlimit-above 60/min --hashlimit-burst 100 -j DROP
  # Persist across reboots if the tooling is present
  command -v netfilter-persistent >/dev/null 2>&1 && netfilter-persistent save >/dev/null 2>&1 || true
else
  echo ">> iptables not found; skipping rate-limit (host on a provider with anti-DDoS)"
fi

# systemd service so it survives reboots (skipped if systemd is absent)
if command -v systemctl >/dev/null 2>&1 && [ -d /etc/systemd/system ]; then
  cat > /etc/systemd/system/bitflashd.service <<EOF
[Unit]
Description=Bitflash rendezvous relay
After=network.target

[Service]
ExecStart=$DIR/bitflashd $PORT
Restart=always
User=$(id -un)

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable --now bitflashd
  sleep 1
  systemctl status bitflashd --no-pager || true
  echo ">> relay running as a service on port $PORT"
else
  echo ">> systemd not found; run it manually:  $DIR/bitflashd $PORT"
fi

echo
echo "==================================================================="
echo " DONE. Open TCP port $PORT in your firewall / cloud panel."
echo " Your relay address for Bitflash users:  <YOUR_PUBLIC_IP>:$PORT"
echo "==================================================================="
