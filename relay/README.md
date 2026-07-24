# bitflashd — Bitflash rendezvous relay (the "meeting node")

This is the small always-on server that lets Bitflash nodes behind CGNAT reach
each other. Both sides connect **out** to this relay; it pairs them by their
`.btf` pubkey and forwards ciphertext. It holds **no keys** and reads nothing —
the two nodes are encrypted end-to-end (X25519 + ChaCha20-Poly1305) over it.

Run it on any machine with a public IP (a cheap VPS). It has **no dependencies**
beyond a C++ compiler.

## Build (on the VPS, Linux)

```
sudo apt-get update && sudo apt-get install -y g++         # if needed
bash build.sh
```

## Run

```
./bitflashd 8434
```

Open the port in the firewall (both the VPS provider's panel and the OS):

```
sudo ufw allow 8434/tcp        # if ufw is enabled
```

## Keep it running (systemd)

Create `/etc/systemd/system/bitflashd.service`:

```
[Unit]
Description=Bitflash rendezvous relay
After=network.target

[Service]
ExecStart=/root/bitflash/relay/bitflashd 8434
Restart=always
User=root

[Install]
WantedBy=multi-user.target
```

Then:

```
sudo systemctl daemon-reload
sudo systemctl enable --now bitflashd
sudo systemctl status bitflashd
```

The relay's address for clients is then `YOUR_VPS_IP:8434` — that is what goes
into a node's descriptor as its `meeting_node`.
