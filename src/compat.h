// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// POSIX/Linux compatibility shim. Lets the Windows-flavored core (Winsock,
// _beginthread, CRITICAL_SECTION, Sleep, ...) compile largely unchanged on
// Linux for the headless node/miner `bitflash-node`. Included by headers.h on
// non-Windows platforms only; the Windows build never sees this file.

#ifndef BITFLASH_COMPAT_H
#define BITFLASH_COMPAT_H
#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

// ---- Windows integer/type aliases used across the core ----
typedef unsigned int   DWORD;
typedef void*          HANDLE;
typedef int64_t        LARGE_INTEGER;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#ifndef _I64_MAX
#define _I64_MAX  0x7fffffffffffffffLL
#define _I64_MIN  (-0x7fffffffffffffffLL - 1)
#endif
#ifndef _UI64_MAX
#define _UI64_MAX 0xffffffffffffffffULL
#endif

// High-resolution counter (used only for optional timing in the core).
inline void QueryPerformanceCounter(LARGE_INTEGER* p)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *p = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---- socket layer (Winsock -> BSD sockets) ----
typedef int SOCKET;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#define closesocket(s) ::close(s)
inline int WSAGetLastError() { return errno; }

// Winsock error codes mapped to POSIX errno values
#define WSAEWOULDBLOCK  EWOULDBLOCK
#define WSAEMSGSIZE     EMSGSIZE
#define WSAEINTR        EINTR
#define WSAEINPROGRESS  EINPROGRESS
#define WSAEADDRINUSE   EADDRINUSE
#define WSAEALREADY     EALREADY
#define WSAECONNREFUSED ECONNREFUSED
#define WSAENOTSOCK     EBADF

// Only FIONBIO (non-blocking toggle) is used; implement it robustly via fcntl.
inline int ioctlsocket(SOCKET s, long cmd, u_long* argp)
{
    if (cmd == (long)FIONBIO)
    {
        int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) return -1;
        if (*argp) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
        return fcntl(s, F_SETFL, flags);
    }
    return ioctl(s, cmd, argp);
}

// WSAStartup/WSACleanup are no-ops outside Windows.
typedef int WSADATA;
inline int WSAStartup(unsigned short, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }
#ifndef MAKEWORD
#define MAKEWORD(a, b) 0
#endif

// ---- Sleep(ms) ----
inline void Sleep(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ---- threads: _beginthread / _endthread over pthreads ----
struct _bf_thread_ctx { void (*fn)(void*); void* arg; };
inline void* _bf_thread_trampoline(void* p)
{
    _bf_thread_ctx* c = (_bf_thread_ctx*)p;
    void (*fn)(void*) = c->fn;
    void* arg = c->arg;
    delete c;
    fn(arg);
    return NULL;
}
inline uintptr_t _beginthread(void (*start)(void*), unsigned, void* arg)
{
    _bf_thread_ctx* c = new _bf_thread_ctx;
    c->fn = start; c->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, _bf_thread_trampoline, c) != 0)
    {
        delete c;
        return (uintptr_t)-1;
    }
    pthread_detach(th);
    return (uintptr_t)th;
}
inline void _endthread() { pthread_exit(NULL); }

// ---- CRITICAL_SECTION over a recursive mutex (util.h's CCriticalSection
//      compiles unchanged on top of these) ----
typedef pthread_mutex_t CRITICAL_SECTION;
inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}
inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(cs); }
inline void EnterCriticalSection(CRITICAL_SECTION* cs)  { pthread_mutex_lock(cs); }
inline void LeaveCriticalSection(CRITICAL_SECTION* cs)  { pthread_mutex_unlock(cs); }
inline bool TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(cs) == 0; }

// ---- misc Windows CRT/debug helpers ----
inline void OutputDebugStringA(const char*) {}
inline void DebugBreak() {}
#ifndef _HEAPOK
#define _HEAPOK 0
#endif
inline int _heapchk() { return _HEAPOK; }
#define _vsnprintf vsnprintf
inline char* strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
#define _strlwr strlwr
inline int _mkdir(const char* p) { return ::mkdir(p, 0755); }

// Thread priority is a Windows nicety; on Linux the message-handler thread just
// runs at normal priority (harmless no-op).
inline void* GetCurrentThread() { return (void*)0; }
inline void SetThreadPriority(void*, int) {}
#ifndef THREAD_PRIORITY_BELOW_NORMAL
#define THREAD_PRIORITY_BELOW_NORMAL 0
#define THREAD_PRIORITY_LOWEST 0
#define THREAD_PRIORITY_NORMAL 0
#endif

// wx is always available (wx/wx.h included via headers.h); no shims needed.

#endif // !_WIN32
#endif // BITFLASH_COMPAT_H
