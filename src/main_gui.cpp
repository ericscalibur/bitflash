// Bitflash entry point -- starts node threads then runs GUI (or headless).

#include "headers_core.h"
#ifndef _WIN32
#include <csignal>
#endif

int RunGUI(int argc, char* argv[]);

// Global definitions (were in ui.cpp, now here)
map<string,string> mapAddressBook;
bool               gPoolServerRunning = false;

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
    printf("  /port=N                  (P2P listen port, default 8433)\n");
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

    // net.cpp has claimed nListenPort was "tunable via /port" since it was
    // written, but nothing ever read the option. Without it a second node
    // cannot start on a machine that already runs one -- which is exactly what
    // testing peer exchange needs.
    string strPort = argval2(argc, argv, "/port", "-port");
    if (!strPort.empty())
    {
        int nPort = atoi(strPort.c_str());
        if (nPort <= 0 || nPort > 65535)
            fprintf(stderr, "Ignoring /port=%s: not a port number\n", strPort.c_str());
        else
        {
            nListenPort = htons((unsigned short)nPort);
            addrLocalHost.port = nListenPort;  // or we advertise a port we never bound
        }
    }
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

    printf("Loading block index...\n");
    string strErrors;
    if (!LoadBlockIndex()) { fprintf(stderr,"LoadBlockIndex failed\n"); return 1; }
    printf("Loading wallet...\n");
    if (!LoadWallet())     { fprintf(stderr,"LoadWallet failed\n"); return 1; }
    printf("Height=%d\n", nBestHeight);
    ReacceptWalletTransactions();

    if (!StartNode(strErrors)) { fprintf(stderr,"StartNode: %s\n",strErrors.c_str()); return 1; }

    if (nMineMode == MINE_OPERATOR) {
        gPoolServerRunning = true;
        gPoolRunning = true;
        if (_beginthread(ThreadRPCServer, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadRPCServer) failed\n");
    }
    if (fGenerateBitcoins)
        if (_beginthread(ThreadBitcoinMiner, 0, NULL) == (uintptr_t)-1)
            printf("Error: _beginthread(ThreadBitcoinMiner) failed\n");

    bool fHeadless = arg(argc,argv,"/nogui") || arg(argc,argv,"-nogui") ||
                     arg(argc,argv,"/daemon") || arg(argc,argv,"-daemon");

    if (fHeadless) {
#ifndef _WIN32
        auto sig=[](int){fShutdown=true;};
        signal(SIGINT,sig); signal(SIGTERM,sig);
#endif
        printf("Running headless. Ctrl-C to stop.\n");
        while (!fShutdown) Sleep(500);
        StopNode();
        return 0;
    }

    int ret = RunGUI(argc, argv);
    fShutdown = true;
    StopNode();
    return ret;
}
