// pluginNetwork - shared probe utilities implementation
#include "Common.h"
#include "ProbeCommon.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <new>

namespace pluginnetwork {

namespace {

long long QpcFrequency()
{
    static const long long frequency = []() -> long long {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0)
            return 1;
        return value.QuadPart;
    }();
    return frequency;
}

long long QpcNow()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

struct PendingDnsQuery
{
    PendingDnsQuery()
    {
        completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped.hEvent = completed;
    }

    ~PendingDnsQuery()
    {
        if (result)
            FreeAddrInfoExW(result);
        if (completed)
            CloseHandle(completed);
    }

    HANDLE completed = nullptr;
    OVERLAPPED overlapped{};
    PADDRINFOEXW result = nullptr;
    HANDLE cancel = nullptr;
    std::wstring host;
    ADDRINFOEXW hints{};
    PendingDnsQuery* next = nullptr;
};

class DnsQueryQuarantine
{
public:
    bool ReapAndCanStart() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            PendingDnsQuery** current = &m_head;
            while (*current)
            {
                PendingDnsQuery* query = *current;
                if (WaitForSingleObject(query->completed, 0) == WAIT_OBJECT_0)
                {
                    *current = query->next;
                    (void)GetAddrInfoExOverlappedResult(&query->overlapped);
                    delete query;
                }
                else
                {
                    current = &query->next;
                }
            }
            // Do not accumulate resolver requests if a cancelled query remains
            // owned by Winsock. The STUN worker can retry after it completes.
            return m_head == nullptr;
        }
        catch (...)
        {
            return false;
        }
    }

    void Add(std::unique_ptr<PendingDnsQuery> query) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            query->next = m_head;
            m_head = query.release();
        }
        catch (...)
        {
            // A live overlapped operation must retain all of its storage even
            // if linking it into the process-lifetime quarantine fails.
            (void)query.release();
        }
    }

private:
    std::mutex m_mutex;
    PendingDnsQuery* m_head = nullptr;
};

DnsQueryQuarantine* GetDnsQueryQuarantine() noexcept
{
    // A cancelled DNS operation may complete after plug-in shutdown. The DLL
    // is pinned before workers start, so this intentionally leaked owner keeps
    // its OVERLAPPED, hostname and hints valid for as long as Winsock needs.
    static DnsQueryQuarantine* quarantine = []() noexcept {
        try
        {
            return new (std::nothrow) DnsQueryQuarantine();
        }
        catch (...)
        {
            return static_cast<DnsQueryQuarantine*>(nullptr);
        }
    }();
    return quarantine;
}

} // namespace

void Stopwatch::Start()
{
    m_start = QpcNow();
}

int Stopwatch::ElapsedMs() const
{
    long long ticks = QpcNow() - m_start;
    if (ticks < 0)
        ticks = 0;
    const long long frequency = QpcFrequency();
    return static_cast<int>((ticks * 1000 + frequency / 2) / frequency);
}

bool EnsureWinsockStarted()
{
    static const bool started = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

std::wstring FormatIpv4(unsigned long networkOrderAddress)
{
    const unsigned long raw = networkOrderAddress;
    return std::to_wstring(raw & 0xFF) + L"." +
           std::to_wstring((raw >> 8) & 0xFF) + L"." +
           std::to_wstring((raw >> 16) & 0xFF) + L"." +
           std::to_wstring((raw >> 24) & 0xFF);
}

bool SplitHostPort(const std::wstring& value, unsigned short defaultPort,
                   std::wstring& hostOut, unsigned short& portOut)
{
    std::wstring rest = value;

    const size_t scheme = rest.find(L"://");
    if (scheme != std::wstring::npos)
        rest = rest.substr(scheme + 3);

    const size_t slash = rest.find_first_of(L"/?#");
    if (slash != std::wstring::npos)
        rest = rest.substr(0, slash);

    while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t'))
        rest.erase(rest.begin());
    while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t'))
        rest.pop_back();

    if (rest.empty())
        return false;

    unsigned short port = defaultPort;
    const size_t colon = rest.rfind(L':');
    if (colon != std::wstring::npos && colon + 1 < rest.size())
    {
        bool allDigits = true;
        for (size_t i = colon + 1; i < rest.size(); ++i)
        {
            if (rest[i] < L'0' || rest[i] > L'9')
            {
                allDigits = false;
                break;
            }
        }
        if (allDigits)
        {
            const int parsed = _wtoi(rest.c_str() + colon + 1);
            if (parsed > 0 && parsed <= 65535)
            {
                port = static_cast<unsigned short>(parsed);
                rest = rest.substr(0, colon);
            }
        }
    }

    if (rest.empty())
        return false;
    if (rest.find(L':') != std::wstring::npos)
        return false;

    hostOut = rest;
    portOut = port;
    return true;
}

bool ResolveIpv4(const std::wstring& host, unsigned short port, int timeoutMs, sockaddr_in& out)
{
    if (host.empty() || !EnsureWinsockStarted())
        return false;

    // A dotted quad needs no resolver round trip at all.
    IN_ADDR literal{};
    if (InetPtonW(AF_INET, host.c_str(), &literal) == 1)
    {
        out = sockaddr_in{};
        out.sin_family = AF_INET;
        out.sin_port = htons(port);
        out.sin_addr = literal;
        return true;
    }

    DnsQueryQuarantine* quarantine = GetDnsQueryQuarantine();
    if (quarantine && !quarantine->ReapAndCanStart())
        return false;

    // GetAddrInfoExW only honours a timeout for overlapped requests, so the
    // query is issued asynchronously and cancelled once timeoutMs elapses. All
    // arguments referenced by Winsock must outlive completion, including the
    // hostname and hints, so the request owns every one of them on the heap.
    std::unique_ptr<PendingDnsQuery> request(new (std::nothrow) PendingDnsQuery());
    if (!request || !request->completed)
        return false;
    request->host = host;
    request->hints.ai_family = AF_INET;
    request->hints.ai_socktype = SOCK_DGRAM;

    const INT queued = GetAddrInfoExW(request->host.c_str(), nullptr, NS_DNS, nullptr,
                                      &request->hints,
                                      &request->result, nullptr, &request->overlapped,
                                      nullptr, &request->cancel);

    bool resolved = false;
    if (queued == NO_ERROR)
    {
        resolved = true;
    }
    else if (queued == WSA_IO_PENDING)
    {
        const DWORD wait = static_cast<DWORD>((std::max)(timeoutMs, 1));
        if (WaitForSingleObject(request->completed, wait) != WAIT_OBJECT_0)
        {
            GetAddrInfoExCancel(&request->cancel);
            if (WaitForSingleObject(request->completed, 0) != WAIT_OBJECT_0)
            {
                if (quarantine)
                    quarantine->Add(std::move(request));
                else
                    (void)request.release();
                return false;
            }
        }
        resolved = GetAddrInfoExOverlappedResult(&request->overlapped) == NO_ERROR;
    }

    bool found = false;
    if (resolved)
    {
        for (PADDRINFOEXW info = request->result; info; info = info->ai_next)
        {
            if (info->ai_family != AF_INET || !info->ai_addr)
                continue;
            out = *reinterpret_cast<const sockaddr_in*>(info->ai_addr);
            out.sin_port = htons(port);
            found = true;
            break;
        }
    }

    return found;
}

} // namespace pluginnetwork
