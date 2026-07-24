// Copyright (c) 2009 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4717)
#endif

#ifdef _WIN32
// ---- Windows build (GUI wallet + miner) ----
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN 1
// winsock2.h must come before windows.h/wx to avoid pulling in winsock 1
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <wx/wx.h>
#include <wx/clipbrd.h>
#include <wx/snglinst.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <direct.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <process.h>
#include <malloc.h>
#include <memory>
#else
// ---- Linux/POSIX headless build (bitflash-node) ----
#include "compat.h"
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <memory>
#endif
#define BOUNDSCHECK 1
#include <sstream>
#include <string>
#include <vector>
#include <list>
#include <deque>
#include <map>
#include <set>
#include <algorithm>
#include <numeric>
#include <array>
#include <boost/foreach.hpp>
#pragma hdrstop
using namespace std;

// In 2009 windows.h provided min/max macros accepting mixed types; wxWidgets 3
// defines NOMINMAX, so we restore mixed-type versions here. For matching types,
// std::min/std::max remain preferred during overload resolution.
template<typename T1, typename T2>
inline typename std::common_type<T1, T2>::type min(const T1& a, const T2& b)
{
    return (a < b) ? a : b;
}
template<typename T1, typename T2>
inline typename std::common_type<T1, T2>::type max(const T1& a, const T2& b)
{
    return (a > b) ? a : b;
}



#include "serialize.h"
#include "uint256.h"
#include "util.h"
#include "key.h"
#include "bignum.h"
#include "base58.h"
#include "script.h"
#include "db.h"
#include "net.h"
#include "irc.h"
#include "nostr.h"
#include "randomx_pow.h"
#include "main.h"
#include "market.h"
#ifdef _WIN32
#include "uibase.h"
#include "ui.h"
#else
// Headless build has no GUI; the core only calls a couple of UI helpers,
// provided by headless.cpp (a no-op repaint and a timestamp formatter).
void MainFrameRepaint();
string DateTimeStr(int64 nTime);
#endif
