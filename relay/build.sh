#!/usr/bin/env bash
# Build the Bitflash rendezvous relay (bitflashd) on Linux. No dependencies
# beyond a C++ compiler -- the relay only forwards bytes, it holds no keys.
set -e
g++ -std=gnu++14 -O2 -pthread bitflashd.cpp btfrv.cpp -o bitflashd
echo "built ./bitflashd  --  run it with:  ./bitflashd <port>   (e.g. ./bitflashd 8434)"
