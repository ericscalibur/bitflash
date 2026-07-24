// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Headless entry point for the Linux node/miner `bitflash-node`. Replaces the
// wxWidgets GUI (ui.cpp) with a command-line daemon: it loads the chain and
// wallet, starts the P2P/Nostr node (which auto-discovers .btf peers through
// the rendezvous relay), and optionally mines with RandomX. No GUI, no wx.
//
//   ./bitflash-node -gen                 mine with the CPU, auto-connect peers
//   ./bitflash-node -gen -datadir=/path  use a specific data directory
//   ./bitflash-node -rvrelay=host:port   use a specific rendezvous relay
//   ./bitflash-node -connectbtf=<addr>.btf   pin a specific peer
//   ./bitflash-node -solomine -gen       mine without waiting for a peer (test)

#include "headers.h"
#include <csignal>

// ---- UI hooks the core references, provided here as headless equivalents ----

// The core asks the GUI to repaint on new blocks/transactions; nothing to do.
void MainFrameRepaint() {}

// Wallet address book -- defined in ui.cpp on Windows; the core (db.cpp) reads
// and writes it, so the headless build must provide the definition.
map<string, string> mapAddressBook;

// Format a unix timestamp for transaction listings (ui.cpp used wxDateTime).
string DateTimeStr(int64 nTime)
{
    time_t t = (time_t)nTime;
    struct tm* ptm = localtime(&t);
    if (!ptm)
        return "";
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%y %H:%M", ptm);
    return string(buf);
}


// ---- argument parsing (mirrors ui.cpp's ParseParameters) ----
static map<string, string> ParseParameters(int argc, char* argv[])
{
    map<string, string> mapArgs;
    for (int i = 0; i < argc; i++)
    {
        string s = argv[i];
        string val;
        size_t eq = s.find('=');
        if (eq != string::npos)
        {
            val = s.substr(eq + 1);
            s   = s.substr(0, eq);
        }
        for (size_t j = 0; j < s.size(); j++)
            s[j] = (char)tolower((unsigned char)s[j]);
        if (!s.empty() && s[0] == '-')
            s[0] = '/';
        mapArgs[s] = val;
    }
    return mapArgs;
}


static void HandleSignal(int)
{
    fShutdown = true;
}


int main(int argc, char* argv[])
{
    map<string, string> mapArgs = ParseParameters(argc, argv);

    if (mapArgs.count("/?") || mapArgs.count("/help") || mapArgs.count("/h"))
    {
        fprintf(stderr,
            "Bitflash headless node\n"
            "Usage: %s [options]\n"
            "  -gen                  generate coins (mine with RandomX)\n"
            "  -datadir=<dir>        data directory (default ~/.bitflash)\n"
            "  -port=<n>             P2P listen port (default 8433)\n"
            "  -rvrelay=host:port    rendezvous meeting relay (has a seed default)\n"
            "  -announcerelay=h:p    announce a relay you run so nodes discover it\n"
            "  -connectbtf=<a>.btf   keep a connection to a specific .btf peer\n"
            "  -solomine             mine without waiting for a peer (testing)\n"
            "  -debug                verbose logging\n",
            argv[0]);
        return 0;
    }

    // Parameters (same set the Windows UI honors)
    if (mapArgs.count("/datadir"))
        strSetDataDir = mapArgs["/datadir"];
    if (mapArgs.count("/port"))
        nListenPort = htons(atoi(mapArgs["/port"].c_str()));
    if (mapArgs.count("/proxy"))
        addrProxy = CAddress(mapArgs["/proxy"].c_str());
    if (mapArgs.count("/rvrelay"))
        vBtfMeetingRelays = { mapArgs["/rvrelay"] };
    if (mapArgs.count("/announcerelay"))
        strBtfAnnounceRelay = mapArgs["/announcerelay"];
    if (mapArgs.count("/connectbtf"))
        strBtfConnect = mapArgs["/connectbtf"];
    if (mapArgs.count("/debug"))
        fDebug = true;
    if (mapArgs.count("/solomine"))
        fSoloMineTest = true;
    if (mapArgs.count("/gen"))
        fGenerateBitcoins = mapArgs["/gen"].empty() ? true : atoi(mapArgs["/gen"].c_str());

    printf("\nBitflash headless node starting\n");

    //
    // Load data files (mirror of the Windows OnInit2 startup)
    //
    printf("Loading addresses...\n");
    if (!LoadAddresses())
        printf("Error loading addr.dat\n");

    printf("Loading block index...\n");
    if (!LoadBlockIndex())
    {
        printf("Error loading blkindex.dat\n");
        return 1;
    }

    printf("Loading wallet...\n");
    if (!LoadWallet())
    {
        printf("Error loading wallet.dat\n");
        return 1;
    }

    printf("Done loading. nBestHeight=%d, wallet keys=%d\n",
           nBestHeight, (int)mapKeys.size());

    // Rebroadcast any wallet transactions not yet in a block
    ReacceptWalletTransactions();

    //
    // Start the node (P2P + Nostr discovery + .btf rendezvous)
    //
    string strErrors;
    if (!StartNode(strErrors))
    {
        printf("StartNode failed: %s\n", strErrors.c_str());
        return 1;
    }

    //
    // Start mining if requested
    //
    if (fGenerateBitcoins)
    {
        if (_beginthread(ThreadBitcoinMiner, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadBitcoinMiner) failed\n");
        else
            printf("Mining enabled (RandomX, CPU)\n");
    }
    else
    {
        printf("Mining disabled (pass -gen to mine)\n");
    }

    //
    // Run until interrupted
    //
    signal(SIGINT,  HandleSignal);
    signal(SIGTERM, HandleSignal);
    printf("Bitflash node running. Press Ctrl-C to stop.\n");

    while (!fShutdown)
        Sleep(500);

    printf("Shutting down...\n");
    StopNode();
    Sleep(500);
    printf("Bitflash node stopped.\n");
    return 0;
}
