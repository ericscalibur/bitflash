// Bitflash GUI — ImGui + GLFW + OpenGL3
// Same source on Linux and Windows. No platform ifdefs.

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "headers_core.h"
#include <string>
#include <vector>
#include <ctime>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include "font_roboto.h"

// ---------------------------------------------------------------------------
// Node API
// ---------------------------------------------------------------------------
extern int64         nTransactionFee;
extern int           fGenerateBitcoins;
extern int           nBestHeight;
extern int           nMineMode;
extern std::string   strParticipantPool;
extern CAddress      addrLocalHost;

void   MainFrameRepaint();
string DateTimeStr(int64 nTime);
bool   SendMoney(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew);
int64  GetBalance();
string PubKeyToAddress(const std::vector<unsigned char>& vchPubKey);
bool   AddressToHash160(const std::string& str, uint160& hash160Ret);
void   ThreadBitcoinMiner(void*);
void   ThreadRPCServer(void*);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static std::string g_myAddress;
static int64       g_balance     = 0;
static int64       g_lastWalletRefresh = 0;
static bool        g_showSend    = false;
static bool        g_showOptions = false;
static bool        g_showAbout   = false;
static bool        g_needRefresh = true;

static char        g_sendAddr[128]        = {};
static char        g_sendAmount[32]       = {};
static std::string g_sendStatus;
static char        g_participantPool[256] = {};
static char        g_poolName[128]        = {};
static char        g_poolFee[32]          = {};
static char        g_poolDash[256]        = {};
static int         g_mineRadio            = 0;

struct TxRow { std::string date, desc; int64 amount; int depth; };
static std::vector<TxRow> g_txRows;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string FmtMoney(int64 n)
{
    bool neg = n < 0; if (neg) n = -n;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%lld.%08lld",
             neg?"-":"", (long long)(n/COIN), (long long)(n%COIN));
    char* dot = strchr(buf, '.');
    if (dot) { char* e = buf+strlen(buf)-1; while(e>dot+2&&*e=='0') *e--='\0'; }
    return buf;
}

static std::string FmtAge(int64 createdAt)
{
    int64 age = GetTime() - createdAt;
    if (age < 0)
        age = 0;
    if (age < 60)
        return strprintf("%llds", (long long)age);
    if (age < 3600)
        return strprintf("%lldm", (long long)(age / 60));
    return strprintf("%lldh", (long long)(age / 3600));
}

static ImVec4 ParticipantStatusColor(const std::string& status)
{
    if (status.find("failed") != std::string::npos ||
        status.find("rejected") != std::string::npos ||
        status.find("stopped") != std::string::npos)
        return ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
    if (status.find("hashing") != std::string::npos ||
        status.find("accepted") != std::string::npos ||
        status.find("connected") != std::string::npos)
        return ImVec4(0.35f, 1.0f, 0.45f, 1.0f);
    if (status.find("asking") != std::string::npos ||
        status.find("authoriz") != std::string::npos ||
        status.find("subscrib") != std::string::npos)
        return ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
    return ImVec4(0.55f, 0.80f, 1.0f, 1.0f);
}

static std::string PoolLabel(const BtfPoolAnnouncement& ann)
{
    std::ostringstream o;
    o << ann.poolName << " - " << std::fixed << std::setprecision(2) << ann.feePercent << "%";
    if (ann.connectedMiners > 0)
        o << " - " << ann.connectedMiners << " miners";
    return o.str();
}

static void RefreshWallet()
{
    std::vector<unsigned char> vchPubKey;
    if (CWalletDB("r").ReadDefaultKey(vchPubKey))
        g_myAddress = PubKeyToAddress(vchPubKey);
    g_balance = GetBalance();

    g_txRows.clear();
    TRY_CRITICAL_BLOCK(cs_mapWallet)
    {
        std::vector<std::pair<int64,uint256>> vs;
        for (auto& kv : mapWallet) vs.push_back({kv.second.GetTxTime(), kv.first});
        std::sort(vs.rbegin(), vs.rend());
        for (auto& sv : vs) {
            auto mi = mapWallet.find(sv.second);
            if (mi == mapWallet.end()) continue;
            CWalletTx& wtx = mi->second;
            int64 net = wtx.GetCredit() - wtx.GetDebit();
            int depth = wtx.GetDepthInMainChain();
            std::string desc;
            if (wtx.IsCoinBase())        desc = depth<1 ? "Generated (unconfirmed)" : "Generated";
            else if (wtx.GetDebit() > 0) desc = "Sent";
            else                          desc = "Received";
            g_txRows.push_back({DateTimeStr(wtx.GetTxTime()), desc, net, depth});
        }
    }
    g_lastWalletRefresh = GetTime();
}

void MainFrameRepaint() { g_needRefresh = true; }

string DateTimeStr(int64 nTime)
{
    time_t t = (time_t)nTime;
    struct tm* p = localtime(&t);
    if (!p) return "";
    char buf[32]; strftime(buf, sizeof(buf), "%m/%d/%y %H:%M", p);
    return buf;
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
static void DrawMainWindow()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12,12));
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.92f);
    ImGui::PopStyleVar();

    // ---- Menu bar ----
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(glfwGetCurrentContext(), true);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Options"))  g_showOptions = true;
        if (ImGui::MenuItem("About"))    g_showAbout   = true;
        ImGui::EndMenuBar();
    }

    // ---- Address bar ----
    ImGui::Spacing();
    ImGui::TextDisabled("Address:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-80);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.14f,0.14f,0.14f,1));
    ImGui::InputText("##addr", (char*)g_myAddress.c_str(), g_myAddress.size()+1,
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Copy##a")) ImGui::SetClipboardText(g_myAddress.c_str());

    // ---- Balance on its own line ----
    ImGui::TextDisabled("Balance:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f,1.0f,0.35f,1.0f));
    ImGui::Text("%s BTF", FmtMoney(g_balance).c_str());
    ImGui::PopStyleColor();

    // ---- Toolbar ----
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.40f,0.15f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.55f,0.20f,1));
    if (ImGui::Button("  Send Coins  ")) g_showSend = true;
    ImGui::PopStyleColor(2);

    // Mining status indicator
    ImGui::SameLine(0, 20);
    if (fGenerateBitcoins) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.6f,0.0f,1.0f));
        const char* modeLabel = nMineMode==MINE_OPERATOR ? "Mining (operator)" :
                                nMineMode==MINE_PARTICIPANT ? "Mining (pool)" : "Mining";
        ImGui::Text("⬤ %s", modeLabel);
        ImGui::PopStyleColor();
        if (nMineMode == MINE_PARTICIPANT) {
            std::string mineStatus = GetParticipantMiningStatus();
            if (!mineStatus.empty()) {
                ImGui::SameLine(0, 12);
                ImGui::PushStyleColor(ImGuiCol_Text, ParticipantStatusColor(mineStatus));
                ImGui::Text("[%s]", mineStatus.c_str());
                ImGui::PopStyleColor();
            }
        }
    } else {
        ImGui::TextDisabled("○ Not mining");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Live telemetry strip ----
    {
        const char* modeName = nMineMode==MINE_OPERATOR ? "Operator" :
                               nMineMode==MINE_PARTICIPANT ? "Participant" : "Solo";
        int64 age = g_lastWalletRefresh > 0 ? (GetTime() - g_lastWalletRefresh) : -1;
        if (age < 0) age = 0;

        ImGui::Text("Mode: %s", modeName);
        ImGui::SameLine(0, 16);
        ImGui::Text("Wallet refresh: %llds ago", (long long)age);

        if (nMineMode == MINE_PARTICIPANT) {
            uint64 sent = 0, accepted = 0;
            double hashRate = 0.0;
            std::string mineStatus = GetParticipantMiningStatus();
            GetParticipantMiningStats(sent, accepted, hashRate);
            if (sent > 0 || accepted > 0 || hashRate > 0.0) {
                double acceptPct = sent ? (100.0 * (double)accepted / (double)sent) : 0.0;
                ImGui::TextDisabled("Your mining: sent %llu, accepted %llu (%.1f%%), %.2f H/s",
                                    (unsigned long long)sent,
                                    (unsigned long long)accepted,
                                    acceptPct,
                                    hashRate);
            } else if (!mineStatus.empty()) {
                ImGui::TextDisabled("Your mining: %s", mineStatus.c_str());
            } else {
                ImGui::TextDisabled("Your mining: asking pool for work");
            }
        } else if (nMineMode == MINE_OPERATOR) {
            int miners = 0, blocksFound = 0;
            uint64 roundShares = 0;
            GetPoolOperatorStats(miners, blocksFound, roundShares);
            if (miners > 0 || blocksFound > 0 || roundShares > 0)
                ImGui::TextDisabled("Pool runtime: %d authorized miners, %llu round shares, %d blocks found", miners, (unsigned long long)roundShares, blocksFound);
            else
                ImGui::TextDisabled("Pool runtime: waiting for miners");
        } else {
            ImGui::TextDisabled("Solo runtime: local wallet mining telemetry");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ---- Transaction list ----
    float listHeight = io.DisplaySize.y * 0.26f;
    if (listHeight < 190.0f) listHeight = 190.0f;
    if (listHeight > 260.0f) listHeight = 260.0f;
    if (ImGui::BeginTable("txlist", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
        ImVec2(0, listHeight)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Status",      ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Date",        ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Amount",      ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableHeadersRow();

        for (auto& row : g_txRows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (row.depth < 1)
                ImGui::TextColored(ImVec4(1,0.6f,0,1), "Unconfirmed");
            else
                ImGui::Text("%d blocks deep", row.depth);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", row.date.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.desc.c_str());
            ImGui::TableSetColumnIndex(3);
            if (row.amount >= 0)
                ImGui::TextColored(ImVec4(0.35f,1,0.35f,1), "+%s BTF", FmtMoney(row.amount).c_str());
            else
                ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),   "%s BTF",  FmtMoney(row.amount).c_str());
        }
        ImGui::EndTable();
    }

    std::vector<BtfPoolAnnouncement> pools;
    BtfGetPoolAnnouncements(pools);

    if (nMineMode == MINE_PARTICIPANT) {
        ImGui::Spacing();
        ImGui::SeparatorText("Participant Mining");
        ImGui::Text("Selected pool:");
        if (strParticipantPool.empty())
            ImGui::TextDisabled("No pool selected yet");
        else
            ImGui::TextWrapped("%s", strParticipantPool.c_str());

        const BtfPoolAnnouncement* selected = NULL;
        for (size_t i = 0; i < pools.size(); i++)
            if (pools[i].btfAddress == strParticipantPool) { selected = &pools[i]; break; }

        if (selected) {
            ImGui::Text("Pool name: %s", selected->poolName.c_str());
            ImGui::Text("Fee: %.2f%%", selected->feePercent);
            ImGui::Text("Live stats: %d miners, %d blocks", selected->connectedMiners, selected->blocksFound);
            if (!selected->dashboardUrl.empty())
                ImGui::TextWrapped("Dashboard: %s", selected->dashboardUrl.c_str());
        } else if (!strParticipantPool.empty()) {
            ImGui::TextDisabled("No fresh announcement found for this pool yet");
        }

        if (!pools.empty()) {
            ImGui::Spacing();
            ImGui::SeparatorText("Live Pools");
            if (ImGui::BeginTable("livepools", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Pool", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Fee", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Stats", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Dashboard", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < pools.size(); i++) {
                    const BtfPoolAnnouncement& ann = pools[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(ann.poolName.c_str());
                    ImGui::TextDisabled("%s", ann.btfAddress.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.2f%%", ann.feePercent);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d miners, %d blocks, %s old", ann.connectedMiners, ann.blocksFound, FmtAge(ann.createdAt).c_str());
                    ImGui::TableSetColumnIndex(3);
                    if (!ann.dashboardUrl.empty())
                        ImGui::TextWrapped("%s", ann.dashboardUrl.c_str());
                    else
                        ImGui::TextDisabled("-");
                    ImGui::TableSetColumnIndex(4);
                    if (ImGui::SmallButton((std::string("Use##") + ann.btfAddress).c_str())) {
                        strncpy(g_participantPool, ann.btfAddress.c_str(), sizeof(g_participantPool)-1);
                        strParticipantPool = ann.btfAddress;
                        CWalletDB().WriteSetting("strParticipantPool", strParticipantPool);
                        g_needRefresh = true;
                    }
                }
                ImGui::EndTable();
            }
        }
    }

    if (nMineMode == MINE_OPERATOR) {
        ImGui::Spacing();
        ImGui::SeparatorText("Operator Overview");
        ImGui::Text("Pool .btf address:");
        std::string opAddr = BtfLocalAddress();
        if (opAddr.empty())
            ImGui::TextDisabled("Identity not ready yet");
        else
            ImGui::TextWrapped("%s", opAddr.c_str());

        std::vector<PendingPayoutView> owed;
        GetPendingPayouts(owed);
        ImGui::Spacing();
        ImGui::SeparatorText("Pending Pool Payouts (you owe miners)");
        if (owed.empty()) {
            ImGui::TextDisabled("No pending pool payouts");
        } else if (ImGui::BeginTable("owedpayouts", 4,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Matures At", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Blocks Left", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Recipients", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const PendingPayoutView& row : owed) {
                int remaining = std::max(0, row.matureAtHeight - nBestHeight);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", row.matureAtHeight);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", remaining);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", row.recipients);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s BTF", FmtMoney(row.totalAmount).c_str());
            }
            ImGui::EndTable();
        }

        std::vector<PoolWorkerStatView> workers;
        GetPoolWorkerStats(workers);
        ImGui::Spacing();
        ImGui::SeparatorText("Current Round Workers");
        if (workers.empty()) {
            ImGui::TextDisabled("No active workers in this round");
        } else if (ImGui::BeginTable("workers", 4,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Payout Addr", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Round Shares", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Last Seen", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();
            for (const PoolWorkerStatView& row : workers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.address.empty() ? "unknown" : row.address.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", row.worker.empty() ? "worker" : row.worker.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)row.roundShares);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s", FmtAge(row.lastSeen).c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---- Status bar ----
    {
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4,2));

        int peers = (int)vNodes.size();
        if (nMineMode == MINE_PARTICIPANT) {
            uint64 sent = 0, accepted = 0;
            double hashRate = 0.0;
            std::string mineStatus = GetParticipantMiningStatus();
            GetParticipantMiningStats(sent, accepted, hashRate);
            if (sent > 0 || accepted > 0 || hashRate > 0.0)
                ImGui::TextDisabled("Peers %d  height %d  shares sent %llu  accepted %llu  hash rate %.2f H/s",
                                    peers,
                                    nBestHeight,
                                    (unsigned long long)sent,
                                    (unsigned long long)accepted,
                                    hashRate);
            else if (!mineStatus.empty())
                ImGui::TextDisabled("Peers %d  height %d  %s", peers, nBestHeight, mineStatus.c_str());
            else
                ImGui::TextDisabled("Peers %d  height %d  asking pool for work", peers, nBestHeight);
        } else if (nMineMode == MINE_OPERATOR) {
            int miners = 0, blocksFound = 0;
            uint64 roundShares = 0;
            GetPoolOperatorStats(miners, blocksFound, roundShares);
            if (miners > 0 || blocksFound > 0 || roundShares > 0)
                ImGui::TextDisabled("Peers %d  height %d  authorized miners %d  round shares %llu  blocks found %d",
                                    peers, nBestHeight, miners, (unsigned long long)roundShares, blocksFound);
            else
                ImGui::TextDisabled("Peers %d  height %d  waiting for miners", peers, nBestHeight);
        } else {
            ImGui::TextDisabled("Peers %d  height %d", peers, nBestHeight);
        }
        ImGui::PopStyleVar();
    }

    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Send dialog
// ---------------------------------------------------------------------------
static void DrawSendDialog()
{
    if (!g_showSend) return;
    ImGui::SetNextWindowSize(ImVec2(500, 180), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(.5f,.5f));
    if (ImGui::Begin("Send Coins", &g_showSend,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::Text("Recipient address:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##sa", g_sendAddr, sizeof(g_sendAddr));

        ImGui::Text("Amount (BTF):");
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("##sm", g_sendAmount, sizeof(g_sendAmount));

        if (!g_sendStatus.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.4f,.4f,1), "%s", g_sendStatus.c_str());
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("Send", ImVec2(90,0))) {
            uint160 h; int64 nv = 0;
            if (!AddressToHash160(g_sendAddr, h))          g_sendStatus = "Invalid address.";
            else if (!ParseMoney(g_sendAmount,nv)||nv<=0)   g_sendStatus = "Invalid amount.";
            else {
                CScript s; s << OP_DUP << OP_HASH160 << h << OP_EQUALVERIFY << OP_CHECKSIG;
                CWalletTx wtx;
                if (SendMoney(s, nv, wtx)) {
                    g_showSend=false; g_sendStatus="";
                    memset(g_sendAddr,0,sizeof(g_sendAddr));
                    memset(g_sendAmount,0,sizeof(g_sendAmount));
                    g_needRefresh = true;
                } else g_sendStatus = "Transaction failed.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90,0))) { g_showSend=false; g_sendStatus=""; }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Options dialog
// ---------------------------------------------------------------------------
static void DrawOptionsDialog()
{
    if (!g_showOptions) return;

    // Always sync from current globals when dialog opens
    static bool wasOpen = false;
    if (!wasOpen) {
        g_mineRadio = nMineMode;
        strncpy(g_participantPool, strParticipantPool.c_str(), sizeof(g_participantPool)-1);
        strncpy(g_poolName, strPoolName.c_str(), sizeof(g_poolName)-1);
        snprintf(g_poolFee, sizeof(g_poolFee), "%.2f", dPoolFeePercent);
        strncpy(g_poolDash, strPoolDashboardUrl.c_str(), sizeof(g_poolDash)-1);
    }
    wasOpen = true;
    ImGui::SetNextWindowSize(ImVec2(500, 310), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(.5f,.5f));
    if (ImGui::Begin("Options", &g_showOptions,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
    {
        // Transaction fee
        ImGui::SeparatorText("Transaction Fee");
        static char feeStr[32] = {};
        if (feeStr[0]=='\0') snprintf(feeStr,sizeof(feeStr),"%s",FmtMoney(nTransactionFee).c_str());
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("BTF per transaction##fee", feeStr, sizeof(feeStr));

        // Mining
        ImGui::SeparatorText("Mining Mode");
        ImGui::RadioButton("Solo  — rewards go to your wallet",       &g_mineRadio, MINE_SOLO);
        ImGui::RadioButton("Operator  — run a pool for other miners", &g_mineRadio, MINE_OPERATOR);
        ImGui::RadioButton("Participant  — mine to someone's pool",   &g_mineRadio, MINE_PARTICIPANT);

        if (g_mineRadio == MINE_OPERATOR) {
            ImGui::Spacing();
            ImGui::SeparatorText("Pool Announcement");
            ImGui::Text("Pool name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##poolname", g_poolName, sizeof(g_poolName));
            ImGui::Text("Fee %%:");
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("##poolfee", g_poolFee, sizeof(g_poolFee));
            ImGui::Text("Dashboard URL:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##pooldash", g_poolDash, sizeof(g_poolDash));
        }

        if (g_mineRadio == MINE_PARTICIPANT) {
            ImGui::Spacing();
            ImGui::SeparatorText("Pool Selection");
            ImGui::TextDisabled("Pick from live discovered pools. Manual entry is disabled here.");

            std::vector<BtfPoolAnnouncement> pools;
            BtfGetPoolAnnouncements(pools);
            if (!pools.empty()) {
                std::string current = "Select a live pool";
                for (size_t i = 0; i < pools.size(); i++) {
                    if (pools[i].btfAddress == g_participantPool) {
                        current = PoolLabel(pools[i]);
                        break;
                    }
                }
                if (ImGui::BeginCombo("Discovered pools", current.c_str())) {
                    for (size_t i = 0; i < pools.size(); i++) {
                        const BtfPoolAnnouncement& ann = pools[i];
                        std::string label = PoolLabel(ann);
                        bool selected = ann.btfAddress == g_participantPool;
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            strncpy(g_participantPool, ann.btfAddress.c_str(), sizeof(g_participantPool)-1);
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("No live pools discovered yet");
            }
        }

        ImGui::Spacing();
        const char* genLabel = fGenerateBitcoins ? "Stop Mining" : "Start Mining";
        if (ImGui::Button(genLabel, ImVec2(130,0))) {
            fGenerateBitcoins = fGenerateBitcoins ? 0 : 1;
            if (fGenerateBitcoins) _beginthread(ThreadBitcoinMiner, 0, NULL);
            CWalletDB().WriteSetting("fGenerateBitcoins", fGenerateBitcoins);
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(90,0))) {
            int64 fee=0;
            if (ParseMoney(feeStr,fee)) { nTransactionFee=fee; CWalletDB().WriteSetting("nTransactionFee",nTransactionFee); }

            bool changed = (g_mineRadio != nMineMode);
            nMineMode = g_mineRadio;
            strParticipantPool = g_participantPool;
            strPoolName = g_poolName[0] ? g_poolName : "Bitflash Pool";
            strPoolDashboardUrl = g_poolDash;
            dPoolFeePercent = atof(g_poolFee);
            CWalletDB().WriteSetting("nMineMode", nMineMode);
            CWalletDB().WriteSetting("strParticipantPool", strParticipantPool);
            CWalletDB().WriteSetting("strPoolName", strPoolName);
            CWalletDB().WriteSetting("strPoolDashboardUrl", strPoolDashboardUrl);
            CWalletDB().WriteSetting("dPoolFeePercent", dPoolFeePercent);

            if (nMineMode == MINE_OPERATOR) {
                if (!gPoolServerRunning) {
                    gPoolServerRunning = true;
                    _beginthread(ThreadRPCServer, 0, NULL);
                } else if (!gPoolRunning) {
                    gPoolRunning = true;
                }
            } else {
                gPoolRunning = false;
            }
            g_showOptions = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90,0))) g_showOptions = false;
    }
    if (!g_showOptions) wasOpen = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// About dialog
// ---------------------------------------------------------------------------
static void DrawAboutDialog()
{
    if (!g_showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(380, 170), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(.5f,.5f));
    if (ImGui::Begin("About Bitflash", &g_showAbout,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextColored(ImVec4(1,.6f,0,1), "Bitflash  BTF  v1.1.0");
        ImGui::Spacing();
        ImGui::TextWrapped("CPU-only cryptocurrency. RandomX proof of work. Anonymous .btf addressing over Nostr. No premine.");
        ImGui::Spacing();
        ImGui::TextDisabled("Based on Bitcoin 0.1.0 (Satoshi Nakamoto, 2009)");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(90,0))) g_showAbout = false;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int RunGUI(int argc, char* argv[])
{
    if (!glfwInit()) {
        printf("GUI ERROR: glfwInit() failed -- no display or GL available. Run headless with -nogui.\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(960, 640, "Bitflash", nullptr, nullptr);
    if (!window) {
        printf("GUI ERROR: could not create an OpenGL 3.3 window.\n");
        printf("           Your GPU/driver may not support OpenGL 3.3 core -- common over Remote Desktop, in VMs, or on old GPUs.\n");
        printf("           Fixes: update graphics drivers, use a software OpenGL (Mesa), or run headless with -nogui.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // Load Roboto at 16px — clean, readable on all platforms
    io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoMedium_compressed_data, RobotoMedium_compressed_size, 16.0f);

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.FrameRounding = s.PopupRounding = 4.0f;
    s.ItemSpacing    = ImVec2(10, 7);
    s.FramePadding   = ImVec2(8, 5);
    s.WindowPadding  = ImVec2(12, 12);
    // Orange accent
    s.Colors[ImGuiCol_TitleBgActive]  = ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_CheckMark]      = ImVec4(1.0f,.60f,.00f,1);
    s.Colors[ImGuiCol_SliderGrab]     = ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_Button]         = ImVec4(.25f,.25f,.25f,1);
    s.Colors[ImGuiCol_ButtonHovered]  = ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_ButtonActive]   = ImVec4(.60f,.30f,.00f,1);
    s.Colors[ImGuiCol_Header]         = ImVec4(.80f,.45f,.00f,.40f);
    s.Colors[ImGuiCol_HeaderHovered]  = ImVec4(.80f,.45f,.20f,.80f);
    s.Colors[ImGuiCol_Tab]            = ImVec4(.20f,.20f,.20f,1);
    s.Colors[ImGuiCol_TabHovered]     = ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_TabActive]      = ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_SeparatorHovered]=ImVec4(.80f,.45f,.00f,1);
    s.Colors[ImGuiCol_FrameBg]        = ImVec4(.14f,.14f,.14f,1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.20f,.20f,.20f,1);
    s.Colors[ImGuiCol_TableHeaderBg]  = ImVec4(.18f,.18f,.18f,1);
    s.Colors[ImGuiCol_TableRowBg]     = ImVec4(.11f,.11f,.11f,1);
    s.Colors[ImGuiCol_TableRowBgAlt]  = ImVec4(.14f,.14f,.14f,1);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    g_mineRadio = nMineMode;
    strncpy(g_participantPool, strParticipantPool.c_str(), sizeof(g_participantPool)-1);

    int frame = 0;
    while (!glfwWindowShouldClose(window) && !fShutdown)
    {
        glfwPollEvents();
        if (g_needRefresh || (frame % 120 == 0)) { RefreshWallet(); g_needRefresh = false; }
        frame++;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawMainWindow();
        DrawSendDialog();
        DrawOptionsDialog();
        DrawAboutDialog();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(.08f,.08f,.08f,1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    fShutdown = true;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
