// pluginNetwork - display data model
//
// DrawItem and GetItemWidthEx only ever read an immutable MetricsSnapshot.
// The two worker threads own all measurement.
#pragma once
#include <string>

namespace pluginnetwork {

enum class ChannelKind
{
    Tcp,
    Udp
};

enum class ChannelState
{
    Disabled,       // switched off in the ini
    Warming,        // window has fewer than MinSamples entries
    Ok,             // measuring normally
    Degraded,       // healthy through a non-primary UDP fallback
    Disconnected,   // MaxFailCount consecutive probe failures
    Unsupported     // UDP only: the path never carried a single STUN reply
};

// Coarse quality band derived from stability, used to pick the accent colour.
enum class QualityTone
{
    Neutral,
    Good,
    Warn,
    Bad
};

struct ChannelSnapshot
{
    ChannelState state = ChannelState::Warming;
    int avgRttMs = -1;              // mean of the last N successful probes; -1 unknown
    int jitterMs = -1;              // mean |delta-RTT| among the last N successes; -1 unknown
    int stabilityPct = -1;          // truncated success ratio over the window; -1 unknown
    int sampleCount = 0;            // samples currently in the window
    int windowCapacity = 0;         // samples the window holds when full
    int successCount = 0;
    int consecutiveFailures = 0;
    int maxConsecutiveFailures = 0; // worst streak seen this session
    int egressChanges = 0;          // observed egress-IP switches (proxy failover)
    std::wstring egressIp;          // UDP only: STUN XOR-MAPPED-ADDRESS
    std::wstring activeTarget;      // endpoint currently being probed
    std::wstring diagnostic;        // last probe outcome, written to the debug log
};

struct MetricsSnapshot
{
    ChannelSnapshot tcp;
    ChannelSnapshot udp;
};

// Stability uses one truncated integer for RTT colour and the optional graph,
// so both surfaces cross their configured thresholds at the same sample.
inline QualityTone ToneForChannel(const ChannelSnapshot& channel,
                                  int greenThreshold, int yellowThreshold)
{
    switch (channel.state)
    {
    case ChannelState::Disabled:
    case ChannelState::Unsupported:
    case ChannelState::Warming:
        return QualityTone::Neutral;
    case ChannelState::Disconnected:
        return QualityTone::Bad;
    case ChannelState::Ok:
    case ChannelState::Degraded:
        break;
    }

    if (channel.stabilityPct < 0)
        return QualityTone::Neutral;
    if (channel.stabilityPct >= greenThreshold)
        return QualityTone::Good;
    if (channel.stabilityPct >= yellowThreshold)
        return QualityTone::Warn;
    return QualityTone::Bad;
}

// Preserve PluginPing's numeric latency bands for each displayed millisecond
// value. RTT and JIT call this independently with their own measurement.
inline QualityTone ToneForLatencyValue(int valueMs)
{
    if (valueMs < 0)
        return QualityTone::Neutral;
    if (valueMs <= 70)
        return QualityTone::Good;
    if (valueMs <= 149)
        return QualityTone::Warn;
    return QualityTone::Bad;
}

} // namespace pluginnetwork
