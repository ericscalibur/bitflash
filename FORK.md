# This fork

A fork of [Bitflash](https://github.com/Bitflash-sh/bitflash) adding a browser
wallet to the headless node, and fixing a number of bugs found while getting a
node mining on macOS and Linux.

Forked at upstream **v1.2.4** (`ae482cc`). Nothing here changes consensus rules,
so a node built from this fork stays compatible with every other node on the
network, patched or not.

## Why

The upstream node has a GUI built with ImGui that needs OpenGL, and it does not
build on macOS. Everything the wallet can do — see a balance, take a receive
address, send a payment — lives in that GUI. On a headless or Mac node the coins
are therefore unreachable: they exist, they are yours, and nothing in the shipped
binary will spend them.

Along the way it turned out `/gen` did not mine at all, the miner used one core
regardless of how many the machine had, and a node could not report its own
address, balance, or hashrate.

## What it adds

**A wallet.** A loopback-only HTTP/JSON API and a self-contained browser UI:
balance split into spendable, maturing and orphaned; a receive address; a send
form; transaction history; live hashrate. Enable with `/walletrpc`, then open
`http://127.0.0.1:8901`.

No new cryptography went into this. Sending calls the node's existing
`SendMoney()` → `CreateTransaction()` → `SignSignature()` path, which is the same
code the desktop GUI has always used. This exposes wallet code that was already
there; it does not reimplement any of it.

**Mining that works.** `/gen` implies solo mode, one miner thread per core, and a
hashrate meter.

**Faster block propagation.** New blocks are pushed to peers rather than
announced-then-requested, removing two of three message legs per hop. Orphan rate
is a propagation phenomenon, so this is the lever that matters.

**Honest balances.** An orphaned block's coinbase used to present as "maturing"
forever. Now it is reported separately, because coins that will never arrive
should not look like coins on the way.

## Build

```bash
git clone <this fork> bitflash && cd bitflash
make linux-node          # Linux: installs deps, builds libsecp256k1 + RandomX, then the node
./bitflash-node /nogui /gen /solomine /walletrpc
```

If the libsecp256k1 step fails with `secp256k1.h: No such file or directory`,
install `automake` — upstream's dependency list omits it, and the Makefile
recipe ends in an `echo` which masks the failure, so the build continues without
the library and fails later at an unrelated-looking place.

On macOS, build in a Linux container (Colima or Docker):

```bash
colima start --cpu 4 --memory 4 --disk 30
docker build -t bitflash .
docker run -d --name bitflash --restart unless-stopped \
  -p 127.0.0.1:8901:8901 \
  -v ~/bitflash-data:/root/.bitflash \
  bitflash /nogui /gen /solomine /walletrpc /walletrpcbind=0.0.0.0
```

`/walletrpcbind=0.0.0.0` is required in a container and is not a loosening of
the security model: published ports forward to the container's `eth0`, never to
its loopback, so a server bound to `127.0.0.1` inside a container cannot be
reached by `-p` at all. Confinement moves to `-p 127.0.0.1:8901:8901`, which
publishes only to the host's loopback. Running natively, leave the default alone.

To reach the wallet from another machine, forward the port over SSH rather than
binding it wider:

```bash
ssh -fN -L 8901:127.0.0.1:8901 user@host
```

## Wallet security

The endpoint can spend money, so:

- Off unless `/walletrpc` is passed.
- Binds `127.0.0.1` by default.
- Every `/api/` call must carry `X-BTF-Token`, matching a 32-byte random token
  generated at startup and written to `<datadir>/walletrpc.token` mode 0600.
- The served page holds the token. A hostile page in the same browser cannot read
  it under the same-origin policy, and cannot send a custom header cross-origin
  without a CORS preflight, which is never answered. Requests arriving with a
  foreign `Origin` are refused.
- The token rotates on every restart, so open tabs stop working after one and
  need a reload. That is deliberate.

## Back up your keys

This is a pre-BIP32 wallet. Every mined block, every receive address and every
send's change output adds an **independent** keypair to `wallet.dat`, with no
seed to regenerate them from. A backup is a snapshot and goes stale as you use
the wallet.

```bash
tar czf ~/bitflash-keys-$(date +%Y%m%d-%H%M).tgz \
  -C ~/.bitflash wallet.dat btf_enc.key nostr.key
```

`btf_enc.key` and `nostr.key` are the node's `.btf` identity — lose them and your
address changes. Coins whose keys are gone remain visible on the chain forever
and are spendable by nobody.

## Branches

`wallet-and-fixes` is everything, and is what to build.

Each fix is also a standalone branch off upstream, so it can be submitted on its
own. Every branch below compiles. See [PULL-REQUESTS.md](PULL-REQUESTS.md) for
the descriptions.

| branch | base | what |
|---|---|---|
| `fix/arm64-sha-bswap` | `ae482cc` | x86 `bswap` asm breaks every ARM64 build |
| `fix/headless-solo-mining` | `ae482cc` | `/gen` ran as a relay and mined nothing |
| `feat/multithreaded-miner` | `ae482cc` | one miner thread per core |
| `feat/hash-meter` | `ae482cc` | measure the hashrate |
| `fix/block-propagation` | `ae482cc` | push blocks instead of announcing them |
| `fix/connectbtf-repeatable` | `ae482cc` | a second `/connectbtf` was discarded |
| `chore/gitignore-keys` | `ae482cc` | ignore the key files the node writes |
| `fix/miner-stale-template` | `feat/hash-meter` | notice a new block 8× sooner |
| `feat/headless-status-line` | `feat/hash-meter` | report balance, address, hashrate |
| `feat/wallet-rpc` | `feat/headless-status-line` | the wallet API and UI |
| `fix/orphan-accounting` | `feat/wallet-rpc` | orphaned coinbase is not income |
| `fix/rpc-concurrency` | `fix/orphan-accounting` | thread per connection |

The stacked ones genuinely depend on their base — they reference symbols it
introduces — rather than merely conflicting textually.

## Status

Running on a Mac mini mining solo, with the wallet used to receive and send.
Every branch has been compiled; `wallet-and-fixes` has been run.

The propagation change is **unmeasured**. The mechanism is real and the message
legs are demonstrably gone, but nobody has before-and-after orphan-rate figures
from the live network. If you run a block explorer that computes orphan rate from
`blk0001.dat`, take a reading before deploying so the comparison is possible.

RandomX large pages are not enabled. `randomx_get_flags()` never sets
`RANDOMX_FLAG_LARGE_PAGES`, so roughly 15–30% of throughput is unused. Enabling
it needs hugepages configured on the host.
