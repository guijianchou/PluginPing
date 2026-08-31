// pluginNetwork - overseas UDP path probe implementation
#include "Common.h"
#include "StunProbe.h"
#include <algorithm>
#include <string>
#include <vector>

namespace pluginnetwork {

namespace {

constexpr size_t kStunHeaderSize = 20;
constexpr size_t kTransactionIdSize = 12;
constexpr unsigned char kMagicCookie[4] = { 0x21, 0x12, 0xA4, 0x42 };

constexpr unsigned short kBindingRequest = 0x0001;
constexpr unsigned short kBindingSuccess = 0x0101;
constexpr unsigned short kBindingError = 0x0111;

constexpr unsigned short kAttrMappedAddress = 0x0001;
constexpr unsigned short kAttrXorMappedAddress = 0x0020;
constexpr unsigned char kFamilyIpv4 = 0x01;

constexpr unsigned char kSocksVersion = 0x05;
constexpr unsigned char kSocksNoAuth = 0x00;
constexpr unsigned char kSocksUdpAssociate = 0x03;
constexpr unsigned char kSocksIpv4 = 0x01;
constexpr unsigned char kSocksDomain = 0x03;

unsigned short ReadU16(const unsigned char* data)
{
    return static_cast<unsigned short>((data[0] << 8) | data[1]);
}

bool FillRandom(unsigned char* buffer, size_t size)
{
    if (BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return true;

    static volatile long long counter = 0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const unsigned long long count = static_cast<unsigned long long>(
        InterlockedIncrement64(&counter));
    const unsigned long long seed = static_cast<unsigned long long>(now.QuadPart) ^
                                    (count * 0x9E3779B97F4A7C15ULL);
    for (size_t i = 0; i < size; ++i)
        buffer[i] = static_cast<unsigned char>((seed >> ((i % 8) * 8)) ^ (i * 31 + 7));
    return true;
}

std::wstring FormatDottedQuad(const unsigned char* address)
{
    return std::to_wstring(address[0]) + L"." + std::to_wstring(address[1]) + L"." +
           std::to_wstring(address[2]) + L"." + std::to_wstring(address[3]);
}

std::wstring ExtractMappedAddress(const unsigned char* message, size_t size)
{
    if (size < kStunHeaderSize)
        return std::wstring();
    const size_t declared = kStunHeaderSize + ReadU16(message + 2);
    const size_t limit = (std::min)(size, declared);

    std::wstring legacy;
    size_t offset = kStunHeaderSize;
    while (offset + 4 <= limit)
    {
        const unsigned short type = ReadU16(message + offset);
        const unsigned short length = ReadU16(message + offset + 2);
        const size_t value = offset + 4;
        if (value + length > limit)
            break;

        if (type == kAttrXorMappedAddress && length >= 8 && message[value + 1] == kFamilyIpv4)
        {
            unsigned char address[4];
            for (size_t i = 0; i < 4; ++i)
                address[i] = message[value + 4 + i] ^ kMagicCookie[i];
            return FormatDottedQuad(address);
        }
        if (type == kAttrMappedAddress && length >= 8 && message[value + 1] == kFamilyIpv4 &&
            legacy.empty())
        {
            legacy = FormatDottedQuad(message + value + 4);
        }

        offset = value + ((static_cast<size_t>(length) + 3) & ~static_cast<size_t>(3));
    }
    return legacy;
}

bool ConnectWithTimeout(SOCKET socketHandle, const sockaddr_in& address, int timeoutMs)
{
    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0)
        return false;

    const int connected = connect(socketHandle, reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
    if (connected != 0)
    {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL)
            return false;

        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socketHandle, &writable);
        FD_SET(socketHandle, &failed);
        const int budget = (std::max)(timeoutMs, 50);
        timeval wait{ budget / 1000, (budget % 1000) * 1000 };
        const int ready = select(0, nullptr, &writable, &failed, &wait);
        if (ready == 0)
        {
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }
        if (ready != 1 || FD_ISSET(socketHandle, &failed) ||
            !FD_ISSET(socketHandle, &writable))
            return false;

        int socketError = 0;
        int errorSize = sizeof(socketError);
        if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &errorSize) != 0 ||
            socketError != 0)
        {
            if (socketError != 0)
                WSASetLastError(socketError);
            return false;
        }
    }

    nonBlocking = 0;
    return ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == 0;
}

void SetSocketTimeouts(SOCKET socketHandle, int timeoutMs)
{
    const DWORD timeout = static_cast<DWORD>((std::max)(timeoutMs, 50));
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

bool SendAll(SOCKET socketHandle, const unsigned char* data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const int chunk = send(socketHandle, reinterpret_cast<const char*>(data + sent),
                               static_cast<int>(size - sent), 0);
        if (chunk <= 0)
            return false;
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

bool ReceiveExact(SOCKET socketHandle, unsigned char* data, size_t size)
{
    size_t received = 0;
    while (received < size)
    {
        const int chunk = recv(socketHandle, reinterpret_cast<char*>(data + received),
                               static_cast<int>(size - received), 0);
        if (chunk <= 0)
            return false;
        received += static_cast<size_t>(chunk);
    }
    return true;
}

bool WideDomainToUtf8(const std::wstring& host, std::string& domain)
{
    if (host.empty())
        return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, host.c_str(),
                                          static_cast<int>(host.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0 || bytes > 255)
        return false;
    domain.resize(static_cast<size_t>(bytes));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, host.c_str(),
                               static_cast<int>(host.size()), domain.data(), bytes,
                               nullptr, nullptr) == bytes;
}

bool Utf8DomainToWide(const unsigned char* data, size_t size, std::wstring& host)
{
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX))
        return false;
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          reinterpret_cast<const char*>(data),
                                          static_cast<int>(size), nullptr, 0);
    if (chars <= 0)
        return false;
    host.resize(static_cast<size_t>(chars));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               reinterpret_cast<const char*>(data),
                               static_cast<int>(size), host.data(), chars) == chars;
}

} // namespace

UdpProbe::~UdpProbe()
{
    Close();
}

void UdpProbe::Configure(const std::wstring& server, unsigned short defaultPort,
                           const PluginConfig& cfg, const ProxyRoute& route,
                           UdpProbeProtocol protocol)
{
    std::wstring host;
    unsigned short port = defaultPort;
    if (!SplitHostPort(server, defaultPort, host, port))
    {
        host.clear();
        port = defaultPort;
    }

    const std::wstring routeKey = route.UdpKey();
    const bool socketConfigChanged = host != m_host || port != m_port || protocol != m_protocol ||
                                      routeKey != m_routeKey || cfg.timeoutMs != m_timeoutMs;
    m_host = host;
    m_port = port;
    m_target = m_host.empty() ? std::wstring(L"--") : (m_host + L":" + std::to_wstring(m_port));
    m_timeoutMs = cfg.timeoutMs;
    m_dnsRefreshMs = cfg.dnsRefreshMs;
    m_recycleAfterFailures = cfg.maxFailCount;
    m_protocol = protocol;
    m_route = route;
    m_routeKey = routeKey;

    if (socketConfigChanged)
    {
        DropSocket();
        m_resolved = false;
        m_lastResolveTick = 0;
        m_failStreak = 0;
        m_lastSocketDiagnostic.clear();
    }
}

void UdpProbe::Close()
{
    std::lock_guard<std::mutex> lk(m_socketMutex);
    m_cancelled = true;
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_socksControl != INVALID_SOCKET)
    {
        closesocket(m_socksControl);
        m_socksControl = INVALID_SOCKET;
    }
}

void UdpProbe::Cancel()
{
    std::lock_guard<std::mutex> lk(m_socketMutex);
    m_cancelled = true;
    // Keep the handles alive until the worker has left send/select/recv. A
    // concurrent closesocket could let Windows reuse the numeric handle while
    // Run() still holds a local copy. shutdown wakes the blocking I/O without
    // creating that close/use race; Close() owns final destruction after join.
    if (m_socket != INVALID_SOCKET)
        shutdown(m_socket, SD_BOTH);
    if (m_socksControl != INVALID_SOCKET)
        shutdown(m_socksControl, SD_BOTH);
}

void UdpProbe::ResetTransport()
{
    DropSocket();
    // The cached A record belongs to the route that resolved it. Keeping it
    // would let the rebuilt socket reconnect to the address the previous route
    // returned for up to DnsRefreshMs - the stale link this reset exists to
    // break. Configure() already does this when the socket config changes; a
    // route change reaches here with an identical host and port.
    m_resolved = false;
    m_lastResolveTick = 0;
    m_failStreak = 0;
    m_lastSocketDiagnostic.clear();
}

void UdpProbe::DropSocket()
{
    std::lock_guard<std::mutex> lk(m_socketMutex);
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_socksControl != INVALID_SOCKET)
    {
        closesocket(m_socksControl);
        m_socksControl = INVALID_SOCKET;
    }
}

bool UdpProbe::EnsureResolved()
{
    if (m_host.empty())
        return false;

    const unsigned long long now = GetTickCount64();
    const bool stale = m_lastResolveTick == 0 ||
                       now - m_lastResolveTick >= static_cast<unsigned long long>(m_dnsRefreshMs);
    if (m_resolved && !stale)
        return true;

    sockaddr_in resolved{};
    if (!ResolveIpv4(m_host, m_port, m_timeoutMs, resolved))
    {
        m_lastResolveTick = now;
        return m_resolved;
    }

    if (!m_resolved || resolved.sin_addr.S_un.S_addr != m_address.sin_addr.S_un.S_addr)
    {
        m_address = resolved;
        DropSocket();
    }
    m_resolved = true;
    m_lastResolveTick = now;
    return true;
}

bool UdpProbe::EnsureSocksTunnel()
{
    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        if (m_cancelled)
            return false;
        if (m_socket != INVALID_SOCKET && m_socksControl != INVALID_SOCKET)
            return true;
    }

    DropSocket();
    if (!EnsureWinsockStarted())
    {
        m_lastSocketDiagnostic = L"udp fail winsock startup";
        return false;
    }

    sockaddr_in proxyAddress{};
    if (!ResolveIpv4(m_route.socksHost, m_route.socksPort, m_timeoutMs, proxyAddress))
    {
        m_lastSocketDiagnostic = L"udp fail socks5 proxy dns " + m_route.socksHost;
        return false;
    }

    SOCKET control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (control == INVALID_SOCKET)
    {
        m_lastSocketDiagnostic = L"udp fail socks5 control socket err " +
                                 std::to_wstring(WSAGetLastError());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        if (m_cancelled)
        {
            closesocket(control);
            return false;
        }
        m_socksControl = control;
    }

    SetSocketTimeouts(control, m_timeoutMs);
    if (!ConnectWithTimeout(control, proxyAddress, m_timeoutMs))
    {
        m_lastSocketDiagnostic = L"udp fail socks5 connect err " +
                                 std::to_wstring(WSAGetLastError());
        DropSocket();
        return false;
    }

    const unsigned char greeting[] = { kSocksVersion, 0x01, kSocksNoAuth };
    unsigned char greetingReply[2] = {};
    if (!SendAll(control, greeting, sizeof(greeting)) ||
        !ReceiveExact(control, greetingReply, sizeof(greetingReply)) ||
        greetingReply[0] != kSocksVersion || greetingReply[1] != kSocksNoAuth)
    {
        m_lastSocketDiagnostic = L"udp fail socks5 no-auth negotiation";
        DropSocket();
        return false;
    }

    // RFC 1928 UDP ASSOCIATE with 0.0.0.0:0 asks the server to bind the relay
    // for the source address of this control connection.
    const unsigned char associate[] = {
        kSocksVersion, kSocksUdpAssociate, 0x00, kSocksIpv4,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    unsigned char responseHeader[4] = {};
    if (!SendAll(control, associate, sizeof(associate)) ||
        !ReceiveExact(control, responseHeader, sizeof(responseHeader)) ||
        responseHeader[0] != kSocksVersion || responseHeader[2] != 0x00)
    {
        m_lastSocketDiagnostic = L"udp fail socks5 udp-associate response";
        DropSocket();
        return false;
    }
    if (responseHeader[1] != 0x00)
    {
        m_lastSocketDiagnostic = L"udp fail socks5 udp-associate rejected " +
                                 std::to_wstring(responseHeader[1]);
        DropSocket();
        return false;
    }

    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    if (responseHeader[3] == kSocksIpv4)
    {
        unsigned char address[4] = {};
        if (!ReceiveExact(control, address, sizeof(address)))
        {
            m_lastSocketDiagnostic = L"udp fail socks5 relay address";
            DropSocket();
            return false;
        }
        memcpy(&relay.sin_addr, address, sizeof(address));
        if (relay.sin_addr.S_un.S_addr == INADDR_ANY)
            relay.sin_addr = proxyAddress.sin_addr;
    }
    else if (responseHeader[3] == kSocksDomain)
    {
        unsigned char length = 0;
        if (!ReceiveExact(control, &length, 1) || length == 0)
        {
            m_lastSocketDiagnostic = L"udp fail socks5 relay domain";
            DropSocket();
            return false;
        }
        std::vector<unsigned char> encoded(length);
        std::wstring relayHost;
        if (!ReceiveExact(control, encoded.data(), encoded.size()) ||
            !Utf8DomainToWide(encoded.data(), encoded.size(), relayHost) ||
            !ResolveIpv4(relayHost, 1, m_timeoutMs, relay))
        {
            m_lastSocketDiagnostic = L"udp fail socks5 relay dns";
            DropSocket();
            return false;
        }
    }
    else
    {
        m_lastSocketDiagnostic = L"udp fail socks5 relay address family";
        DropSocket();
        return false;
    }

    unsigned char portBytes[2] = {};
    if (!ReceiveExact(control, portBytes, sizeof(portBytes)))
    {
        m_lastSocketDiagnostic = L"udp fail socks5 relay port";
        DropSocket();
        return false;
    }
    memcpy(&relay.sin_port, portBytes, sizeof(portBytes));
    if (relay.sin_port == 0)
    {
        m_lastSocketDiagnostic = L"udp fail socks5 relay returned port zero";
        DropSocket();
        return false;
    }

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET ||
        connect(udp, reinterpret_cast<const sockaddr*>(&relay), sizeof(relay)) != 0)
    {
        const int error = WSAGetLastError();
        if (udp != INVALID_SOCKET)
            closesocket(udp);
        m_lastSocketDiagnostic = L"udp fail socks5 relay socket err " +
                                 std::to_wstring(error);
        DropSocket();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        if (m_cancelled || m_socksControl != control)
        {
            closesocket(udp);
            return false;
        }
        m_socket = udp;
    }
    m_lastSocketDiagnostic.clear();
    return true;
}

bool UdpProbe::EnsureSocket()
{
    if (m_route.udp == UdpProxyRoute::Unavailable)
    {
        m_lastSocketDiagnostic = L"udp fail " +
            (m_route.udpDiagnostic.empty() ? std::wstring(L"SOCKS5 proxy unavailable") :
                                             m_route.udpDiagnostic);
        return false;
    }
    if (m_route.udp == UdpProxyRoute::Socks5)
        return EnsureSocksTunnel();

    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        if (m_cancelled)
            return false;
        if (m_socket != INVALID_SOCKET)
            return true;
    }

    if (!EnsureWinsockStarted())
        return false;

    SOCKET created = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (created == INVALID_SOCKET)
        return false;
    if (connect(created, reinterpret_cast<const sockaddr*>(&m_address), sizeof(m_address)) != 0)
    {
        closesocket(created);
        return false;
    }

    std::lock_guard<std::mutex> lk(m_socketMutex);
    if (m_cancelled)
    {
        closesocket(created);
        return false;
    }
    m_socket = created;
    return true;
}

void UdpProbe::DrainStaleDatagrams()
{
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        sock = m_socket;
    }
    if (sock == INVALID_SOCKET)
        return;

    char buffer[2048];
    for (int i = 0; i < 16; ++i)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        timeval immediate{ 0, 0 };
        if (select(0, &readable, nullptr, nullptr, &immediate) != 1)
            return;
        if (recv(sock, buffer, static_cast<int>(sizeof(buffer)), 0) <= 0)
            return;
    }
}

bool UdpProbe::BuildOutboundDatagram(const unsigned char* payload, size_t payloadSize,
                                      std::vector<unsigned char>& datagram) const
{
    if (m_route.udp != UdpProxyRoute::Socks5)
    {
        datagram.assign(payload, payload + payloadSize);
        return true;
    }

    std::string domain;
    if (!WideDomainToUtf8(m_host, domain))
        return false;
    datagram.clear();
    datagram.reserve(7 + domain.size() + payloadSize);
    datagram.push_back(0x00); // RSV
    datagram.push_back(0x00);
    datagram.push_back(0x00); // FRAG: fragmentation is not supported by RFC 1928 clients
    datagram.push_back(kSocksDomain);
    datagram.push_back(static_cast<unsigned char>(domain.size()));
    datagram.insert(datagram.end(), domain.begin(), domain.end());
    datagram.push_back(static_cast<unsigned char>(m_port >> 8));
    datagram.push_back(static_cast<unsigned char>(m_port & 0xFF));
    datagram.insert(datagram.end(), payload, payload + payloadSize);
    return true;
}

bool UdpProbe::LocateInboundPayload(const unsigned char* datagram, size_t datagramSize,
                                     const unsigned char*& payload, size_t& payloadSize) const
{
    if (m_route.udp != UdpProxyRoute::Socks5)
    {
        payload = datagram;
        payloadSize = datagramSize;
        return true;
    }
    if (datagramSize < 4 || datagram[0] != 0 || datagram[1] != 0 || datagram[2] != 0)
        return false;

    size_t offset = 4;
    if (datagram[3] == kSocksIpv4)
    {
        offset += 4;
    }
    else if (datagram[3] == kSocksDomain)
    {
        if (offset >= datagramSize)
            return false;
        offset += 1 + datagram[offset];
    }
    else
    {
        return false;
    }

    if (offset + 2 > datagramSize)
        return false;
    offset += 2;
    payload = datagram + offset;
    payloadSize = datagramSize - offset;
    return true;
}

ProbeResult UdpProbe::Run()
{
    ProbeResult result;

    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        if (m_cancelled)
        {
            result.cancelled = true;
            result.diagnostic = L"udp cancelled";
            return result;
        }
    }

    if (m_host.empty())
    {
        result.diagnostic = L"udp fail invalid server";
        return result;
    }
    if (m_route.udp == UdpProxyRoute::Direct && !EnsureResolved())
    {
        ++m_failStreak;
        result.diagnostic = L"udp fail dns " + m_host;
        return result;
    }
    if (!EnsureSocket())
    {
        {
            std::lock_guard<std::mutex> lk(m_socketMutex);
            if (m_cancelled)
            {
                result.cancelled = true;
                result.diagnostic = L"udp cancelled";
                return result;
            }
        }
        ++m_failStreak;
        result.diagnostic = m_lastSocketDiagnostic.empty()
                                ? L"udp fail socket err " + std::to_wstring(WSAGetLastError())
                                : m_lastSocketDiagnostic;
        return result;
    }

    DrainStaleDatagrams();

    unsigned char request[32] = {};
    size_t requestSize = kStunHeaderSize;
    if (m_protocol == UdpProbeProtocol::Echo)
    {
        const unsigned char prefix[8] = { 'P', 'P', 'M', 'E', 'C', 'H', 'O', '1' };
        memcpy(request, prefix, sizeof(prefix));
        FillRandom(request + sizeof(prefix), sizeof(request) - sizeof(prefix));
        requestSize = sizeof(request);
    }
    else
    {
        request[0] = static_cast<unsigned char>(kBindingRequest >> 8);
        request[1] = static_cast<unsigned char>(kBindingRequest & 0xFF);
        memcpy(request + 4, kMagicCookie, sizeof(kMagicCookie));
        FillRandom(request + 8, kTransactionIdSize);
    }

    std::vector<unsigned char> outbound;
    if (!BuildOutboundDatagram(request, requestSize, outbound))
    {
        ++m_failStreak;
        result.diagnostic = L"udp fail socks5 target encoding";
        return result;
    }

    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(m_socketMutex);
        sock = m_socket;
        if (sock == INVALID_SOCKET)
        {
            result.cancelled = m_cancelled;
            result.diagnostic = m_cancelled ? L"udp cancelled" : L"udp fail socket closed";
            return result;
        }
    }

    Stopwatch watch;
    watch.Start();

    if (send(sock, reinterpret_cast<const char*>(outbound.data()),
             static_cast<int>(outbound.size()), 0) != static_cast<int>(outbound.size()))
    {
        const int error = WSAGetLastError();
        {
            std::lock_guard<std::mutex> lk(m_socketMutex);
            if (m_cancelled)
            {
                result.cancelled = true;
                result.diagnostic = L"udp cancelled";
                return result;
            }
        }
        ++m_failStreak;
        if (m_failStreak >= m_recycleAfterFailures)
        {
            DropSocket();
            m_failStreak = 0;
        }
        result.diagnostic = L"udp fail send err " + std::to_wstring(error);
        return result;
    }

    const int budgetMs = (std::max)(m_timeoutMs, 50);
    for (;;)
    {
        const int elapsed = watch.ElapsedMs();
        const int remaining = budgetMs - elapsed;
        if (remaining <= 0)
            break;

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        timeval wait{ remaining / 1000, (remaining % 1000) * 1000 };
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == 0)
            break;
        if (ready != 1)
        {
            const int error = WSAGetLastError();
            std::lock_guard<std::mutex> lk(m_socketMutex);
            if (m_cancelled)
            {
                result.cancelled = true;
                result.diagnostic = L"udp cancelled";
                return result;
            }
            ++m_failStreak;
            result.diagnostic = L"udp fail select err " + std::to_wstring(error);
            break;
        }

        unsigned char response[2048];
        const int received = recv(sock, reinterpret_cast<char*>(response),
                                  static_cast<int>(sizeof(response)), 0);
        if (received <= 0)
        {
            const int error = WSAGetLastError();
            {
                std::lock_guard<std::mutex> lk(m_socketMutex);
                if (m_cancelled)
                {
                    result.cancelled = true;
                    result.diagnostic = L"udp cancelled";
                    return result;
                }
            }
            ++m_failStreak;
            result.diagnostic = L"udp fail recv err " + std::to_wstring(error);
            break;
        }

        const unsigned char* message = nullptr;
        size_t size = 0;
        if (!LocateInboundPayload(response, static_cast<size_t>(received), message, size))
            continue;

        const int rtt = watch.ElapsedMs();
        if (m_protocol == UdpProbeProtocol::Echo)
        {
            if (size != requestSize || memcmp(message, request, requestSize) != 0)
                continue;
            m_failStreak = 0;
            result.ok = true;
            result.rttMs = rtt;
            result.diagnostic = L"udp echo ok " + std::to_wstring(rtt) + L" ms " + m_target;
            if (m_route.udp == UdpProxyRoute::Socks5)
                result.diagnostic += L" via socks5 " + m_route.socksHost + L":" +
                                     std::to_wstring(m_route.socksPort);
            return result;
        }

        if (size < kStunHeaderSize)
            continue;
        const size_t declaredBody = ReadU16(message + 2);
        if ((declaredBody & 3) != 0 || declaredBody > size - kStunHeaderSize)
            continue;
        if ((message[0] & 0xC0) != 0)
            continue;
        if (memcmp(message + 4, kMagicCookie, sizeof(kMagicCookie)) != 0)
            continue;
        if (memcmp(message + 8, request + 8, kTransactionIdSize) != 0)
            continue;

        const unsigned short type = ReadU16(message);
        if (type == kBindingError)
        {
            ++m_failStreak;
            result.diagnostic = L"udp fail stun error response";
            break;
        }
        if (type != kBindingSuccess)
            continue;

        m_failStreak = 0;
        result.ok = true;
        result.rttMs = rtt;
        result.egressIp = ExtractMappedAddress(message, size);
        result.diagnostic = L"udp stun ok " + std::to_wstring(rtt) + L" ms " + m_target;
        if (m_route.udp == UdpProxyRoute::Socks5)
            result.diagnostic += L" via socks5 " + m_route.socksHost + L":" +
                                 std::to_wstring(m_route.socksPort);
        if (!result.egressIp.empty())
            result.diagnostic += L" egress " + result.egressIp;
        return result;
    }

    if (result.diagnostic.empty())
    {
        ++m_failStreak;
        result.diagnostic = L"udp fail timeout " + std::to_wstring(budgetMs) + L" ms " + m_target;
    }

    if (m_failStreak >= m_recycleAfterFailures)
    {
        DropSocket();
        m_failStreak = 0;
    }
    return result;
}

} // namespace pluginnetwork
