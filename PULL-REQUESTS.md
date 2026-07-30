# Pull requests, ready to open

Twelve changes, each on its own branch, each compiling. None alters consensus
rules.

Nothing here has been submitted yet. Below is the title, base branch and body for
each. The body is the commit message — GitHub prefills it from the commit, so
usually you only need to check the base and open it.

**Suggested order.** Send the independent fixes first, smallest and most obviously
correct ahead of the rest: `fix/arm64-sha-bswap`, then
`fix/headless-solo-mining`, then `feat/multithreaded-miner`. Those three are
short, self-evident, and establish that the reports are careful. Save
`feat/wallet-rpc` for after some of them have landed — it is the largest change
and the easiest to stall.

**Stacked branches.** Six branches build on another rather than on upstream,
because they use symbols it introduces. Open those against the parent branch, or
wait until the parent merges and retarget to `main`. GitHub will show only the
branch's own commit either way.

---

## sha: restrict the x86 bswap asm to x86

**Branch:** `fix/arm64-sha-bswap`  
**Base:** `ae482cc`

```
sha: restrict the x86 bswap asm to x86

ByteReverse(word32) in src/sha.h uses the x86 `bswap` instruction via inline
asm, guarded only by `#if defined(__GNUC__)`. GCC and Clang define __GNUC__ on
every architecture they target, so on aarch64 the guard passes and the
assembler then rejects the instruction:

    Error: unknown mnemonic `bswap' -- `bswap %eax'

This makes the tree fail to build on any ARM64 host -- Apple Silicon, ARM
servers, a Raspberry Pi -- at a point far from anything the user did.

Narrow the guard to the architectures where the instruction exists. The
portable C fallback directly below is then used everywhere else; it was
already there and already correct.
```

---

## main: make /gen actually mine

**Branch:** `fix/headless-solo-mining`  
**Base:** `ae482cc`

```
main: make /gen actually mine

nMineMode defaults to MINE_RELAY (0) and ParseStartupArguments() never changed
it, so a node started with /gen would spawn ThreadBitcoinMiner, enter
BitcoinMiner(), hit

    if (nMineMode == MINE_RELAY)
        return true;

and return before doing any work. The node then ran indefinitely looking
completely healthy -- it synced, it held peers, it reported no errors -- while
earning nothing. Nothing in the log said otherwise; the only hint was
`nMineMode = 0` in a startup line that reads like a status echo.

This affects every headless miner, because the GUI is the only other way to
select a mining mode.

/gen now implies MINE_SOLO unless /operator or /participant asked for something
else. Verify with `nMineMode = 1` at startup.
```

---

## miner: one thread per processor

**Branch:** `feat/multithreaded-miner`  
**Base:** `ae482cc`

```
miner: one thread per processor

net.cpp has carried

    //// todo: start one thread per processor, use getenv("NUMBER_OF_PROCESSORS")

above ThreadBitcoinMiner since the beginning, and the node has only ever run a
single miner thread. On a 4-core machine that leaves three quarters of the
available hashrate unused.

Start one thread per core, overridable with BITFLASH_MINERS=N.

No work is duplicated. Each thread builds its own block template with its own
coinbase key, so the templates differ in their merkle roots and the threads
search disjoint keyspaces -- there is no need to partition the nonce range
between them. They share the single RandomX dataset via
RandomXCreateMinerVM(), so the memory cost is one 2 MB scratchpad per thread,
not another 2 GB.

The dataset is built before the threads start. RandomXInitDataset() is not
thread-safe: it checks g_fFast at entry but only sets it after the (slow)
initialisation completes, so several threads entering together each allocate a
full dataset. Measured on 4 cores: ~4x the previous hashrate, memory steady at
~2.3 GB.
```

---

## miner: measure the hashrate

**Branch:** `feat/hash-meter`  
**Base:** `ae482cc`

```
miner: measure the hashrate

The node never counted a hash, so neither the GUI nor any other consumer could
report a rate -- there was no counter to read. A miner has no way to tell a
misconfigured machine from a slow one.

Count hashes where the search loop already pauses to check its stop conditions,
so the counter costs nothing in the inner loop, and expose a rate over a
15-second moving window.

The window is in whole seconds because util.h provides only GetTime(). At 15
seconds that is a few percent of timing error, comfortably below the natural
burstiness of RandomX.
```

---

## miner: notice a new block sooner

**Branch:** `fix/miner-stale-template`  
**Base:** `feat/hash-meter`
> **Stacked.** Open against `feat/hash-meter`, or retarget to `main` once that merges.


```
miner: notice a new block sooner

The search loop checks its stop conditions -- including whether pindexBest has
moved -- every 256 nonces. At the few hundred hashes per second a RandomX
thread manages, that is most of a second during which the thread is extending a
parent that is already stale. A block found in that window is born orphaned.

Check every 32 instead. The cost is negligible (the checks are a handful of
comparisons against a hash that takes milliseconds) and it cuts this class of
self-inflicted orphan by 8x.

This is a small effect next to propagation delay, not a substitute for fixing
it.
```

---

## main: report status from a headless node

**Branch:** `feat/headless-status-line`  
**Base:** `feat/hash-meter`
> **Stacked.** Open against `feat/hash-meter`, or retarget to `main` once that merges.


```
main: report status from a headless node

A headless node could not tell you three things it plainly knows: its balance,
its own .btf address, and its hashrate.

GetBalance() had a single caller, the ImGui GUI. rpc.cpp is a pool/Stratum
surface with no wallet methods. So a miner running /nogui had no way to see
what it had earned -- and because the found-block message in BitcoinMiner()
sits behind the "net" log category (off unless /debug), a solo miner that found
a block printed nothing at all.

BtfLocalAddress() likewise had no headless consumer, so an operator could not
discover the address peers must dial to reach them, which makes /connectbtf
unusable in practice: you cannot tell anyone where to find you.

Print a status line every 60 seconds (BITFLASH_STATUS_SECS=0 to disable):

    STATUS height=3320 peers=8 blocks_mined=20 spendable=10.00 maturing=900.00 (18 block(s))
    STATUS btf=vyqls7b7lw3dyiz72xnkwo5kvlfc5465nlfpqymhdpm6o34rttvgloi.btf
    STATUS hashrate=1854 H/s

Mature and immature are separate because CWalletTx::GetCredit() values immature
coinbase at 0; adding them would report coins as spendable that are not.
```

---

## net: push new blocks instead of announcing them

**Branch:** `fix/block-propagation`  
**Base:** `ae482cc`

```
net: push new blocks instead of announcing them

Orphan rate is a propagation problem: two miners extend the same parent because
neither has heard the other's block yet, and the block that arrives second did
its work for nothing. On this network a third of all blocks are stale.

The relay path costs three message legs per hop. AcceptBlock() calls
RelayInventory(), which queues an inv; the peer answers getdata; only then does
the block move. Each leg waits on the 100ms ThreadMessageHandler tick, and
since IP dialling was removed every leg also crosses a rendezvous relay that
may be intercontinental. Three of those per hop, times the number of hops
between two miners.

Blocks here are tiny -- 983 KB of blk0001.dat for ~3,240 blocks, so ~300 bytes
each. Announcing a 300-byte block with a 36-byte inv and waiting a round trip
for a request is a poor trade. Push the block to every peer not already known
to have it, and two of the three legs disappear.

Backward compatible in both directions, and needs no coordination: the "block"
handler in ProcessMessage() never checks that a block was requested, so
unpatched peers accept a pushed block normally. A duplicate costs one
ProcessBlock() rejection. Blocks above 32 KB fall back to inv so this cannot
become a bandwidth problem if block sizes grow.
```

---

## net: let /connectbtf name more than one peer

**Branch:** `fix/connectbtf-repeatable`  
**Base:** `ae482cc`

```
net: let /connectbtf name more than one peer

strBtfConnect was a single string filled by argval2(), which returns the first
match and stops scanning. A second /connectbtf was therefore accepted on the
command line and silently discarded -- no warning, no error, the peer simply
never dialled.

For a group of nodes deliberately peering with each other this is the whole
feature: with three nodes, each could pin only one of the other two, so the
third link was left to chance. Deliberate peering is how you shorten the paths
that matter for orphan rate.

Make it repeatable, scanning all arguments the way /btfseed alongside it
already does, and start one ThreadBtfConnect per address. That function already
took a single address and reconnected it on a 30-second cycle, so N peers means
N threads and no change to its logic.
```

---

## gitignore: cover the key files the node actually writes

**Branch:** `chore/gitignore-keys`  
**Base:** `ae482cc`

```
gitignore: cover the key files the node actually writes

.gitignore ignores *.dat and wallet.dat, but the node also writes three files
in its data directory that are not .dat and are just as sensitive:

  btf_enc.key      this node's .btf service key -- its network identity
  nostr.key        the Nostr key its descriptors are signed with
  walletrpc.token  bearer token authorising spends over the wallet API

A node run with its data directory inside a working tree -- which is the
obvious thing to do while developing -- leaves all three untracked but
committable, and `git add -A` takes them. Losing btf_enc.key means losing your
address; publishing walletrpc.token hands over the wallet to anyone who can
reach the port.

Also ignore key backup archives and stray .patch/.rej/.orig files.
```

---

## wallet: a localhost JSON API and browser wallet

**Branch:** `feat/wallet-rpc`  
**Base:** `feat/headless-status-line`
> **Stacked.** Open against `feat/headless-status-line`, or retarget to `main` once that merges.


```
wallet: a localhost JSON API and browser wallet

There is no way to see a balance, take a receive address or send a payment
except through the ImGui window. That window needs libGL and does not build on
macOS at all, so on a headless or Mac node the coins are unreachable: they
exist, they are yours, and nothing in the shipped binary will spend them.

Add a small HTTP/JSON server on the loopback interface exposing getinfo,
getnewaddress, listtransactions and sendtoaddress, and serve a self-contained
browser UI that speaks to it. Off unless /walletrpc is passed.

No new cryptography. sendtoaddress builds the same P2PKH script the GUI builds
and calls the existing SendMoney() -> CreateTransaction() -> SignSignature()
path. This exposes the wallet code that was already there and already tested;
it does not reimplement any of it.

Security, since the endpoint can spend money:

  * Binds 127.0.0.1 by default. /walletrpcbind=ADDR overrides, which is
    necessary under Docker, whose published ports forward to the container's
    eth0 and never to its loopback.
  * Every /api/ call must carry X-BTF-Token, matching a 32-byte random token
    generated at startup and written to <datadir>/walletrpc.token mode 0600.
    The served page holds the token; a hostile page in the same browser cannot
    read it under the same-origin policy, and cannot send a custom header
    cross-origin without a CORS preflight, which is never answered. Requests
    arriving with a foreign Origin are refused outright.
  * One thread per connection. A sequential handler would be parked for the
    whole socket timeout by any browser preconnect that sends no request.

Amounts are parsed at the full eight decimals COIN implies. util.cpp's
ParseMoney() is the original dollars-and-cents parser -- exactly two digits
after the point, capped at 99 -- so it rejects "1.0" and "1.5" and cannot
express anything below 0.01 BTF. It is left untouched for compatibility.

Balances report spendable and maturing separately, because
CWalletTx::GetCredit() values immature coinbase at 0.
```

---

## wallet: stop counting orphaned blocks as income

**Branch:** `fix/orphan-accounting`  
**Base:** `feat/wallet-rpc`
> **Stacked.** Open against `feat/wallet-rpc`, or retarget to `main` once that merges.


```
wallet: stop counting orphaned blocks as income

An orphaned block's coinbase is reported as pending income, permanently.

GetBlocksToMaturity() is max(0, COINBASE_MATURITY+20 - GetDepthInMainChain()),
and GetDepthInMainChain() returns 0 for a block that is not on the main chain --
correctly, since it tests IsInMainChain() rather than subtracting heights. So an
orphaned coinbase reports the full maturity remaining and presents as "maturing".
The depth never grows, so it presents that way forever: coins that will never
arrive, displayed indefinitely as coins on the way.

Observed on a node with 20 mined blocks: 950 BTF reported as maturing, of which
100 BTF belonged to two orphaned blocks and was never going to be spendable.

Distinguish the two by asking whether the block is on the main chain, which is a
different question from how deep it is. getinfo gains blocks_accepted,
blocks_orphaned and an orphaned total; the status line gains accepted= and
orphaned= counts; the UI labels orphaned history rows and stops colouring them
as income.

This also answers a question a miner cannot otherwise answer: are my blocks
being accepted by the network at all?
```

---

## wallet: handle connections concurrently

**Branch:** `fix/rpc-concurrency`  
**Base:** `fix/orphan-accounting`
> **Stacked.** Open against `fix/orphan-accounting`, or retarget to `main` once that merges.


```
wallet: handle connections concurrently

The accept loop called HandleConnection() inline, making the server strictly
sequential. Browsers open speculative preconnect sockets that send no request
at all; such a socket parks the single handler in recv() for the entire
SO_RCVTIMEO while every other request -- including the page's own polling --
queues in the backlog behind it.

Symptom: the server intermittently takes ten seconds to answer, or appears to
hang entirely, with nothing wrong on either side. A curl against it sits at zero
bytes received for the full timeout.

Handle each connection in its own thread. Reproduced by holding a socket open
without sending: before, concurrent requests blocked for 10s; after, they answer
immediately.

Also in passing:

  * Answer /favicon.ico with 204 instead of 404. Browsers request it on every
    page load, so the console collected a red error line per visit that is
    indistinguishable from a real failure when debugging one.
  * Send correct HTTP reason phrases. Every non-200 response was labelled "Bad
    Request", so a 404 announced itself as "404 (Bad Request)".
```

---

## Not included

Two things were deliberately left out.

**RandomX large pages.** `randomx_get_flags()` never sets
`RANDOMX_FLAG_LARGE_PAGES`, leaving roughly 15-30% of hashrate unused. Enabling
it requires hugepages configured on the host, so it needs a decision about
whether to attempt and fall back, or require opt-in. Worth an issue rather than a
surprise patch.

**The 100ms message-loop tick.** `ThreadMessageHandler2` sleeps 100ms per
iteration, which gates every message leg. With propagation cut from three legs to
one this matters about a third as much as it did, and changing both at once makes
the effect of either impossible to measure.
