// Copyright (c) 2009 Satoshi Nakamoto / Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Bitflash pool server — .btf-only pool listener.
// Miners connect through rendezvous tunnels; no raw IP:port endpoints remain.
// Pending payouts are held until maturity and shown in the GUI.
//
// Compiles on both Windows (mingw) and Linux unchanged.
// Launched from ui.cpp (GUI) and headless.cpp (node) via ThreadRPCServer().

// nlohmann must be included before headers.h because util.h redefines snprintf
#pragma push_macro("snprintf")
#undef snprintf
#include <nlohmann/json.hpp>
#pragma pop_macro("snprintf")

#include "headers.h"
#ifdef snprintf
#undef snprintf
#endif
#include "btftunnel.h"

// POSIX socket headers — already provided by compat.h on Windows,
// and by the OS directly on Linux.
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <atomic>
#include <mutex>
#include <map>
#include <deque>
#include <sstream>
#include <iomanip>
#include <ctime>

using json = nlohmann::json;

// Cross-platform socket close / send flags
#ifdef _WIN32
#define sock_close(s)  closesocket(s)
#define SEND_FLAGS     0
#else
#define sock_close(s)  ::close(s)
#define SEND_FLAGS     MSG_NOSIGNAL
#endif

// Cross-platform thread launch: wraps _beginthread (compat.h maps it to
// pthread_create+detach on Linux, native _beginthread on Windows).
// fn must be void(*)(void*).
static void LaunchThread(void (*fn)(void*), void* arg)
{
    _beginthread(fn, 0, arg);
}

// ---------------------------------------------------------------------------
// Config — set by headless.cpp / ui.cpp before threads launch
// ---------------------------------------------------------------------------
std::string gRpcUser;
std::string gRpcPassword;

// Pool server running flag — set false to stop server cleanly (all loops check it)
volatile bool gPoolRunning = false;

// ---------------------------------------------------------------------------
// Stubs for symbols absent from Bitflash 0.1.0
// ---------------------------------------------------------------------------
double dHashesPerSec  = 0.0;
int64  nHPSTimerStart = 0;

static bool IsInitialBlockDownload()
{
    return (pindexBest == NULL || nBestHeight < 1);
}

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------
static std::string ToHex(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    std::string s; s.reserve(n*2);
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { s += H[b[i]>>4]; s += H[b[i]&0xf]; }
    return s;
}
static std::vector<unsigned char> FromHex(const std::string& s)
{
    std::vector<unsigned char> v;
    for (size_t i = 0; i+1 < s.size(); i += 2) {
        auto h=[](char c)->int{
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return 0;
        };
        v.push_back((unsigned char)((h(s[i])<<4)|h(s[i+1])));
    }
    return v;
}

// ---------------------------------------------------------------------------
// PPLNS share tracking
// ---------------------------------------------------------------------------
struct Share {
    std::string minerAddress;
    int64       timestamp;
    uint64      difficulty;
};

struct MinerStats {
    std::string address;
    std::string worker;
    uint64 totalShares = 0;
    uint64 roundShares = 0;
    int64  lastSeen    = 0;
};
static std::mutex                        gMinerStatsMutex;
static std::map<std::string, MinerStats> gMinerStats;

struct FoundBlock {
    int    height;
    int64  timestamp;
    std::string minerAddress;
    uint256 hash;
};
static std::mutex             gBlocksMutex;
static std::deque<FoundBlock> gFoundBlocks;

struct Miner;
static std::mutex          gMinersMutex;
static std::vector<Miner*> gMiners;

static std::string MinerKey(const std::string& address, const std::string& worker)
{
    return address + "\n" + worker;
}

void GetPoolWorkerStats(std::vector<PoolWorkerStatView>& out)
{
    out.clear();
    int64 now = GetTime();
    std::lock_guard<std::mutex> lk(gMinerStatsMutex);
    for (std::map<std::string, MinerStats>::const_iterator it = gMinerStats.begin(); it != gMinerStats.end(); ++it)
    {
        const MinerStats& ms = it->second;
        if (ms.lastSeen <= 0)
            continue;
        if (now - ms.lastSeen > 15 * 60)
            continue;
        PoolWorkerStatView view;
        view.address = ms.address;
        view.worker = ms.worker;
        view.totalShares = ms.totalShares;
        view.roundShares = ms.roundShares;
        view.lastSeen = ms.lastSeen;
        out.push_back(view);
    }
    std::sort(out.begin(), out.end(), [](const PoolWorkerStatView& a, const PoolWorkerStatView& b) {
        if (a.roundShares != b.roundShares) return a.roundShares > b.roundShares;
        if (a.totalShares != b.totalShares) return a.totalShares > b.totalShares;
        return a.lastSeen > b.lastSeen;
    });
}

// ---------------------------------------------------------------------------
// Current mining job
// ---------------------------------------------------------------------------
struct StratumJob {
    std::string jobId;
    CBlock      block;
    uint256     target;
    uint256     shareTarget;
    int         height;
};

static std::mutex    gJobMutex;
static StratumJob    gCurrentJob;
static bool          gHaveJob = false;
static std::atomic<uint32_t> gJobSeq{0};

static uint256 MakeShareTarget(const uint256& blockTarget)
{
    uint256 shareTarget = blockTarget;
    shareTarget <<= 8;
    if (shareTarget < blockTarget)
        shareTarget = ~uint256(0);
    return shareTarget;
}

static bool RebuildJob()
{
    if (!pindexBest) return false;

    CBlockIndex* pindexPrev = pindexBest;
    unsigned int nBits = GetNextWorkRequired(pindexPrev);

    CKey key;
    key.MakeNewKey();
    static std::atomic<uint32_t> sExtra{0};
    uint32_t extraNonce = ++sExtra;

    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vin[0].scriptSig << nBits << (CBigNum)extraNonce;
    txNew.vout.resize(1);
    // Always pay coinbase to a fresh wallet key. DoPayouts() distributes
    // shares to miners via SendMoney() after the block is accepted.
    txNew.vout[0].scriptPubKey << key.GetPubKey() << OP_CHECKSIG;

    CBlock block;
    block.vtx.push_back(txNew);

    {
        CTxDB txdb("r");
        CRITICAL_BLOCK(cs_mapTransactions)
        {
            map<uint256,CTxIndex> pool;
            unsigned int sz = 0;
            for (auto& kv : mapTransactions) {
                CTransaction& tx = kv.second;
                if (tx.IsCoinBase() || !tx.IsFinal()) continue;
                int64 nFees = 0;
                int64 nMinFee = tx.GetMinFee(block.vtx.size() < 100);
                map<uint256,CTxIndex> tmp(pool);
                if (!tx.ConnectInputs(txdb, tmp, CDiskTxPos(1,1,1), 0, nFees, false, true, nMinFee)) continue;
                pool = tmp;
                block.vtx.push_back(tx);
                sz += ::GetSerializeSize(tx, SER_NETWORK);
                if (sz > MAX_SIZE/2) break;
            }
        }
    }
    block.vtx[0].vout[0].nValue = block.GetBlockValue(0);
    block.hashPrevBlock  = pindexPrev->GetBlockHash();
    block.hashMerkleRoot = block.BuildMerkleTree();
    block.nTime = max((unsigned int)(pindexPrev->GetMedianTimePast()+1),
                      (unsigned int)GetAdjustedTime());
    block.nBits  = nBits;
    block.nNonce = 1;

    AddKey(key);

    StratumJob job;
    job.jobId  = ToHex(&extraNonce, 4);
    job.block  = block;
    job.target = CBigNum().SetCompact(nBits).getuint256();
    job.shareTarget = MakeShareTarget(job.target);
    job.height = nBestHeight + 1;

    std::lock_guard<std::mutex> lk(gJobMutex);
    gCurrentJob = job;
    gHaveJob    = true;
    ++gJobSeq;
    return true;
}

// ---------------------------------------------------------------------------
// PPLNS payout
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Deferred payouts — coinbase outputs require COINBASE_MATURITY (100) blocks
// before they can be spent. We store the share snapshot when a block is found
// and execute the actual SendMoney() calls once the coinbase matures.
// ---------------------------------------------------------------------------
struct PendingPayout {
    int matureAtHeight;                   // pay out when nBestHeight >= this
    std::map<std::string, int64> amounts; // addr -> BTF amount
};

static std::mutex                   gPayoutMutex;
static std::vector<PendingPayout>   gPendingPayouts;

void GetPendingPayouts(std::vector<PendingPayoutView>& out)
{
    out.clear();
    std::lock_guard<std::mutex> lk(gPayoutMutex);
    for (const PendingPayout& p : gPendingPayouts)
    {
        PendingPayoutView view;
        view.matureAtHeight = p.matureAtHeight;
        view.recipients = (int)p.amounts.size();
        view.totalAmount = 0;
        for (const std::map<std::string, int64>::value_type& kv : p.amounts)
            view.totalAmount += kv.second;
        out.push_back(view);
    }
}

// Called when a block is found: snapshot shares, schedule payout for maturity.
static void QueuePayouts(int blockHeight)
{
    if (nMineMode != MINE_OPERATOR) return;

    std::map<std::string, uint64> shareCount;
    uint64 total = 0;
    {
        std::lock_guard<std::mutex> lk(gMinerStatsMutex);
        for (std::map<std::string, MinerStats>::iterator it = gMinerStats.begin(); it != gMinerStats.end(); ++it)
        {
            MinerStats& ms = it->second;
            if (ms.roundShares == 0)
                continue;
            if (ms.address.empty())
                continue;
            shareCount[ms.address] += ms.roundShares;
            total += ms.roundShares;
            ms.roundShares = 0;
        }
    }
    if (total == 0) return;

    int64 blockReward = CBlock().GetBlockValue(0);
    PendingPayout pp;
    pp.matureAtHeight = blockHeight + COINBASE_MATURITY;
    for (auto& kv : shareCount) {
        int64 amount = (int64)(((double)kv.second / (double)total) * (double)blockReward);
        if (amount >= CENT)
            pp.amounts[kv.first] = amount;
    }

    { std::lock_guard<std::mutex> lk(gPayoutMutex); gPendingPayouts.push_back(pp); }

    printf("Pool: block %d payout queued for height %d (%zu miners)\n",
           blockHeight, pp.matureAtHeight, pp.amounts.size());
}

// Called from JobWatcher every new block: execute any matured payouts.
static void FlushMaturePayouts()
{
    if (nMineMode != MINE_OPERATOR) return;

    std::lock_guard<std::mutex> lk(gPayoutMutex);
    for (auto it = gPendingPayouts.begin(); it != gPendingPayouts.end(); ) {
        if (nBestHeight < it->matureAtHeight) { ++it; continue; }

        printf("Pool: executing payout for block matured at height %d\n", it->matureAtHeight);
        for (auto& kv : it->amounts) {
            uint160 hash160;
            if (!AddressToHash160(kv.first, hash160)) continue;
            CScript scriptPubKey;
            scriptPubKey << OP_DUP << OP_HASH160 << hash160 << OP_EQUALVERIFY << OP_CHECKSIG;
            CWalletTx wtx;
            wtx.mapValue["comment"] = strprintf("Pool payout height %d", it->matureAtHeight);
            if (SendMoney(scriptPubKey, kv.second, wtx))
                printf("Pool: paid %s to %s\n", FormatMoney(kv.second).c_str(), kv.first.c_str());
            else
                printf("Pool: payout FAILED for %s (will not retry)\n", kv.first.c_str());
        }
        it = gPendingPayouts.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Stratum protocol
// ---------------------------------------------------------------------------
static json MakeNotifyParams(const StratumJob& job, bool clean)
{
    CBlock& b = const_cast<CBlock&>(job.block);
    unsigned char hdr[80];
    memcpy(hdr,    &b.nVersion,       4);
    memcpy(hdr+4,  &b.hashPrevBlock,  32);
    memcpy(hdr+36, &b.hashMerkleRoot, 32);
    memcpy(hdr+68, &b.nTime,          4);
    memcpy(hdr+72, &b.nBits,          4);
    memcpy(hdr+76, &b.nNonce,         4);
    unsigned char shareTgt[32]; memcpy(shareTgt, &job.shareTarget, 32);
    return json::array({job.jobId, ToHex(hdr,80), b.hashPrevBlock.GetHex(), ToHex(shareTgt,32), clean});
}

struct Miner {
    SOCKET      fd;
    bool        authorised;
    std::string address;
    std::string worker;
    std::string readBuf;
    int64       connectTime;
    uint64      sessionShares;
    Miner(SOCKET f) : fd(f), authorised(false), connectTime(GetTime()), sessionShares(0) {}
};

void GetPoolOperatorStats(int& authorizedMiners, int& blocksFound, uint64& roundShares)
{
    authorizedMiners = 0;
    blocksFound = 0;
    roundShares = 0;
    {
        std::lock_guard<std::mutex> lk(gMinersMutex);
        for (Miner* m : gMiners)
            if (m->authorised)
                authorizedMiners++;
    }
    {
        std::lock_guard<std::mutex> lk(gBlocksMutex);
        blocksFound = (int)gFoundBlocks.size();
    }
    {
        std::lock_guard<std::mutex> lk(gMinerStatsMutex);
        for (std::map<std::string, MinerStats>::const_iterator it = gMinerStats.begin(); it != gMinerStats.end(); ++it)
            roundShares += it->second.roundShares;
    }
}

static bool SendLine(Miner* m, const json& j)
{
    std::string s = j.dump() + "\n";
    int off = 0;
    while (off < (int)s.size()) {
        int n = send(m->fd, s.c_str() + off, (int)s.size() - off, SEND_FLAGS);
        if (n <= 0)
            return false;
        off += n;
    }
    return true;
}

static void BroadcastJob(bool clean)
{
    StratumJob job;
    { std::lock_guard<std::mutex> lk(gJobMutex); if(!gHaveJob) return; job=gCurrentJob; }
    json n = {{"id",nullptr},{"method","mining.notify"},{"params",MakeNotifyParams(job,clean)}};
    std::lock_guard<std::mutex> lk(gMinersMutex);
    for (Miner* m : gMiners)
        if (m->authorised && !SendLine(m, n))
            printf("Stratum: failed to broadcast job to %s\n", m->address.c_str());
}

static void HandleLine(Miner* m, const std::string& line)
{
    json req;
    try { req = json::parse(line); } catch(...){ return; }

    json id = req.value("id", json(nullptr));
    std::string method = req.value("method","");

    auto reply = [&](json result, json error=nullptr){
        if (!SendLine(m, json{{"id",id},{"result",result},{"error",error}}))
            printf("Stratum: failed to reply to miner %s\n", m->address.c_str());
    };

    if (method == "mining.subscribe") {
        std::string sid = ToHex(&m->fd, 4);
        printf("Stratum: miner subscribed %s\n", sid.c_str());
        reply(json::array({json::array({json::array({"mining.notify",sid})}),sid,4}));
        return;
    }

    if (method == "mining.authorize") {
        auto& p = req["params"];
        m->address = (p.size()>0 && p[0].is_string()) ? p[0].get<std::string>() : "";
        m->worker  = (p.size()>1 && p[1].is_string()) ? p[1].get<std::string>() : "worker";
        m->authorised = true;
        printf("Stratum: miner authorized address=%s worker=%s\n", m->address.c_str(), m->worker.c_str());
        reply(true);

        uint160 h; bool valid = AddressToHash160(m->address, h);
        if (!valid) printf("Stratum: miner %s invalid address '%s' — shares won't pay out\n",
                           m->worker.c_str(), m->address.c_str());

        { std::lock_guard<std::mutex> lk(gMinerStatsMutex);
          auto& ms = gMinerStats[m->address];
          ms.lastSeen = GetTime(); ms.worker = m->worker; }

        if (!SendLine(m, json{{"id",nullptr},{"method","mining.set_difficulty"},{"params",json::array({1})}}))
            printf("Stratum: failed to send difficulty to %s\n", m->address.c_str());

        StratumJob job; bool have;
        { std::lock_guard<std::mutex> lk(gJobMutex); have=gHaveJob; if(have) job=gCurrentJob; }
        if (have) {
            printf("Stratum: sending job %s to %s\n", job.jobId.c_str(), m->address.c_str());
            if (!SendLine(m, json{{"id",nullptr},{"method","mining.notify"},{"params",MakeNotifyParams(job,true)}}))
                printf("Stratum: failed to send job %s to %s\n", job.jobId.c_str(), m->address.c_str());
        } else {
            printf("Stratum: no job available for %s\n", m->address.c_str());
        }
        return;
    }

    if (method == "mining.extranonce.subscribe") { reply(true); return; }

    if (method == "mining.submit") {
        if (!m->authorised) { reply(false,"not authorised"); return; }

        auto& p = req["params"];
        if (p.size() < 4) { reply(false,"bad params"); return; }

        std::string jobId    = p[1].get<std::string>();
        std::string nonceHex = p[2].get<std::string>();

        StratumJob job;
        { std::lock_guard<std::mutex> lk(gJobMutex);
          if (!gHaveJob || gCurrentJob.jobId != jobId) { reply(false,"stale job"); return; }
          job = gCurrentJob; }

        auto nb = FromHex(nonceHex);
        if (nb.size()<4) { reply(false,"bad nonce"); return; }
        unsigned int nNonce; memcpy(&nNonce, nb.data(), 4);

        CBlock& b = job.block;
        b.nNonce = nNonce;
        unsigned char hdr[80];
        memcpy(hdr,    &b.nVersion,      4);
        memcpy(hdr+4,  &b.hashPrevBlock, 32);
        memcpy(hdr+36, &b.hashMerkleRoot,32);
        memcpy(hdr+68, &b.nTime,         4);
        memcpy(hdr+72, &b.nBits,         4);
        memcpy(hdr+76, &b.nNonce,        4);

        uint256 powHash = RandomXPoWHash(hdr, 80);

                // Record share in the current round.
                std::string key = MinerKey(m->address, m->worker);
                { std::lock_guard<std::mutex> lk(gMinerStatsMutex);
                    MinerStats& ms = gMinerStats[key];
                    ms.address = m->address;
                    ms.worker = m->worker;
                    ms.roundShares++;
                    ms.totalShares++;
                    ms.lastSeen = GetTime(); }
        m->sessionShares++;

        if (powHash > job.shareTarget) { reply(true); return; }  // valid share, not a full block

        // Full block
        CBlock* pblock = new CBlock(b);
        bool accepted = false;
        CRITICAL_BLOCK(cs_main)
        {
            if (ProcessBlock(NULL, pblock)) {
                accepted = true;
                printf("Pool: BLOCK FOUND by %s height=%d nNonce=%u\n",
                       m->address.c_str(), job.height, nNonce);
                FoundBlock fb{job.height, GetTime(), m->address, pblock->GetHash()};
                { std::lock_guard<std::mutex> lk(gBlocksMutex);
                  gFoundBlocks.push_front(fb);
                  if (gFoundBlocks.size()>50) gFoundBlocks.pop_back(); }
                { std::lock_guard<std::mutex> lk(gJobMutex); gHaveJob=false; }
            } else {
                delete pblock;
            }
        }
        reply(accepted);
        if (accepted) { QueuePayouts(job.height); RebuildJob(); BroadcastJob(true); }
        return;
    }

    if (method == "mining.get_transactions") { reply(json::array()); return; }
    reply(nullptr, "unknown method");
}

static void MinerThreadFn(void* arg)
{
    Miner* m = (Miner*)arg;
    while (!fShutdown && gPoolRunning) {
        fd_set fds; FD_ZERO(&fds); FD_SET(m->fd, &fds);
        struct timeval tv={1,0};
        if (select((int)m->fd+1, &fds, NULL, NULL, &tv) <= 0) continue;
        char buf[4096]; 
        int n = recv(m->fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n]=0; m->readBuf += buf;
        size_t pos;
        while ((pos = m->readBuf.find('\n')) != std::string::npos) {
            std::string line = m->readBuf.substr(0, pos);
            m->readBuf = m->readBuf.substr(pos+1);
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (!line.empty()) HandleLine(m, line);
        }
    }
    sock_close(m->fd);
    printf("Stratum: miner %s disconnected (%llu shares)\n",
           m->worker.empty()?"unknown":m->worker.c_str(),
           (unsigned long long)m->sessionShares);
    { std::lock_guard<std::mutex> lk(gMinersMutex);
      auto it = std::find(gMiners.begin(), gMiners.end(), m);
      if (it != gMiners.end()) gMiners.erase(it); }
    delete m;
}

static void JobWatcherFn(void*)
{
    int lastHeight = -1;
    while (!fShutdown && gPoolRunning) {
        Sleep(500);
        bool newBlock = false;
        CRITICAL_BLOCK(cs_main)
        { if (nBestHeight != lastHeight && pindexBest) { lastHeight = nBestHeight; newBlock = true; } }
        if (newBlock) {
            FlushMaturePayouts();
            if (RebuildJob()) { printf("Stratum: new job height=%d\n", lastHeight+1); BroadcastJob(true); }
        } else if (!gHaveJob) {
            if (RebuildJob()) BroadcastJob(false);
        }
    }
}

// ---------------------------------------------------------------------------
// Main Stratum accept loop — entry point called from ui.cpp and headless.cpp
// ---------------------------------------------------------------------------
// .btf Stratum listener — miners connect via rendezvous relay using the
// node's .btf address instead of a raw IP:port. Same Stratum protocol,
// same MinerThreadFn handler, works through CGNAT with no port forwarding.
//
// Keeps one listener registered per relay at all times. When a miner
// connects, a replacement listener is spawned immediately so the next
// miner doesn't have to wait.
// ---------------------------------------------------------------------------

struct BtfAcceptCtx { unsigned char pk[32]; unsigned char sk[32]; string relay; };

// Track active listener count per relay to avoid pile-up when relay is slow
static std::map<string,int> gBtfListenerCount;
static std::mutex            gBtfListenerMutex;

static void BtfStratumAcceptOneFn(void* arg)
{
    BtfAcceptCtx* ctx = (BtfAcceptCtx*)arg;
    unsigned char pk[32], sk[32];
    memcpy(pk, ctx->pk, 32); memcpy(sk, ctx->sk, 32);
    string relay = ctx->relay;
    delete ctx;

    auto decrement = [&]() {
        std::lock_guard<std::mutex> lk(gBtfListenerMutex);
        gBtfListenerCount[relay]--;
    };

    size_t colon = relay.rfind(':');
    if (colon == string::npos) { decrement(); return; }
    string strHost = relay.substr(0, colon);
    int nPort = atoi(relay.substr(colon+1).c_str());

    btf::RvSocket rv = btf::RvServiceRegister(strHost.c_str(), (unsigned short)nPort, pk);
    if (fShutdown || !gPoolRunning) { if (rv != btf::RV_INVALID) btf::RvClose(rv); decrement(); return; }
    if (rv == btf::RV_INVALID) { decrement(); return; }

    // Paired: spawn replacement (count transfers to new thread)
    if (!fShutdown && gPoolRunning) {
        BtfAcceptCtx* next = new BtfAcceptCtx();
        memcpy(next->pk, pk, 32); memcpy(next->sk, sk, 32);
        next->relay = relay;
        LaunchThread(BtfStratumAcceptOneFn, next);
    } else {
        decrement();
    }

    btf_socket_t hSocket = btf::BtfServiceWrap(rv, sk);
    if (hSocket == INVALID_SOCKET) return;

    printf("Stratum/.btf: miner connected via %s\n", relay.c_str());
    Miner* m = new Miner(hSocket);
    { std::lock_guard<std::mutex> lk(gMinersMutex); gMiners.push_back(m); }
    LaunchThread(MinerThreadFn, m);
}

static void BtfStratumAcceptFn(void*)
{
    printf("Stratum/.btf: starting \xe2\x80\x94 miners can connect via %s\n",
           BtfLocalAddress().c_str());

    unsigned char pk[32], sk[32];
    while (!BtfGetIdentity(pk, sk)) {
        if (fShutdown || !gPoolRunning) return;
        Sleep(2000);
    }

    while (!fShutdown && gPoolRunning) {
        vector<string> relays = BtfAllRelays();
        if (relays.empty()) { Sleep(5000); continue; }

        // Spawn one listener per relay, only if none already active
        for (const string& relay : relays) {
            std::lock_guard<std::mutex> lk(gBtfListenerMutex);
            if (gBtfListenerCount[relay] == 0) {
                gBtfListenerCount[relay]++;
                BtfAcceptCtx* ctx = new BtfAcceptCtx();
                memcpy(ctx->pk, pk, 32); memcpy(ctx->sk, sk, 32);
                ctx->relay = relay;
                LaunchThread(BtfStratumAcceptOneFn, ctx);
            }
        }
        for (int i = 0; i < 30 && !fShutdown && gPoolRunning; i++) Sleep(1000);
    }
}
void ThreadRPCServer(void*)
{
    gPoolRunning = true;
    printf("Bitflash pool server starting\n");
    printf("  Stratum .btf:       %s  (no port forwarding needed)\n",
           BtfLocalAddress().c_str());
    printf("  Mode:               operator\n");

    if (RebuildJob())
        printf("Stratum: initial job seeded height=%d\n", nBestHeight + 1);

    LaunchThread(JobWatcherFn, NULL);
    LaunchThread(BtfStratumAcceptFn, NULL);
    LaunchThread(ThreadBtfPoolAnnouncer, NULL);

    while (!fShutdown && gPoolRunning)
        Sleep(1000);
    printf("Pool server stopped\n");
}
