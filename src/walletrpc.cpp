// Bitflash wallet RPC + web GUI.
//
// The node had no way to show a balance or send a payment except through the
// ImGui window, which does not build on macOS and needs libGL besides. This
// exposes the wallet functions the GUI already calls -- GetBalance(),
// GenerateNewKey(), SendMoney() -- over a small HTTP/JSON server, and serves a
// browser UI that talks to it. No new crypto: signing is still CreateTransaction
// /SignSignature in main.cpp, which is the code the GUI has always used.
//
// Security, since this endpoint can spend money:
//   * Binds 127.0.0.1 by default, so it is not reachable off the machine.
//     /walletrpcbind=ADDR overrides this, which is REQUIRED under Docker:
//     published ports (-p) forward to the container's eth0, never to its
//     loopback, so a loopback bind inside a container cannot be reached at all.
//     The correct container pattern is /walletrpcbind=0.0.0.0 together with
//     -p 127.0.0.1:8901:8901, which confines exposure to the host's loopback
//     while leaving the container reachable from Docker's bridge.
//   * Every /api/ call must carry X-BTF-Token, matching a 32-byte random token
//     generated at startup and written to <datadir>/walletrpc.token mode 0600.
//     A custom header cannot be sent cross-origin without a CORS preflight, and
//     no CORS headers are ever returned, so a hostile web page in the same
//     browser can neither preflight successfully nor read the token out of the
//     served page. That is what stops a drive-by from draining the wallet.
//   * Requests carrying an Origin header that is not our own are refused, so
//     even a simple-request forgery attempt dies before reaching the wallet.
//   * Off by default. Requires /walletrpc on the command line.

// nlohmann/json.hpp must precede the project headers: util.h does
// "#define snprintf my_snprintf", which breaks any std header afterwards that
// refers to std::snprintf. rpc.cpp orders its includes the same way.
#include <nlohmann/json.hpp>

#include "headers_core.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#endif

using json = nlohmann::json;

extern int    nWalletRpcPort;  // main_gui.cpp, set by /walletrpcport=N
extern bool   fWalletRpc;      // main_gui.cpp, set by /walletrpc
extern string strWalletRpcBind; // main_gui.cpp, set by /walletrpcbind=ADDR

static string g_token;

// The page is served from this node, so it is same-origin with the API and can
// hold the token. __TOKEN__ is substituted at serve time.
static const char* pszWalletHtml = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Bitflash Wallet</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#0f1115;--card:#171a21;--line:#252a34;--fg:#e6e8eb;--mut:#8b93a1;
--ok:#2ea44f;--warn:#d29922;--err:#f85149;--acc:#4b9fff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif}
.wrap{max-width:760px;margin:0 auto;padding:28px 20px 60px}
h1{font-size:19px;font-weight:600;margin:0 0 2px}
.sub{color:var(--mut);font-size:13px;margin-bottom:22px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;
padding:18px 20px;margin-bottom:16px}
.bal{font-size:34px;font-weight:600;letter-spacing:-.5px}
.bal span{font-size:16px;color:var(--mut);font-weight:400}
.row{display:flex;gap:26px;flex-wrap:wrap;margin-top:14px}
.stat{font-size:13px}
.stat b{display:block;font-size:17px;font-weight:600;margin-top:2px}
.mut{color:var(--mut)}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px}
label{display:block;font-size:12.5px;color:var(--mut);margin:12px 0 5px}
input{width:100%;padding:10px 12px;background:#0d0f13;color:var(--fg);
border:1px solid var(--line);border-radius:7px;font-size:14px;font-family:inherit}
input:focus{outline:0;border-color:var(--acc)}
button{padding:10px 18px;border-radius:7px;border:1px solid var(--line);
background:#222732;color:var(--fg);font-size:14px;cursor:pointer;font-family:inherit}
button:hover{background:#2a303d}
button.p{background:var(--ok);border-color:var(--ok);color:#fff;font-weight:500}
button.p:hover{background:#3cb85f}
button:disabled{opacity:.5;cursor:default}
.addr{background:#0d0f13;border:1px solid var(--line);border-radius:7px;
padding:11px 13px;word-break:break-all;margin-top:6px}
table{width:100%;border-collapse:collapse;margin-top:6px}
th{text-align:left;font-size:11.5px;text-transform:uppercase;letter-spacing:.4px;
color:var(--mut);font-weight:500;padding:0 0 8px}
td{padding:9px 0;border-top:1px solid var(--line);font-size:13.5px;vertical-align:top}
.amt{text-align:right;font-family:ui-monospace,Menlo,monospace;white-space:nowrap}
.pos{color:var(--ok)}.neg{color:var(--err)}
.pill{display:inline-block;font-size:11px;padding:2px 7px;border-radius:20px;
background:#252a34;color:var(--mut)}
.pill.m{background:#3a2f0c;color:var(--warn)}
.pill.o{background:#3d1418;color:var(--err)}
.bad{color:var(--err)}
.msg{padding:11px 13px;border-radius:7px;margin-top:12px;font-size:13.5px;display:none}
.msg.s{display:block;background:#0d2b14;border:1px solid var(--ok);color:#7ee495}
.msg.e{display:block;background:#2d1113;border:1px solid var(--err);color:#ff9b95}
.off{opacity:.45}
</style></head><body><div class="wrap">
<h1>Bitflash Wallet</h1>
<div class="sub" id="sub">connecting…</div>

<div class="card">
  <div class="mut" style="font-size:12.5px">Spendable</div>
  <div class="bal"><span id="bal">–</span> <span>BTF</span></div>
  <div class="row">
    <div class="stat mut">Maturing<b class="mut" id="imm">–</b></div>
    <div class="stat mut">Blocks mined<b class="mut" id="mined">–</b></div>
    <div class="stat mut">Hashrate<b class="mut" id="hr">–</b></div>
    <div class="stat mut">Orphaned<b class="mut" id="orph">–</b></div>
    <div class="stat mut">Height<b class="mut" id="h">–</b></div>
    <div class="stat mut">Peers<b class="mut" id="p">–</b></div>
  </div>
</div>

<div class="card">
  <b style="font-size:14px">Receive</b>
  <div class="mut" style="font-size:12.5px;margin-top:3px">
    A fresh address. Every one you generate stays spendable by this wallet.</div>
  <div class="addr mono" id="ra">–</div>
  <div style="margin-top:12px;display:flex;gap:9px">
    <button onclick="newAddr()">New address</button>
    <button onclick="copyAddr()">Copy</button>
  </div>
</div>

<div class="card">
  <b style="font-size:14px">Send</b>
  <label>To address</label>
  <input id="to" class="mono" placeholder="B…" autocomplete="off">
  <label>Amount (BTF)</label>
  <input id="amt" placeholder="0.00" autocomplete="off">
  <div style="margin-top:15px"><button class="p" id="sb" onclick="send()">Send</button></div>
  <div class="msg" id="sm"></div>
</div>

<div class="card">
  <b style="font-size:14px">History</b>
  <table><thead><tr><th>Type</th><th>Transaction</th><th>When</th>
  <th class="amt">Amount</th></tr></thead><tbody id="tb">
  <tr><td colspan="4" class="mut">loading…</td></tr></tbody></table>
</div>
</div><script>
const TOKEN="__TOKEN__";
async function api(m,b){
  const r=await fetch("/api/"+m,{method:"POST",
    headers:{"X-BTF-Token":TOKEN,"Content-Type":"application/json"},
    body:JSON.stringify(b||{})});
  const j=await r.json();
  if(!r.ok||j.error)throw new Error(j.error||("HTTP "+r.status));
  return j;
}
const f=n=>Number(n).toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:8});
const hr=n=>!n?"measuring…":(n>=1000?(n/1000).toFixed(2)+" kH/s":n.toFixed(0)+" H/s");
function ago(t){if(!t)return"–";const s=Math.floor(Date.now()/1000)-t;
  if(s<60)return s+"s ago";if(s<3600)return Math.floor(s/60)+"m ago";
  if(s<86400)return Math.floor(s/3600)+"h ago";return Math.floor(s/86400)+"d ago";}
async function refresh(){
  try{
    const i=await api("getinfo");
    document.getElementById("bal").textContent=f(i.spendable);
    document.getElementById("imm").textContent=f(i.maturing)+" BTF";
    document.getElementById("mined").textContent=i.blocks_mined;
    document.getElementById("hr").textContent=i.mining?hr(i.hashrate):"off";
    const oe=document.getElementById("orph");
    oe.textContent=i.blocks_orphaned+(i.blocks_orphaned?" ("+f(i.orphaned)+")":"");
    oe.className=i.blocks_orphaned>0?"bad":"mut";
    document.getElementById("h").textContent=i.height;
    document.getElementById("p").textContent=i.peers;
    document.getElementById("sub").textContent=
      (i.mining?"mining "+hr(i.hashrate)+" · ":"")+i.peers+" peers · height "+i.height;
    document.body.classList.remove("off");
    const t=await api("listtransactions");
    const tb=document.getElementById("tb");
    if(!t.txs.length){tb.innerHTML='<tr><td colspan="4" class="mut">Nothing yet.</td></tr>';return}
    tb.innerHTML=t.txs.map(x=>{
      const pos=x.amount>=0 && !x.orphaned;
      const pill=x.type==="mined"
        ? (x.orphaned
            ? '<span class="pill o">orphaned</span>'
            : '<span class="pill'+(x.maturing?' m':'')+'">'+(x.maturing?"maturing":"mined")+'</span>')
        : '<span class="pill">'+x.type+'</span>';
      return '<tr><td>'+pill+'</td><td class="mono">'+x.txid.slice(0,20)+'…<br>'
        +'<span class="mut" style="font-size:11.5px">'+x.confirmations+' conf</span></td>'
        +'<td class="mut">'+ago(x.time)+'</td>'
        +'<td class="amt '+(pos?"pos":"neg")+'">'+(pos?"+":"")+f(x.amount)+'</td></tr>';
    }).join("");
  }catch(e){
    document.getElementById("sub").textContent="node unreachable — "+e.message;
    document.body.classList.add("off");
  }
}
async function newAddr(){
  try{const r=await api("getnewaddress");document.getElementById("ra").textContent=r.address}
  catch(e){document.getElementById("ra").textContent="error: "+e.message}
}
function copyAddr(){
  const a=document.getElementById("ra").textContent;
  if(a&&a!=="–")navigator.clipboard.writeText(a);
}
function msg(el,txt,ok){el.className="msg "+(ok?"s":"e");el.textContent=txt}
async function send(){
  const to=document.getElementById("to").value.trim();
  const amt=document.getElementById("amt").value.trim();
  const sm=document.getElementById("sm"),sb=document.getElementById("sb");
  if(!to||!amt){msg(sm,"Enter an address and an amount.",false);return}
  if(!confirm("Send "+amt+" BTF to\n"+to+"\n\nThis cannot be undone."))return;
  sb.disabled=true;sb.textContent="Sending…";
  try{
    const r=await api("sendtoaddress",{address:to,amount:amt});
    msg(sm,"Sent. txid "+r.txid.slice(0,24)+"…",true);
    document.getElementById("to").value="";document.getElementById("amt").value="";
    refresh();
  }catch(e){msg(sm,e.message,false)}
  sb.disabled=false;sb.textContent="Send";
}
newAddr();refresh();setInterval(refresh,15000);
</script></body></html>)HTML";


// ------------------------------------------------------------------ helpers
static string TokenPath()
{
    return GetAppDir() + "/walletrpc.token";
}

static void MakeToken()
{
    unsigned char buf[32];
    RAND_bytes(buf, sizeof(buf));
    static const char* hex = "0123456789abcdef";
    g_token.clear();
    for (unsigned int i = 0; i < sizeof(buf); i++)
    {
        g_token += hex[buf[i] >> 4];
        g_token += hex[buf[i] & 15];
    }
    string path = TokenPath();
    FILE* f = fopen(path.c_str(), "w");
    if (f)
    {
        fprintf(f, "%s\n", g_token.c_str());
        fclose(f);
#ifndef _WIN32
        chmod(path.c_str(), 0600);   // the token is spend authority
#endif
    }
}

// Is this wallet transaction's block on the main chain?
//   1 = yes,  0 = block is known but NOT on the main chain (orphaned/stale),
//  -1 = no block recorded yet, so we cannot tell.
//
// mapBlockIndex is read without holding cs_main, matching what
// CMerkleTx::GetDepthInMainChain() beside it already does. Locking in one of
// the two and not the other would introduce an inconsistent order rather than
// remove a race.
static int WalletTxChainStatus(const CWalletTx* p)
{
    if (p->hashBlock == 0 || p->nIndex == -1)
        return -1;
    map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(p->hashBlock);
    if (mi == mapBlockIndex.end() || !(*mi).second)
        return -1;
    return (*mi).second->IsInMainChain() ? 1 : 0;
}

// Wallet snapshot, splitting mined coins three ways.
//
// Mature and immature are reported apart because CWalletTx::GetCredit()
// deliberately values immature coinbase at 0. Orphaned coinbase has to be
// separated too, and that is easy to get wrong: an orphaned block has
// GetDepthInMainChain() == 0, so GetBlocksToMaturity() returns the full
// COINBASE_MATURITY and the coins look like they are merely "maturing" -- and
// keep looking that way forever, because the depth never grows. Counting them
// as pending income overstates the balance permanently. They are counted here
// only if the block is genuinely absent from the main chain, which is a
// different question from how deep it is.
static void WalletTotals(int64& nMature, int64& nImmature, int64& nOrphaned,
                         int& nMined, int& nMinedImmature, int& nMinedOrphaned)
{
    nMature = GetBalance();
    nImmature = 0; nOrphaned = 0;
    nMined = 0; nMinedImmature = 0; nMinedOrphaned = 0;
    CRITICAL_BLOCK(cs_mapWallet)
    {
        for (map<uint256, CWalletTx>::iterator it = mapWallet.begin();
             it != mapWallet.end(); ++it)
        {
            CWalletTx* p = &(*it).second;
            if (!p->IsCoinBase())
                continue;
            nMined++;
            if (WalletTxChainStatus(p) == 0)
            {
                nMinedOrphaned++;
                nOrphaned += p->CTransaction::GetCredit();
            }
            else if (p->GetBlocksToMaturity() > 0)
            {
                nMinedImmature++;
                nImmature += p->CTransaction::GetCredit();
            }
        }
    }
}

static string FormatAmount(int64 n)
{
    // FormatMoney gives a display string; the GUI parses numbers, so emit a
    // plain decimal with no thousands separators.
    char buf[64];
    my_snprintf(buf, sizeof(buf), "%.8f", (double)n / (double)COIN);
    return string(buf);
}


// ------------------------------------------------------------------ methods
// Parse a BTF amount at the full eight decimal places the chain supports.
//
// util.cpp's ParseMoney() is the original 2009 "dollars and cents" parser: it
// demands exactly two digits after the point and caps them at 99. So "1.0" is
// rejected, "1.5" is rejected, and 0.001 BTF cannot be expressed at all --
// even though COIN is 1e8 and eight decimals are used everywhere else in the
// codebase. The ImGui wallet inherits that limit; this does not.
static bool ParseAmount(const string& strIn, int64& nRet)
{
    string s;
    for (unsigned int i = 0; i < strIn.size(); i++)
        if (!isspace((unsigned char)strIn[i]))
            s += strIn[i];
    if (s.empty())
        return false;

    string whole, frac;
    bool fDot = false;
    for (unsigned int i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '.')
        {
            if (fDot) return false;              // more than one decimal point
            fDot = true;
            continue;
        }
        if (!isdigit((unsigned char)c)) return false;
        (fDot ? frac : whole) += c;
    }
    if (whole.empty() && frac.empty()) return false;
    if (frac.size() > 8)  return false;          // finer than one satoshi
    if (whole.size() > 8) return false;           // past MAX_MONEY regardless
    while (frac.size() < 8) frac += '0';          // pad out to satoshis

    int64 nValue = (whole.empty() ? 0 : atoi64(whole.c_str())) * COIN
                 + atoi64(frac.c_str());
    if (!MoneyRange(nValue)) return false;
    nRet = nValue;
    return true;
}


static json RpcGetInfo()
{
    int64 nMature, nImmature, nOrphaned; int nMined, nMinedImm, nMinedOrph;
    WalletTotals(nMature, nImmature, nOrphaned, nMined, nMinedImm, nMinedOrph);
    int nPeers = 0;
    CRITICAL_BLOCK(cs_vNodes)
        nPeers = (int)vNodes.size();
    json j;
    j["spendable"]    = FormatAmount(nMature);
    j["maturing"]     = FormatAmount(nImmature);
    j["blocks_mined"]    = nMined;                 // every block we ever found
    j["blocks_accepted"] = nMined - nMinedOrph;    // ...that the chain kept
    j["blocks_orphaned"] = nMinedOrph;             // ...that it did not
    j["orphaned"]        = FormatAmount(nOrphaned);
    j["maturing_blocks"] = nMinedImm;
    j["height"]       = nBestHeight;
    j["peers"]        = nPeers;
    j["mining"]       = fGenerateBitcoins ? true : false;
    j["btf_address"]  = BtfLocalAddress();
    j["hashrate"]     = HashMeterRate();          // H/s, 0 until the window fills
    j["best_hash"]    = hashBestChain.GetHex();   // tip hash, for fork detection
    return j;
}

static json RpcGetNewAddress()
{
    json j;
    j["address"] = PubKeyToAddress(GenerateNewKey());
    return j;
}

static json RpcListTransactions()
{
    struct Row { unsigned int t; string txid, type; int64 amt; int conf; bool imm, orph; };
    vector<Row> rows;
    CRITICAL_BLOCK(cs_mapWallet)
    {
        for (map<uint256, CWalletTx>::iterator it = mapWallet.begin();
             it != mapWallet.end(); ++it)
        {
            CWalletTx* p = &(*it).second;
            Row r;
            r.txid = p->GetHash().GetHex();
            r.t    = p->nTimeReceived;
            r.conf = p->GetDepthInMainChain();
            r.orph = p->IsCoinBase() && WalletTxChainStatus(p) == 0;
            r.imm  = p->IsCoinBase() && !r.orph && p->GetBlocksToMaturity() > 0;
            int64 nCredit = p->CTransaction::GetCredit();
            int64 nDebit  = p->GetDebit();
            if (p->IsCoinBase())      { r.type = "mined";    r.amt = nCredit; }
            else if (nDebit > 0)      { r.type = "sent";     r.amt = nCredit - nDebit; }
            else                      { r.type = "received"; r.amt = nCredit; }
            rows.push_back(r);
        }
    }
    // newest first
    for (unsigned int i = 0; i < rows.size(); i++)
        for (unsigned int k = i + 1; k < rows.size(); k++)
            if (rows[k].t > rows[i].t)
                swap(rows[i], rows[k]);

    json arr = json::array();
    for (unsigned int i = 0; i < rows.size() && i < 100; i++)
    {
        json o;
        o["txid"]          = rows[i].txid;
        o["type"]          = rows[i].type;
        o["amount"]        = FormatAmount(rows[i].amt);
        o["confirmations"] = rows[i].conf;
        o["time"]          = (int64)rows[i].t;
        o["maturing"]      = rows[i].imm;
        o["orphaned"]      = rows[i].orph;
        arr.push_back(o);
    }
    json j; j["txs"] = arr;
    return j;
}

static json RpcSendToAddress(const json& in)
{
    json j;
    if (!in.contains("address") || !in.contains("amount"))
        { j["error"] = "address and amount are required"; return j; }

    string strAddr = in["address"].is_string() ? in["address"].get<string>() : string();
    string strAmt  = in["amount"].is_string()  ? in["amount"].get<string>()
                                              : to_string(in["amount"].get<double>());

    uint160 hash160;
    if (!AddressToHash160(strAddr.c_str(), hash160))
        { j["error"] = "That is not a valid Bitflash address."; return j; }

    int64 nValue = 0;
    if (!ParseAmount(strAmt, nValue) || nValue <= 0)
        { j["error"] = "That is not a valid amount. Up to 8 decimal places."; return j; }

    int64 nMature, nImmature, nOrphaned; int nMined, nMinedImm, nMinedOrph;
    WalletTotals(nMature, nImmature, nOrphaned, nMined, nMinedImm, nMinedOrph);
    if (nValue > nMature)
    {
        // Worth distinguishing: "you have it but it is not mature yet" is a very
        // different problem from "you do not have it".
        string msg = "Not enough spendable coin. You have " + FormatAmount(nMature) + " BTF";
        if (nImmature > 0)
            msg += ", plus " + FormatAmount(nImmature) + " BTF still maturing";
        msg += ".";
        j["error"] = msg;
        return j;
    }

    // Same script and the same send path the ImGui wallet uses.
    CScript scriptPubKey;
    scriptPubKey << OP_DUP << OP_HASH160 << hash160 << OP_EQUALVERIFY << OP_CHECKSIG;

    CWalletTx wtx;
    if (!SendMoney(scriptPubKey, nValue, wtx))
        { j["error"] = "The node refused the transaction. See debug.log."; return j; }

    j["txid"] = wtx.GetHash().GetHex();
    return j;
}


// ------------------------------------------------------------------ http
static void SendRaw(SOCKET s, const string& str)
{
    const char* p = str.c_str();
    int left = (int)str.size();
    while (left > 0)
    {
        int n = send(s, p, left, 0);
        if (n <= 0) return;
        p += n; left -= n;
    }
}

static void Respond(SOCKET s, int code, const char* status,
                    const string& body, const char* ctype)
{
    char hdr[512];
    my_snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n", code, status, ctype, (int)body.size());
    SendRaw(s, string(hdr) + body);
}

static void RespondJson(SOCKET s, int code, const json& j)
{
    Respond(s, code, code == 200 ? "OK" : "Bad Request",
            j.dump(), "application/json");
}

static string HeaderValue(const string& req, const char* name)
{
    // case-insensitive header lookup over the raw request
    string lreq, lname(name);
    lreq.reserve(req.size());
    for (unsigned int i = 0; i < req.size(); i++) lreq += tolower(req[i]);
    for (unsigned int i = 0; i < lname.size(); i++) lname[i] = tolower(lname[i]);
    size_t pos = lreq.find("\r\n" + lname + ":");
    if (pos == string::npos) return "";
    size_t vs = req.find(':', pos + 2) + 1;
    size_t ve = req.find("\r\n", vs);
    if (ve == string::npos) return "";
    string v = req.substr(vs, ve - vs);
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    while (!v.empty() && (v[v.size()-1] == ' ' || v[v.size()-1] == '\r')) v.erase(v.size()-1);
    return v;
}

static void HandleConnection(SOCKET s)
{
    // Read headers, then the body indicated by Content-Length.
    string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == string::npos && req.size() < 32768)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, n);
    }
    size_t hdrEnd = req.find("\r\n\r\n");
    if (hdrEnd == string::npos) return;
    string body = req.substr(hdrEnd + 4);
    int nLen = atoi(HeaderValue(req, "Content-Length").c_str());
    while ((int)body.size() < nLen && body.size() < 1048576)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, n);
    }

    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 == string::npos ? 0 : sp1 + 1);
    if (sp1 == string::npos || sp2 == string::npos) return;
    string method = req.substr(0, sp1);
    string path   = req.substr(sp1 + 1, sp2 - sp1 - 1);

    // A cross-origin page cannot set X-BTF-Token without a preflight, and we
    // answer no preflight -- but refuse anything carrying a foreign Origin
    // outright so a simple-request forgery never touches the wallet.
    string origin = HeaderValue(req, "Origin");
    if (!origin.empty() && origin.find("127.0.0.1") == string::npos
                        && origin.find("localhost") == string::npos)
    {
        json j; j["error"] = "cross-origin requests are refused";
        RespondJson(s, 403, j);
        return;
    }

    if (method == "GET" && (path == "/" || path.substr(0, 2) == "/?"))
    {
        string html(pszWalletHtml);
        size_t at = html.find("__TOKEN__");
        if (at != string::npos)
            html = html.substr(0, at) + g_token + html.substr(at + 9);
        Respond(s, 200, "OK", html, "text/html; charset=utf-8");
        return;
    }

    if (path.substr(0, 5) != "/api/")
    {
        json j; j["error"] = "no such endpoint";
        RespondJson(s, 404, j);
        return;
    }
    if (method != "POST")
    {
        json j; j["error"] = "POST required";
        RespondJson(s, 405, j);
        return;
    }
    if (HeaderValue(req, "X-BTF-Token") != g_token || g_token.empty())
    {
        json j; j["error"] = "bad or missing X-BTF-Token";
        RespondJson(s, 401, j);
        return;
    }

    json in = json::object();
    if (!body.empty())
    {
        try { in = json::parse(body); }
        catch (...) { json j; j["error"] = "malformed JSON body"; RespondJson(s, 400, j); return; }
        if (!in.is_object()) in = json::object();
    }

    string m = path.substr(5);
    json out;
    try
    {
        if      (m == "getinfo")          out = RpcGetInfo();
        else if (m == "getnewaddress")    out = RpcGetNewAddress();
        else if (m == "listtransactions") out = RpcListTransactions();
        else if (m == "sendtoaddress")    out = RpcSendToAddress(in);
        else { out["error"] = "no such method"; RespondJson(s, 404, out); return; }
    }
    catch (std::exception& e) { out = json::object(); out["error"] = e.what(); }
    catch (...)               { out = json::object(); out["error"] = "internal error"; }

    RespondJson(s, out.contains("error") ? 400 : 200, out);
}

void ThreadWalletRPC(void*)
{
    if (!fWalletRpc)
        return;

    MakeToken();

    SOCKET hListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hListen == INVALID_SOCKET)
    {
        printf("walletrpc: socket() failed\n");
        return;
    }
    int one = 1;
    setsockopt(hListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    // Resolve the bind address. Anything other than loopback is an explicit
    // choice by the operator, who is then responsible for confining exposure
    // (under Docker, with -p 127.0.0.1:PORT:PORT).
    unsigned long nBindAddr = inet_addr(strWalletRpcBind.c_str());
    if (nBindAddr == INADDR_NONE)
    {
        printf("walletrpc: /walletrpcbind=%s is not an IPv4 address\n",
               strWalletRpcBind.c_str());
        closesocket(hListen);
        return;
    }
    bool fLoopbackOnly = (nBindAddr == htonl(INADDR_LOOPBACK));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)nWalletRpcPort);
    sa.sin_addr.s_addr = (in_addr_t)nBindAddr;

    if (::bind(hListen, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
    {
        printf("walletrpc: port %d is already in use\n", nWalletRpcPort);
        closesocket(hListen);
        return;
    }
    if (listen(hListen, 16) == SOCKET_ERROR)
    {
        printf("walletrpc: listen() failed\n");
        closesocket(hListen);
        return;
    }

    printf("Wallet GUI: http://%s:%d   (token in %s)\n",
           strWalletRpcBind.c_str(), nWalletRpcPort, TokenPath().c_str());
    if (!fLoopbackOnly)
        printf("walletrpc: bound to %s -- confine this yourself "
               "(Docker: -p 127.0.0.1:%d:%d)\n",
               strWalletRpcBind.c_str(), nWalletRpcPort, nWalletRpcPort);

    while (!fShutdown)
    {
        // select so shutdown is noticed instead of blocking in accept()
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(hListen, &fds);
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 500000;
        if (select(hListen + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        SOCKET s = accept(hListen, (struct sockaddr*)&from, &fromlen);
        if (s == INVALID_SOCKET)
            continue;

        // Only meaningful when bound to loopback. Under Docker the peer address
        // is the bridge gateway, not 127.0.0.1, so enforcing this on a
        // non-loopback bind would reject every legitimate request -- which is
        // exactly the empty-response failure that made this configurable.
        if (fLoopbackOnly && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        {
            closesocket(s);
            continue;
        }

        struct timeval io;
        io.tv_sec = 10; io.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&io, sizeof(io));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&io, sizeof(io));

        try { HandleConnection(s); }
        catch (...) { }
        closesocket(s);
    }

    closesocket(hListen);
}
