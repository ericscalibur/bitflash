// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// bitflashd -- the headless Bitflash rendezvous relay (the "meeting node").
// Runs on a machine with a public IP (a cheap VPS) and lets nodes behind CGNAT
// reach each other: both sides connect OUT to this relay, which pairs them by
// `.btf` pubkey and forwards ciphertext (it holds no keys and reads nothing).
//
// Build on the VPS (Linux):
//   g++ -std=gnu++14 -O2 -pthread bitflashd.cpp btfrv.cpp -o bitflashd
// Run:
//   ./bitflashd 8434        # or any port you open in the firewall
//
// Cross-builds unchanged on Windows (for local testing):
//   g++ -std=gnu++14 -O2 -pthread bitflashd.cpp btfrv.cpp -lws2_32 -o bitflashd.exe

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

    // Blocks forever, forwarding between paired peers.
    if (!btf::RvRelayRun(port))
    {
        fprintf(stderr, "bitflashd: could not bind/listen on port %u\n", port);
        return 1;
    }
    return 0;
}
