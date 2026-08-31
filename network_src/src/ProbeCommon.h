// pluginNetwork - shared probe utilities
#pragma once
#include "Common.h"
#include <string>

namespace pluginnetwork {

// Outcome of a single probe. `cancelled` is kept separate from a plain failure:
// a probe aborted by shutdown says nothing about the link and must not be
// pushed into the stability window.
struct ProbeResult
{
    bool ok = false;
    bool cancelled = false;
    int rttMs = -1;
    std::wstring diagnostic;
    std::wstring egressIp;   // STUN only: XOR-MAPPED-ADDRESS
};

// Round-trip timing must not use GetTickCount64: its ~15.6 ms resolution would
// quantise a 45 ms STUN reply and reduce the reported jitter to noise.
class Stopwatch
{
public:
    void Start();
    int ElapsedMs() const;

private:
    long long m_start = 0;
};

// Idempotent WSAStartup. Deliberately never paired with WSACleanup: the DLL is
// pinned for the process lifetime, and tearing Winsock down from a static
// destructor would race the workers.
bool EnsureWinsockStarted();

// Bounded IPv4 resolution. Plain getaddrinfo cannot be interrupted, so a wedged
// resolver would keep the worker inside a join and hang host shutdown.
bool ResolveIpv4(const std::wstring& host, unsigned short port, int timeoutMs, sockaddr_in& out);

// Formats an in_addr (network byte order) as dotted quad.
std::wstring FormatIpv4(unsigned long networkOrderAddress);

// Splits "host", "host:port" and "scheme://host[:port]/path" into host and port.
// Returns false when no host can be extracted.
bool SplitHostPort(const std::wstring& value, unsigned short defaultPort,
                   std::wstring& hostOut, unsigned short& portOut);

} // namespace pluginnetwork
