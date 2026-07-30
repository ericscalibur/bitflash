// Bitflash entry point -- starts node threads then runs GUI (or headless).

#include "headers_core.h"
#ifndef _WIN32
#include <csignal>
#endif

// BITFLASH_NO_GUI builds the same entry point without ImGui, GLFW or OpenGL,
// so the binary does not need libGL present just to start. See headless.cpp.
#ifndef BITFLASH_NO_GUI
int RunGUI(int argc, char* argv[]);
#endif

// Global definitions (were in ui.cpp, now here)
map<string,string> mapAddressBook;
bool               gPoolServerRunning = false;
int                nWalletRpcPort = 8901;   // /walletrpcport=N
bool               fWalletRpc     = false;  // /walletrpc -- off unless asked for
string             strWalletRpcBind = "127.0.0.1";  // /walletrpcbind=ADDR
void ThreadWalletRPC(void*);                // walletrpc.cpp

static bool arg(int argc, char* argv[], const char* key)
{
    for (int i=1;i<argc;i++) { string s=argv[i]; if(s==key||s.substr(0,s.find('='))==key) return true; }
    return false;
}
static string argval(int argc, char* argv[], const char* key)
{
    for (int i=1;i<argc;i++) {
        string s=argv[i]; size_t eq=s.find('=');
        if(eq!=string::npos && s.substr(0,eq)==key) return s.substr(eq+1);
    }
    return "";
}

static string argval2(int argc, char* argv[], const char* keySlash, const char* keyDash)
{
    string v = argval(argc, argv, keySlash);
    if (!v.empty())
        return v;
    return argval(argc, argv, keyDash);
}

static void PrintUsage()
{
    printf("Bitflash command-line options\n");
    printf("\n");
    printf("General:\n");
    printf("  /help, -help, --help, /?\n");
    printf("  /datadir=PATH\n");
    printf("  /debug\n");
    printf("  /gen\n");
    printf("  /nogui or /daemon\n");
    printf("\n");
    printf("Mining mode:\n");
    printf("  /operator\n");
    printf("  /participant=POOL_BTF_ADDRESS\n");
    printf("  /solomine\n");
    printf("\n");
    printf("Pool operator announcement:\n");
    printf("  /poolname=NAME\n");
    printf("  /poolfee=PCT\n");
    printf("  /pooldashboard=URL  (alias: /pooldash=URL)\n");
    printf("\n");
    printf(".btf and rendezvous:\n");
    printf("  /connectbtf=PEER_BTF_ADDRESS\n");
    printf("  /rvrelay=HOST:PORT\n");
    printf("  /announcerelay=HOST:PORT\n");
    printf("\n");
    printf("Wallet GUI:\n");
    printf("  /walletrpc                 (serve the wallet UI on 127.0.0.1)\n");
    printf("  /walletrpcport=N           (default 8901)\n");
    printf("  /walletrpcbind=ADDR        (default 127.0.0.1; use 0.0.0.0 in Docker\n");
    printf("                              with -p 127.0.0.1:8901:8901)\n");
    printf("\n");
    printf("Network:\n");
    printf("  /port=N                    (P2P listen port, default 8433)\n");
    printf("  /btfseed=ADDRESS:ENCHEX    (extra bootstrap peer, repeatable)\n");
    printf("\n");
    printf("Each option also accepts '-' instead of '/'.\n");
}

static void ParseStartupArguments(int argc, char* argv[])
{
    if (arg(argc,argv,"/datadir") || arg(argc,argv,"-datadir"))
        strSetDataDir = argval2(argc, argv, "/datadir", "-datadir");

    if (arg(argc,argv,"/debug") || arg(argc,argv,"-debug"))
        fDebug = true;

    if (arg(argc,argv,"/gen") || arg(argc,argv,"-gen"))
        fGenerateBitcoins = 1;

    if (arg(argc,argv,"/walletrpc") || arg(argc,argv,"-walletrpc"))
        fWalletRpc = true;

    string strWBind = argval2(argc, argv, "/walletrpcbind", "-walletrpcbind");
    if (!strWBind.empty())
        strWalletRpcBind = strWBind;

    string strWrpc = argval2(argc, argv, "/walletrpcport", "-walletrpcport");
    if (!strWrpc.empty())
    {
        int n = atoi(strWrpc.c_str());
        if (n <= 0 || n > 65535)
            fprintf(stderr, "Ignoring /walletrpcport=%s: not a port number\n", strWrpc.c_str());
        else
            nWalletRpcPort = n;
    }

    if (arg(argc,argv,"/solomine") || arg(argc,argv,"-solomine"))
        fSoloMineTest = true;

    if (arg(argc,argv,"/operator") || arg(argc,argv,"-operator"))
        nMineMode = MINE_OPERATOR;

    if (arg(argc,argv,"/participant") || arg(argc,argv,"-participant"))
    {
        nMineMode = MINE_PARTICIPANT;
        strParticipantPool = argval2(argc, argv, "/participant", "-participant");
    }

    string poolName = argval2(argc, argv, "/poolname", "-poolname");
    if (!poolName.empty())
        strPoolName = poolName;

    string poolDash = argval2(argc, argv, "/pooldashboard", "-pooldashboard");
    if (poolDash.empty())
        poolDash = argval2(argc, argv, "/pooldash", "-pooldash");
    if (!poolDash.empty())
        strPoolDashboardUrl = poolDash;

    string poolFee = argval2(argc, argv, "/poolfee", "-poolfee");
    if (!poolFee.empty())
        dPoolFeePercent = atof(poolFee.c_str());

    string btfConnect = argval2(argc, argv, "/connectbtf", "-connectbtf");
    if (!btfConnect.empty())
        strBtfConnect = btfConnect;

    string rvRelay = argval2(argc, argv, "/rvrelay", "-rvrelay");
    if (!rvRelay.empty())
    {
        vBtfMeetingRelays.clear();
        vBtfMeetingRelays.push_back(rvRelay);
    }

    string announceRelay = argval2(argc, argv, "/announcerelay", "-announcerelay");
    if (!announceRelay.empty())
        strBtfAnnounceRelay = announceRelay;

    // /btfseed=ADDRESS:ENCHEX -- extra bootstrap peers, repeatable. Useful for
    // testing the seed path and for private networks that ship no compiled list.
    for (int i = 1; i < argc; i++)
    {
        string s = argv[i];
        size_t eq = s.find('=');
        if (eq == string::npos) continue;
        string key = s.substr(0, eq);
        if (key != "/btfseed" && key != "-btfseed") continue;
        string val = s.substr(eq + 1);
        size_t colon = val.rfind(':');
        if (colon == string::npos || colon + 1 >= val.size())
        {
            fprintf(stderr, "Ignoring %s: expected ADDRESS:ENCHEX\n", s.c_str());
            continue;
        }
        vBtfExtraSeeds.push_back(make_pair(val.substr(0, colon), val.substr(colon + 1)));
    }

    // net.cpp has described nListenPort as "tunable via /port" since it was
    // written, but nothing ever read the option, so the port was fixed at 8433
    // and a second node could not start on a machine already running one.
    string strPort = argval2(argc, argv, "/port", "-port");
    if (!strPort.empty())
    {
        int nPort = atoi(strPort.c_str());
        if (nPort <= 0 || nPort > 65535)
            fprintf(stderr, "Ignoring /port=%s: not a port number\n", strPort.c_str());
        else
        {
            nListenPort = htons((unsigned short)nPort);
            // Or we would announce a port we never bound.
            addrLocalHost.port = nListenPort;
        }
    }
}


// Headless status line.
//
// GetBalance() had exactly one caller, the ImGui GUI, so a headless node could
// mine indefinitely with no way to report what it had earned. The found-block
// message in BitcoinMiner() sits behind the "net" log category, which is off
// unless /debug is passed, so a solo miner that found a block printed nothing
// whatsoever.
//
// Mined coins are coinbase outputs and CWalletTx::GetCredit() deliberately
// values immature coinbase at 0, so mature and immature are reported apart
// rather than summed into one misleading number.
static void PrintStatusLine()
{
    int64 nMature   = GetBalance();
    int64 nImmature = 0;
    int nMinedTotal = 0, nMinedImmature = 0;
    CRITICAL_BLOCK(cs_mapWallet)
    {
        for (map<uint256, CWalletTx>::iterator it = mapWallet.begin();
             it != mapWallet.end(); ++it)
        {
            CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsCoinBase())
                continue;
            nMinedTotal++;
            if (pcoin->GetBlocksToMaturity() > 0)
            {
                nMinedImmature++;
                nImmature += pcoin->CTransaction::GetCredit();
            }
        }
    }
    int nPeers = 0;
    CRITICAL_BLOCK(cs_vNodes)
        nPeers = (int)vNodes.size();

    printf("STATUS height=%d peers=%d blocks_mined=%d spendable=%s maturing=%s (%d block(s))\n",
           nBestHeight, nPeers, nMinedTotal,
           FormatMoney(nMature).c_str(),
           FormatMoney(nImmature).c_str(), nMinedImmature);
    // Only the GUI ever displayed this, so a headless operator had no way to
    // learn the address other nodes must dial to reach them -- which makes
    // deliberate peering with a known node impossible.
    printf("STATUS btf=%s\n", BtfLocalAddress().c_str());
    if (fGenerateBitcoins)
        printf("STATUS hashrate=%.0f H/s\n", HashMeterRate());
    fflush(stdout);
}

int main(int argc, char* argv[])
{
    if (arg(argc,argv,"/help") || arg(argc,argv,"-help") ||
        arg(argc,argv,"--help") || arg(argc,argv,"/?"))
    {
        PrintUsage();
        return 0;
    }

    ParseStartupArguments(argc, argv);

    // Berkeley DB reports failure by throwing, and CDB's constructor lets it
    // through. Nothing on this path caught anything, so an unreadable
    // blkindex.dat or wallet.dat -- one written by another platform's Berkeley
    // DB, a truncated file, a version mismatch -- unwound out of main() into
    // std::terminate and abort(). On Windows that surfaces as
    // STATUS_STACK_BUFFER_OVERRUN (0xC0000409) inside ucrtbase.dll: no message,
    // no log line past "Loading wallet...", and a faulting module with nothing
    // to do with the real problem. Confirmed by stack trace:
    //
    //   libdb_cxx-6.2.dll -> CDB::CDB(...) -> LoadBlockIndex(...) -> main()
    //
    // The program knew what had gone wrong and threw the reason away.
    string strErrors;
    printf("Loading block index...\n");
    try
    {
        if (!LoadBlockIndex()) { fprintf(stderr, "LoadBlockIndex failed\n"); return 1; }
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Cannot read the block index: %s\n", e.what());
        fprintf(stderr, "blkindex.dat and blk0001.dat may be from another machine or "
                        "incomplete. Deleting both is safe -- they are re-downloaded -- "
                        "but never delete wallet.dat, which holds your keys.\n");
        return 1;
    }

    printf("Loading wallet...\n");
    try
    {
        if (!LoadWallet()) { fprintf(stderr, "LoadWallet failed\n"); return 1; }
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Cannot read wallet.dat: %s\n", e.what());
        fprintf(stderr, "The file was left untouched. A wallet.dat written by a "
                        "different platform's Berkeley DB is the usual cause; back it "
                        "up before trying anything else.\n");
        return 1;
    }
    printf("Height=%d\n", nBestHeight);
    ReacceptWalletTransactions();

    if (!StartNode(strErrors)) { fprintf(stderr,"StartNode: %s\n",strErrors.c_str()); return 1; }

    if (fWalletRpc)
        if (_beginthread(ThreadWalletRPC, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadWalletRPC) failed\n");

    if (nMineMode == MINE_OPERATOR) {
        gPoolServerRunning = true;
        gPoolRunning = true;
        if (_beginthread(ThreadRPCServer, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadRPCServer) failed\n");
    }
    if (fGenerateBitcoins)
        if (_beginthread(ThreadBitcoinMiner, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadBitcoinMiner) failed\n");

#ifdef BITFLASH_NO_GUI
    // Nothing else this binary can do; /nogui is accepted and redundant.
    bool fHeadless = true;
#else
    bool fHeadless = arg(argc,argv,"/nogui") || arg(argc,argv,"-nogui") ||
                     arg(argc,argv,"/daemon") || arg(argc,argv,"-daemon");
#endif

    if (fHeadless) {
#ifndef _WIN32
        auto sig=[](int){fShutdown=true;};
        signal(SIGINT,sig); signal(SIGTERM,sig);
#endif
        printf("Running headless. Ctrl-C to stop.\n");
        // Status every 60s (BITFLASH_STATUS_SECS=0 disables).
        int nStatusSecs = 60;
        const char* pszStatus = getenv("BITFLASH_STATUS_SECS");
        if (pszStatus) nStatusSecs = atoi(pszStatus);
        int64 nLastStatus = 0;
        while (!fShutdown)
        {
            Sleep(500);
            if (nStatusSecs > 0 && GetTime() - nLastStatus >= nStatusSecs)
            {
                nLastStatus = GetTime();
                PrintStatusLine();
            }
        }
        StopNode();
        return 0;
    }

#ifdef BITFLASH_NO_GUI
    return 0;   // unreachable: fHeadless is always true in this build
#else
    int ret = RunGUI(argc, argv);
    fShutdown = true;
    StopNode();
    return ret;
#endif
}
