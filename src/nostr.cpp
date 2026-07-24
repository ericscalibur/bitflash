// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Peer discovery over Nostr relays. See nostr.h.

#include "headers.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
// We link libsecp256k1 statically (without this the header assumes DLL import)
#define SECP256K1_STATIC
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>

// util.h redefines snprintf as a macro (2009 MSVC compat); this breaks
// nlohmann/json, which calls std::snprintf. Undo the macro just here.
#ifdef snprintf
#undef snprintf
#endif
#include <nlohmann/json.hpp>

#include "btfaddr.h"
#include "btfchan.h"

using json = nlohmann::json;

// Bitflash rendezvous descriptor: parameterized-replaceable Nostr event so each
// node keeps exactly one current descriptor (keyed by author pubkey).
static const int   BTF_DESC_KIND = 38501;
// "-2" scopes the 2026 stable relaunch: new nodes discover only each other, and
// the abandoned test .btf descriptors lingering on the relays become invisible.
static const char* BTF_DESC_DTAG = "btf-descriptor-2";

// Nostr network/scope for Bitflash. Events are replaceable (NIP-78).
static const int   NOSTR_KIND   = 30078;
static const char* NOSTR_DTAG   = "bitflash-mainnet-2";

// Anonymous auto-discovery: how many peers to keep connected via .btf rendezvous
// before we stop dialing more, and how many new dials to attempt per cycle.
static const unsigned int BTF_TARGET_CONN   = 8;
static const int          BTF_DIALS_PER_PASS = 4;

// Other nodes' .btf addresses learned from their descriptors on the relays.
static set<string> g_btfPeers;
static CCriticalSection cs_btfPeers;

// Public discovery relays. Tunable.
const char* pszNostrRelays[] = {
    "wss://relay.damus.io",
    "wss://nos.lol",
    "wss://relay.nostr.band",
    "wss://relay.snort.social",
};
const int nNostrRelays = ARRAYLEN(pszNostrRelays);

// Rendezvous meeting relay this node registers at (ThreadBtfAccept in net.cpp)
// and advertises in its .btf descriptor. Seed default; /rvrelay overrides.
// Seed rendezvous relays. More entries = more resilience: the node fails over
// between them (see ThreadBtfAccept), so DDoSing one relay IP can't take the
// network down. /rvrelay overrides this list with a single entry.
vector<string> vBtfMeetingRelays = {
    "92.246.128.180:8434",  // Sao Paulo, BR
    "31.44.4.249:8434",     // New Jersey, US
    "90.156.222.107:8434",  // Almaty, KZ
};
string strBtfActiveRelay;
static CCriticalSection cs_activeRelay;

string BtfActiveRelay()
{
    CRITICAL_BLOCK(cs_activeRelay)
        if (!strBtfActiveRelay.empty())
            return strBtfActiveRelay;
    if (!vBtfMeetingRelays.empty())
        return vBtfMeetingRelays[0];
    return "";
}

void BtfSetActiveRelay(const string& relay)
{
    CRITICAL_BLOCK(cs_activeRelay)
        strBtfActiveRelay = relay;
}


//
// Utilities
//
static string HexEncode(const unsigned char* p, size_t n)
{
    static const char* h = "0123456789abcdef";
    string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++)
    {
        s += h[p[i] >> 4];
        s += h[p[i] & 0xf];
    }
    return s;
}

static bool HexDecode(const string& s, unsigned char* out, size_t n)
{
    if (s.size() != n * 2)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        unsigned int b;
        if (sscanf(s.c_str() + i * 2, "%2x", &b) != 1)
            return false;
        out[i] = (unsigned char)b;
    }
    return true;
}

static string Base64Encode(const unsigned char* p, size_t n)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string s;
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned int b = p[i] << 16;
        if (i + 1 < n) b |= p[i + 1] << 8;
        if (i + 2 < n) b |= p[i + 2];
        s += tbl[(b >> 18) & 0x3f];
        s += tbl[(b >> 12) & 0x3f];
        s += (i + 1 < n) ? tbl[(b >> 6) & 0x3f] : '=';
        s += (i + 2 < n) ? tbl[b & 0x3f] : '=';
    }
    return s;
}


//
// Node Nostr key -- secp256k1 x-only (BIP340), persisted in <appdir>/nostr.key
//
class CNostrKey
{
public:
    secp256k1_context* ctx;
    secp256k1_keypair  keypair;
    unsigned char      seckey[32];   // node's secp256k1 secret (for descriptor signing)
    unsigned char      xonly[32];    // node's x-only pubkey (also its .btf identity)
    unsigned char      enc_pk[32];   // node's x25519 public key (echan `enc` field)
    unsigned char      enc_sk[32];   // node's x25519 secret key
    bool               fValid;

    CNostrKey() : ctx(NULL), fValid(false) {}

    ~CNostrKey()
    {
        if (ctx) secp256k1_context_destroy(ctx);
    }

    // Load or generate a 32-byte key persisted as hex in <appdir>/<name>.
    static void LoadOrGenSecp(secp256k1_context* ctx, const string& file, unsigned char sk[32])
    {
        FILE* f = fopen(file.c_str(), "rb");
        string strHex;
        if (f) { char buf[65] = {0}; fread(buf, 1, 64, f); fclose(f); strHex = buf; }
        if (!HexDecode(strHex, sk, 32) || !secp256k1_ec_seckey_verify(ctx, sk))
        {
            do { RAND_bytes(sk, sizeof(unsigned char) * 32); } while (!secp256k1_ec_seckey_verify(ctx, sk));
            FILE* fw = fopen(file.c_str(), "wb");
            if (fw) { string h = HexEncode(sk, 32); fwrite(h.c_str(), 1, h.size(), fw); fclose(fw); }
        }
    }

    bool Init()
    {
        ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        if (!ctx)
            return false;
        unsigned char seed[32];
        RAND_bytes(seed, sizeof(seed));
        secp256k1_context_randomize(ctx, seed);

        LoadOrGenSecp(ctx, GetAppDir() + "/nostr.key", seckey);

        if (!secp256k1_keypair_create(ctx, &keypair, seckey))
            return false;
        secp256k1_xonly_pubkey xpub;
        if (!secp256k1_keypair_xonly_pub(ctx, &xpub, NULL, &keypair))
            return false;
        secp256k1_xonly_pubkey_serialize(ctx, xonly, &xpub);

        // x25519 encryption keypair for the end-to-end channel, persisted separately.
        btf::ChanInit();
        string strEncFile = GetAppDir() + "/btf_enc.key";
        FILE* fe = fopen(strEncFile.c_str(), "rb");
        bool fLoaded = false;
        if (fe)
        {
            char buf[65] = {0};
            if (fread(buf, 1, 64, fe) == 64 && HexDecode(string(buf), enc_sk, 32))
                fLoaded = true;
            fclose(fe);
        }
        if (fLoaded)
        {
            // Derive the public key from the stored secret (stable identity).
            btf::ChanPublicFromSecret(enc_pk, enc_sk);
        }
        else
        {
            btf::ChanKeypair(enc_pk, enc_sk);
            FILE* few = fopen(strEncFile.c_str(), "wb");
            if (few) { string h = HexEncode(enc_sk, 32); fwrite(h.c_str(), 1, h.size(), few); fclose(few); }
        }

        fValid = true;
        return true;
    }

    string PubKeyHex() const { return HexEncode(xonly, 32); }
    string EncPubHex() const { return HexEncode(enc_pk, 32); }
    string BtfAddress() const { return btf::Address(xonly); }

    bool SignId(const unsigned char id[32], string& sigHexOut)
    {
        unsigned char sig[64];
        unsigned char aux[32];
        RAND_bytes(aux, sizeof(aux));
        if (!secp256k1_schnorrsig_sign32(ctx, sig, id, &keypair, aux))
            return false;
        sigHexOut = HexEncode(sig, 64);
        return true;
    }
};

// The node's single Nostr/.btf identity, shared by the seeding thread here and
// the rendezvous transport in net.cpp. Lazily initialized under a lock; the
// secp256k1 context is immutable after Init, so concurrent use is safe.
static CNostrKey g_nostrKey;
static CCriticalSection cs_nostrKey;

static bool EnsureNostrKey()
{
    CRITICAL_BLOCK(cs_nostrKey)
        if (!g_nostrKey.fValid)
            g_nostrKey.Init();
    return g_nostrKey.fValid;
}

bool BtfGetIdentity(unsigned char pubkey[32], unsigned char enc_sk[32])
{
    if (!EnsureNostrKey())
        return false;
    memcpy(pubkey, g_nostrKey.xonly, 32);
    memcpy(enc_sk, g_nostrKey.enc_sk, 32);
    return true;
}

std::string BtfLocalAddress()
{
    if (!EnsureNostrKey())
        return "";
    return g_nostrKey.BtfAddress();
}


// Build and sign a Nostr event (NIP-01). Returns the event JSON.
static bool BuildSignedEvent(CNostrKey& key, int kind, const json& tags,
                             const string& content, json& eventOut)
{
    int64 created = GetTime();
    // Canonical serialization for the id: [0,pubkey,created_at,kind,tags,content]
    json ser = json::array({ 0, key.PubKeyHex(), created, kind, tags, content });
    string s = ser.dump();

    unsigned char id[32];
    SHA256((const unsigned char*)s.data(), s.size(), id);

    string sigHex;
    if (!key.SignId(id, sigHex))
        return false;

    eventOut = json::object();
    eventOut["id"]         = HexEncode(id, 32);
    eventOut["pubkey"]     = key.PubKeyHex();
    eventOut["created_at"] = created;
    eventOut["kind"]       = kind;
    eventOut["tags"]       = tags;
    eventOut["content"]    = content;
    eventOut["sig"]        = sigHex;
    return true;
}


//
// Minimal WebSocket client over TLS (wss) or TCP (ws), blocking with timeout.
//
class CWebSocket
{
public:
    SOCKET     hSocket;
    SSL_CTX*   sslctx;
    SSL*       ssl;
    bool       fTls;
    string     recvbuf;

    CWebSocket() : hSocket(INVALID_SOCKET), sslctx(NULL), ssl(NULL), fTls(false) {}
    ~CWebSocket() { Close(); }

    void Close()
    {
        if (ssl)    { SSL_shutdown(ssl); SSL_free(ssl); ssl = NULL; }
        if (sslctx) { SSL_CTX_free(sslctx); sslctx = NULL; }
        if (hSocket != INVALID_SOCKET) { closesocket(hSocket); hSocket = INVALID_SOCKET; }
    }

    int RawRead(char* buf, int len)
    {
        if (fTls) return SSL_read(ssl, buf, len);
        return recv(hSocket, buf, len, 0);
    }

    bool RawWrite(const char* buf, int len)
    {
        int off = 0;
        while (off < len)
        {
            int n = fTls ? SSL_write(ssl, buf + off, len - off)
                         : send(hSocket, buf + off, len - off, 0);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }

    // Connect and perform the WebSocket handshake. url = wss://host[:port]/path
    bool Connect(const string& url, int timeoutSec)
    {
        string u = url;
        int port = 443;
        fTls = true;
        if (u.compare(0, 6, "wss://") == 0) { u = u.substr(6); fTls = true; port = 443; }
        else if (u.compare(0, 5, "ws://") == 0) { u = u.substr(5); fTls = false; port = 80; }
        else return false;

        string host, path = "/";
        size_t slash = u.find('/');
        if (slash != string::npos) { host = u.substr(0, slash); path = u.substr(slash); }
        else host = u;
        size_t colon = host.find(':');
        if (colon != string::npos) { port = atoi(host.substr(colon + 1).c_str()); host = host.substr(0, colon); }

        // Resolve and connect (TCP)
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[16]; sprintf(portstr, "%d", port);
        if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res)
            return error("Nostr: getaddrinfo %s failed", host.c_str());
        hSocket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (hSocket == INVALID_SOCKET) { freeaddrinfo(res); return false; }

        // Socket receive/send timeout: Windows takes a DWORD of milliseconds;
        // POSIX takes a struct timeval.
#ifdef _WIN32
        DWORD tv = timeoutSec * 1000;
        setsockopt(hSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        setsockopt(hSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(hSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        setsockopt(hSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
#endif

        if (connect(hSocket, res->ai_addr, (int)res->ai_addrlen) != 0)
        {
            freeaddrinfo(res);
            return error("Nostr: connect %s:%d failed", host.c_str(), port);
        }
        freeaddrinfo(res);

        // TLS
        if (fTls)
        {
            sslctx = SSL_CTX_new(TLS_client_method());
            if (!sslctx) return false;
            ssl = SSL_new(sslctx);
            SSL_set_fd(ssl, (int)hSocket);
            SSL_set_tlsext_host_name(ssl, host.c_str());  // SNI
            if (SSL_connect(ssl) != 1)
                return error("Nostr: TLS handshake with %s failed", host.c_str());
        }

        // WebSocket handshake (HTTP Upgrade)
        unsigned char keybytes[16];
        RAND_bytes(keybytes, sizeof(keybytes));
        string wskey = Base64Encode(keybytes, sizeof(keybytes));
        string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wskey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!RawWrite(req.data(), (int)req.size()))
            return false;

        // Read the handshake response until \r\n\r\n
        string resp;
        char c;
        while (resp.find("\r\n\r\n") == string::npos)
        {
            int n = RawRead(&c, 1);
            if (n <= 0) return error("Nostr: no handshake response from %s", host.c_str());
            resp += c;
            if (resp.size() > 4096) break;
        }
        if (resp.find(" 101 ") == string::npos)
            return error("Nostr: handshake refused by %s", host.c_str());
        return true;
    }

    // Send a text message (masked frame, as required by the protocol)
    bool SendText(const string& msg)
    {
        string frame;
        frame += (char)0x81; // FIN + opcode texto
        size_t len = msg.size();
        if (len < 126)
            frame += (char)(0x80 | len);
        else if (len <= 0xffff)
        {
            frame += (char)(0x80 | 126);
            frame += (char)((len >> 8) & 0xff);
            frame += (char)(len & 0xff);
        }
        else
        {
            frame += (char)(0x80 | 127);
            for (int i = 7; i >= 0; i--) frame += (char)((len >> (8 * i)) & 0xff);
        }
        unsigned char mask[4];
        RAND_bytes(mask, 4);
        frame.append((char*)mask, 4);
        string masked = msg;
        for (size_t i = 0; i < masked.size(); i++)
            masked[i] ^= mask[i & 3];
        frame += masked;
        return RawWrite(frame.data(), (int)frame.size());
    }

    // Receive the next complete text message. Returns false on error/timeout.
    bool RecvText(string& out)
    {
        out.clear();
        for (;;)
        {
            unsigned char hdr[2];
            if (!ReadN((char*)hdr, 2)) return false;
            bool fin = hdr[0] & 0x80;
            int opcode = hdr[0] & 0x0f;
            bool masked = hdr[1] & 0x80;
            uint64 len = hdr[1] & 0x7f;
            if (len == 126)
            {
                unsigned char e[2];
                if (!ReadN((char*)e, 2)) return false;
                len = (e[0] << 8) | e[1];
            }
            else if (len == 127)
            {
                unsigned char e[8];
                if (!ReadN((char*)e, 8)) return false;
                len = 0;
                for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
            }
            unsigned char mask[4] = {0,0,0,0};
            if (masked && !ReadN((char*)mask, 4)) return false;

            string payload;
            payload.resize((size_t)len);
            if (len && !ReadN(&payload[0], (int)len)) return false;
            if (masked)
                for (size_t i = 0; i < payload.size(); i++)
                    payload[i] ^= mask[i & 3];

            if (opcode == 0x8) return false;                 // close
            if (opcode == 0x9) { SendPong(payload); continue; } // ping
            if (opcode == 0xA) continue;                      // pong
            out += payload;                                   // text/continuation
            if (fin) return true;
        }
    }

private:
    bool ReadN(char* buf, int n)
    {
        int off = 0;
        while (off < n)
        {
            int r = RawRead(buf + off, n - off);
            if (r <= 0) return false;
            off += r;
        }
        return true;
    }

    void SendPong(const string& payload)
    {
        string frame;
        frame += (char)0x8A;
        frame += (char)(0x80 | (payload.size() & 0x7f));
        unsigned char mask[4]; RAND_bytes(mask, 4);
        frame.append((char*)mask, 4);
        string m = payload;
        for (size_t i = 0; i < m.size(); i++) m[i] ^= mask[i & 3];
        frame += m;
        RawWrite(frame.data(), (int)frame.size());
    }
};


// Handle a received event: extract "ip:port" and register the address.
static void HandlePeerEvent(const json& ev)
{
    if (!ev.contains("content")) return;
    string content = ev["content"].get<string>();
    // expected content: "ip:port"
    CAddress addr(content.c_str());
    if (addr.ip == 0 || addr.ip == INADDR_NONE)
        return;
    if (addr.ip == addrLocalHost.ip && addr.port == addrLocalHost.port)
        return; // not ourselves (same ip:port)
    // Peer received from the relay (before AddAddress's routability filter).
    printf("Nostr: peer received from relay: %s\n", addr.ToStringIPPort().c_str());
    CAddrDB addrdb;
    if (AddAddress(addrdb, addr))
        printf("Nostr: new peer added %s\n", addr.ToStringIPPort().c_str());
}


// Publish this node's self-certifying `.btf` descriptor: "reach me (this key)
// via <meeting_node>, encrypting to my x25519 key <enc>". Signed by the node
// key so only the address's owner can publish it (no hijacking).
static void PublishDescriptor(CWebSocket& ws, CNostrKey& key)
{
    // meeting_node is the rendezvous relay this node's hidden service
    // (ThreadBtfAccept in net.cpp) is registered at -- where clients dial us.
    string meeting_node = BtfActiveRelay();
    if (meeting_node.empty())
        meeting_node = "rendezvous-pending";
    string desc = btf::SignDescriptor(key.ctx, key.seckey, key.EncPubHex(),
                                      meeting_node, (uint64_t)GetTime());
    if (desc.empty())
        return;
    json tags = json::array({ json::array({ "d", BTF_DESC_DTAG }) });
    json ev;
    if (BuildSignedEvent(key, BTF_DESC_KIND, tags, desc, ev))
    {
        json pub = json::array({ "EVENT", ev });
        ws.SendText(pub.dump());
    }
}

// Resolve a `.btf` address via a relay: fetch the descriptor published by that
// address's key and verify it self-certifies under the decoded pubkey.
static bool ResolveDescriptor(CWebSocket& ws, void* ctx, const string& btfAddr,
                              btf::Descriptor& out)
{
    unsigned char pk[32];
    if (!btf::ParseAddress(btfAddr, pk))
        return false;
    json filter = json::object();
    filter["authors"] = json::array({ HexEncode(pk, 32) });
    filter["kinds"]   = json::array({ BTF_DESC_KIND });
    filter["limit"]   = 1;
    json req = json::array({ "REQ", "btf-resolve", filter });
    if (!ws.SendText(req.dump()))
        return false;
    for (;;)
    {
        string msg;
        if (!ws.RecvText(msg))
            break;
        json j;
        try { j = json::parse(msg); } catch (...) { continue; }
        if (!j.is_array() || j.empty()) continue;
        string t = j[0].get<string>();
        if (t == "EVENT" && j.size() >= 3)
        {
            const json& ev = j[2];
            if (ev.contains("content") &&
                btf::VerifyDescriptor(ctx, ev["content"].get<string>(), pk, out))
                return true;
        }
        else if (t == "EOSE")
            break;
    }
    return false;
}

// Resolve a `.btf` address into its rendezvous coordinates by querying the
// public discovery relays for the owner's self-certified descriptor. Used by
// ConnectNodeBtf in net.cpp.
bool BtfResolve(const std::string& btfAddr, std::string& meetingHostPort,
                unsigned char enc_pub[32])
{
    if (!EnsureNostrKey())
        return false;
    for (int i = 0; i < nNostrRelays && !fShutdown; i++)
    {
        try
        {
            CWebSocket ws;
            if (!ws.Connect(pszNostrRelays[i], 10))
                continue;
            btf::Descriptor d;
            if (!ResolveDescriptor(ws, g_nostrKey.ctx, btfAddr, d))
                continue;
            if (d.meeting_node.empty() || d.meeting_node == "rendezvous-pending")
                continue; // descriptor predates the owner's relay config
            if (!HexDecode(d.enc, enc_pub, 32))
                continue;
            meetingHostPort = d.meeting_node;
            return true;
        }
        CATCH_PRINT_EXCEPTION("BtfResolve")
    }
    return false;
}


// Connect to a relay: publish our address and collect the others'.
static bool SeedFromRelay(CNostrKey& key, const string& relay)
{
    CWebSocket ws;
    if (!ws.Connect(relay, 10))
        return false;
    printf("Nostr: connected to %s\n", relay.c_str());

    // Publish our announcement (replaceable event with our ip:port)
    if (addrLocalHost.ip != 0)
    {
        json tags = json::array({
            json::array({ "d", NOSTR_DTAG }),
            json::array({ "t", NOSTR_DTAG })
        });
        json ev;
        if (BuildSignedEvent(key, NOSTR_KIND, tags, addrLocalHost.ToStringIPPort(), ev))
        {
            json pub = json::array({ "EVENT", ev });
            ws.SendText(pub.dump());
        }
    }

    // Publish our self-certifying .btf descriptor (rendezvous discovery)
    PublishDescriptor(ws, key);

    // Subscribe to receive the announcements of other nodes
    json filter = json::object();
    filter["kinds"] = json::array({ NOSTR_KIND });
    filter["#d"]    = json::array({ NOSTR_DTAG });
    filter["limit"] = 500;
    json req = json::array({ "REQ", "bf-sub", filter });
    if (!ws.SendText(req.dump()))
        return false;

    // Read responses until EOSE (end of stored events) or timeout
    int nEvents = 0;
    for (;;)
    {
        string msg;
        if (!ws.RecvText(msg))
            break; // timeout or closed connection
        json j;
        try { j = json::parse(msg); } catch (...) { continue; }
        if (!j.is_array() || j.empty()) continue;
        string type = j[0].get<string>();
        if (type == "EVENT" && j.size() >= 3)
        {
            HandlePeerEvent(j[2]);
            if (++nEvents > 1000) break;
        }
        else if (type == "EOSE")
        {
            // We received all stored events; close this relay.
            break;
        }
        else if (type == "NOTICE" || type == "OK" || type == "CLOSED")
        {
            // informational
        }
    }
    printf("Nostr: %s returned %d events\n", relay.c_str(), nEvents);

    // Discover other nodes' .btf descriptors so we can auto-connect anonymously
    // (no manual /connectbtf). Each node publishes one addressable descriptor
    // (kind 38501, d-tag btf-descriptor); we learn every peer's .btf address by
    // deriving it from the event author. Reachability is verified later, when
    // the rendezvous tunnel boxes to that key's static x25519 key.
    ws.SendText(json::array({ "CLOSE", "bf-sub" }).dump());
    {
        json dfilter = json::object();
        dfilter["kinds"] = json::array({ BTF_DESC_KIND });
        dfilter["#d"]    = json::array({ BTF_DESC_DTAG });
        dfilter["limit"] = 200;
        json dreq = json::array({ "REQ", "btf-disc", dfilter });
        int nPeers = 0;
        if (ws.SendText(dreq.dump()))
        {
            for (;;)
            {
                string msg;
                if (!ws.RecvText(msg))
                    break;
                json j;
                try { j = json::parse(msg); } catch (...) { continue; }
                if (!j.is_array() || j.empty()) continue;
                string type = j[0].get<string>();
                if (type == "EVENT" && j.size() >= 3)
                {
                    const json& ev = j[2];
                    if (!ev.contains("pubkey") || !ev.contains("kind")) continue;
                    if (ev["kind"].get<int>() != BTF_DESC_KIND) continue;
                    unsigned char pkb[32];
                    if (!HexDecode(ev["pubkey"].get<string>(), pkb, 32)) continue;
                    if (memcmp(pkb, key.xonly, 32) == 0) continue; // not ourselves
                    string peerAddr = btf::Address(pkb);
                    bool fNew = false;
                    CRITICAL_BLOCK(cs_btfPeers)
                        fNew = g_btfPeers.insert(peerAddr).second;
                    if (fNew)
                        printf("Nostr: discovered .btf peer %s\n", peerAddr.c_str());
                    if (++nPeers > 500) break;
                }
                else if (type == "EOSE")
                    break;
            }
        }
    }
    return true;
}


// Per-peer backoff: don't redial a .btf that just failed until this time. Keeps
// dead/stale descriptors (e.g. abandoned test identities that linger on the
// relays) from consuming the whole per-pass dial budget and starving the live
// peers. Backoff grows on repeated failures, capped.
static map<string, int64> g_btfPeerBackoff;   // addr -> next-try time
static map<string, int>   g_btfPeerFails;      // addr -> consecutive failures

// Dial the .btf peers discovered on the relays, up to a target connection count.
// Mirrors ThreadOpenConnections' ref handling: a kept connection holds one ref
// (owned by fNetworkNode, released on disconnect); a redundant one is released.
static void ConnectDiscoveredBtfPeers()
{
    vector<string> peers;
    CRITICAL_BLOCK(cs_btfPeers)
        peers.assign(g_btfPeers.begin(), g_btfPeers.end());

    // Shuffle: the candidate set is sorted, so without this a handful of dead
    // peers at the front would be retried every pass and the live peers further
    // down the list would never be reached within the per-pass dial budget.
    for (size_t i = peers.size(); i > 1; i--)
        swap(peers[i - 1], peers[(size_t)GetRand(i)]);

    int64 now = GetTime();
    int nDials = 0;
    foreach(const string& addr, peers)
    {
        if (fShutdown) break;
        if (vNodes.size() >= BTF_TARGET_CONN) break;
        if (nDials >= BTF_DIALS_PER_PASS) break;

        // Skip peers still in backoff from a recent failure.
        map<string, int64>::iterator bi = g_btfPeerBackoff.find(addr);
        if (bi != g_btfPeerBackoff.end() && now < bi->second)
            continue;

        CNode* pnode = ConnectNodeBtf(addr);
        if (pnode)
        {
            if (!pnode->fNetworkNode)
                pnode->fNetworkNode = true; // keep the ref from ConnectNodeBtf
            else
                pnode->Release();           // already connected; drop the extra ref
            g_btfPeerBackoff.erase(addr);   // reachable again
            g_btfPeerFails.erase(addr);
        }
        else
        {
            // Exponential-ish backoff: 1,2,4,... minutes, capped at 30.
            int n = ++g_btfPeerFails[addr];
            int64 mins = 1;
            for (int k = 1; k < n && mins < 30; k++) mins *= 2;
            if (mins > 30) mins = 30;
            g_btfPeerBackoff[addr] = now + mins * 60;
            nDials++; // only count real (failed) dial attempts against the budget
        }
    }
}


void ThreadNostrSeed(void* parg)
{
    printf("ThreadNostrSeed started\n");

    if (!EnsureNostrKey())
    {
        printf("Nostr: failed to initialize secp256k1 key\n");
        return;
    }
    CNostrKey& key = g_nostrKey;
    printf("Nostr: node pubkey = %s\n", key.PubKeyHex().c_str());
    printf("Nostr: node .btf address = %s\n", key.BtfAddress().c_str());

    // One-time self-test of the rendezvous: publish our descriptor to a relay,
    // then resolve our OWN .btf address back and verify it self-certifies. Proves
    // the .btf publish/resolve/verify chain works against real public relays.
    for (int i = 0; i < nNostrRelays && !fShutdown; i++)
    {
        try
        {
            CWebSocket ws;
            if (!ws.Connect(pszNostrRelays[i], 10)) continue;
            PublishDescriptor(ws, key);
            Sleep(700); // let the relay store the replaceable event
            btf::Descriptor d;
            if (ResolveDescriptor(ws, key.ctx, key.BtfAddress(), d))
            {
                printf("Nostr: .btf self-resolve OK via %s (meeting_node=%s, enc=%s...)\n",
                       pszNostrRelays[i], d.meeting_node.c_str(), d.enc.substr(0, 16).c_str());
                break;
            }
        }
        CATCH_PRINT_EXCEPTION("btf self-test")
    }

    // OpenSSL is already initialized by the rest of the app; TLS_client_method is enough.
    loop
    {
        for (int i = 0; i < nNostrRelays; i++)
        {
            if (fShutdown) return;
            try
            {
                SeedFromRelay(key, pszNostrRelays[i]);
            }
            CATCH_PRINT_EXCEPTION("SeedFromRelay")
        }

        // Auto-connect to discovered .btf peers over the rendezvous relay -- the
        // anonymous, CGNAT-friendly replacement for the old IP-based connect. No
        // manual /connectbtf needed: nodes find each other purely via Nostr.
        try
        {
            ConnectDiscoveredBtfPeers();
        }
        CATCH_PRINT_EXCEPTION("ConnectDiscoveredBtfPeers")

        // Re-announce/re-discover periodically (the replaceable event keeps the
        // relay up to date and picks up nodes that joined after us)
        for (int i = 0; i < 60 && !fShutdown; i++)
            Sleep(1000);
    }
}
