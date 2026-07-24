<div align="center">

<img src="docs/logo.png" width="128" alt="Bitflash"/>

# Bitflash ⚡ &nbsp;`BTF`

### Satoshi's Bitcoin, reborn — mine with your **CPU**, stay **invisible**, run **anywhere**.

A faithful revival of the original 2009 `bitcoin-0.1.0`, rebuilt for today:
**memory-hard CPU mining** that shuts out ASICs and GPUs, **anonymous addressing**
that hides your IP and punches through CGNAT, and a **fair launch** with no premine.

<br/>

![PoW](https://img.shields.io/badge/PoW-RandomX%20(CPU)-2ea44f?style=for-the-badge)
![Privacy](https://img.shields.io/badge/addresses-.btf%20anonymous-2ea44f?style=for-the-badge)
![Fair Launch](https://img.shields.io/badge/premine-none-2ea44f?style=for-the-badge)

![Windows](https://img.shields.io/badge/Windows-GUI%20wallet-blue)
![Linux](https://img.shields.io/badge/Linux-headless%20node-blue)
![Language](https://img.shields.io/badge/built%20in-C%2B%2B-00599C)
![License](https://img.shields.io/badge/license-MIT-green)
![Supply](https://img.shields.io/badge/max%20supply-21%2C000%2C000-orange)

**[⬇️ Download for Windows](../../releases/latest)** &nbsp;·&nbsp; **[🐧 Build for Linux](#-linux-node--miner)** &nbsp;·&nbsp; **[🧠 How it works](#-how-it-works)**

</div>

---

## 💡 One CPU, one vote

Bitcoin promised money mined by anyone with a computer. Then ASIC farms and mining
pools took over, and "anyone" became "whoever owns a warehouse." Bitflash gives that
promise back.

- 🧩 **Memory-hard proof of work (RandomX).** The same algorithm Monero uses to keep
  mining on ordinary CPUs. No ASICs. No GPU farms. Your laptop competes on equal footing.
- 🕶️ **Anonymous by default.** Your node is reachable by a self-certifying `.btf`
  address instead of an IP — a Tor-style hidden service built into the coin, over
  [Nostr](https://nostr.com). Peers never see your IP.
- 🌐 **Works behind any firewall.** No public IP? Stuck behind CGNAT? No port
  forwarding? Doesn't matter. Nodes rendezvous through a lightweight relay and connect
  anyway.
- ⚖️ **Fair launch.** No premine. No ICO. No founder's reward. The genesis block mints
  the same 50 coins everyone else mines for.
- 🪙 **Sound money.** 50 BTF per block, halving on schedule, hard cap of **21,000,000** —
  exactly like the original.

---

<div align="center">

## 🔥 Don't let the network die — run a node

A cryptocurrency is only as alive as the people running it. Every node that mines,
relays, and stays online makes Bitflash **faster, freer, and impossible to shut down.**
No companies. No servers to trust. No gatekeepers — just people, one CPU each.

### Download it. Run it. Keep it alive.

**[⬇️ Windows](../../releases/latest)** &nbsp;·&nbsp; **[🐧 Linux](#-linux-node--miner)** &nbsp;·&nbsp; **[🛰️ Run a relay](#-become-a-relay-volunteer--automatically)**

*The network needs you online. Close this tab and it's one node weaker.*

</div>

---

## ✨ Features

| | |
|---|---|
| 🧠 **RandomX PoW** | CPU-only, ASIC/GPU-resistant mining — one CPU, one vote |
| 🔐 **`.btf` hidden addresses** | Self-certifying, unforgeable node identity; your IP is never exposed |
| 🛰️ **Nostr peer discovery** | No central servers, no DNS seeds — nodes find each other over open relays |
| 🚪 **CGNAT traversal** | Rendezvous transport lets nodes behind carrier NAT reach each other |
| 🔒 **End-to-end encrypted P2P** | X25519 + XChaCha20-Poly1305 channel; the relay only ever sees ciphertext |
| 💸 **Instant transfers** | Send coins peer-to-peer in under a second |
| 🖥️ **Retro wallet** | The classic, no-nonsense Bitcoin 0.1.0 interface, one "Generate Coins" click away |
| 🐧 **Windows & Linux** | GUI wallet on Windows, headless daemon for Linux servers |

---

## 🚀 Quick start

### 🪟 Windows (GUI wallet + miner)

1. **[Download the latest release](../../releases/latest)** and unzip it anywhere.
2. Double-click **`bitflash.exe`**.
3. Wait ~1 minute — it finds peers on its own and starts syncing.
4. **Options → Generate Coins** to start mining with your CPU. That's it. 🎉

No install, no config, no command line. It connects automatically, even behind CGNAT.

### 🐧 Linux (node + miner)

```bash
# grab the source, then:
bash build-linux.sh          # installs deps, builds secp256k1 + RandomX + the node
./src/bitflash-node -gen      # run + mine (auto-discovers peers, no config needed)
```

Run it 24/7 as a service — see [`docs/`](docs) and the systemd snippet below.

---

## 🧠 How it works

Bitflash keeps Satoshi's consensus intact and swaps two things: **the proof of work**
and **how nodes find and reach each other**.

### Anonymous rendezvous (the `.btf` network)

Every node has a `.btf` address derived from its public key — like a Tor `.onion`, but
built into the coin. Nodes publish a signed descriptor over Nostr, and connect through a
meeting relay so **neither side ever learns the other's IP**, and **no port forwarding is
ever needed**.

```mermaid
flowchart LR
    A["🧑‍💻 Node A<br/>(behind CGNAT)"] -- "encrypted" --> R(("🛰️ Meeting<br/>relay"))
    B["🧑‍💻 Node B<br/>(behind CGNAT)"] -- "encrypted" --> R
    R -. "pairs by .btf key<br/>sees only ciphertext" .-> A
    R -. "pairs by .btf key<br/>sees only ciphertext" .-> B
    N["🛰️ Nostr relays"] -. "discover .btf peers" .-> A
    N -. "discover .btf peers" .-> B
```

The relay is dumb on purpose: it pairs peers by their `.btf` key and forwards bytes. The
real connection is end-to-end encrypted (X25519 + XChaCha20-Poly1305) and authenticated
to each peer's static key, so a malicious relay can't read or tamper — it can only refuse.

### RandomX proof of work

Blocks are mined with **RandomX**, a memory-hard algorithm that runs efficiently on
general-purpose CPUs and terribly on specialized hardware. This is what keeps mining
decentralized: no ASIC advantage, no GPU farms — just CPUs, one vote each.

---

## 🛠️ Build from source

### Linux

```bash
bash build-linux.sh
```

Installs `build-essential cmake libssl-dev libdb5.3++-dev libsodium-dev
nlohmann-json3-dev libboost-dev`, builds `libsecp256k1` (Schnorr) and `RandomX` from
source, then the `bitflash-node` binary. Debian/Ubuntu.

### Windows

Built with **MSYS2 UCRT64** (g++ 16, wxWidgets 3.2, OpenSSL 3, Berkeley DB). See
[`build.sh`](build.sh) and `src/makefile.mingw`.

### 🛰️ Become a relay volunteer — automatically

Relays are the meeting points that let nodes behind CGNAT find each other. More
relays, in more places, make the network **faster** and **harder to knock
offline**. If you have a VPS with a public IP, you can add one — and **the whole
network discovers it automatically. No approval, no gatekeeper, no waiting.**

**1. Run the relay — one command:**
```bash
sudo bash relay/install-bitflash-relay.sh 8434
```
Builds a tiny, dependency-free daemon, applies anti-flood firewall rules, and
installs it as a `systemd` service (auto-restarts, survives reboot).

**2. Open TCP port `8434`** in your firewall / cloud panel.

**3. Announce it** by running a node that advertises your relay:
```bash
./bitflash-node -gen -announcerelay=YOUR_PUBLIC_IP:8434
```
That node publishes your relay over Nostr; every other node discovers it within
a minute and adds it to its failover list. **Done — you're part of the backbone.**

> 🔒 A relay is **trustless** — it only forwards end-to-end-encrypted bytes and
> pairs peers by their `.btf` key. It **cannot read, tamper with, or MITM** your
> traffic; a malicious relay can at most refuse to forward, and nodes simply fail
> over. That's exactly why anyone can run one safely. A 1 GB VPS is plenty.

<details>
<summary>Run the Linux node 24/7 (systemd)</summary>

```ini
# /etc/systemd/system/bitflash-node.service
[Unit]
Description=Bitflash node/miner
After=network.target

[Service]
WorkingDirectory=/root/bitflash/src
ExecStart=/root/bitflash/src/bitflash-node -gen
Restart=always
User=root

[Install]
WantedBy=multi-user.target
```
```bash
systemctl daemon-reload && systemctl enable --now bitflash-node
```
</details>

---

## 📊 At a glance

| Parameter | Value |
|---|---|
| Ticker | **BTF** |
| Proof of work | RandomX (CPU, memory-hard) |
| Block reward | 50 BTF, halving on schedule |
| Max supply | 21,000,000 BTF |
| Target block time | ~2 minutes |
| Address privacy | `.btf` hidden-service addressing over Nostr |
| Premine / ICO | **None** |
| Platforms | Windows (GUI), Linux (headless) |

---

## 🗺️ Roadmap

- [x] Faithful port of `bitcoin-0.1.0` to a modern toolchain
- [x] RandomX CPU proof of work
- [x] Nostr peer discovery (replacing IRC)
- [x] `.btf` anonymous addressing + CGNAT rendezvous
- [x] Automatic peer discovery — zero config
- [x] Headless Linux node/miner
- [x] Fair-launch, stable-parameter relaunch
- [x] Redundant relays across regions with automatic failover
- [x] Automatic relay discovery over Nostr (self-announcing volunteer relays)
- [ ] Optional direct IPv6 transport (opt-in, for public backbone nodes)
- [ ] One-click installers

---

## ⚠️ Disclaimer

Bitflash is **experimental software**, released in the spirit of the original Bitcoin:
*"This is a design and prototype. Use at your own risk. It comes with no warranty of any
kind."* It has real cryptography and a live network, but it is young. Don't put in more
than you're willing to lose, and don't rely on it for anything critical.

---

## 📜 License

MIT — see [LICENSE](LICENSE). Built on Satoshi Nakamoto's original Bitcoin (2009),
extended by the Bitflash developers (2026).

<div align="center">
<br/>
<b>⚡ One CPU. One vote. Mining returns to the people. ⚡</b>
</div>
