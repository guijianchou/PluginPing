// PluginPing - deterministic channel state policy.
#pragma once

#include "MetricsData.h"

namespace pluginnetwork {

struct ChannelStateInput
{
    bool enabled = true;
    bool udp = false;
    bool tcpHealthy = false;
    bool fallbackPending = false;
    bool degraded = false;
    int successCount = 0;
    int sampleCount = 0;
    int windowCapacity = 1;
    int consecutiveFailures = 0;
    int maxFailCount = 3;
    int minSamples = 5;
};

inline ChannelState EvaluateChannelState(const ChannelStateInput& input)
{
    if (!input.enabled)
        return ChannelState::Disabled;
    if (input.fallbackPending)
        return ChannelState::Warming;

    const bool unsupported = input.udp && input.tcpHealthy &&
        input.successCount == 0 && input.sampleCount >= input.windowCapacity;
    if (unsupported)
        return ChannelState::Unsupported;
    if (input.consecutiveFailures >= input.maxFailCount)
        return ChannelState::Disconnected;
    if (input.degraded)
        return ChannelState::Degraded;
    if (input.sampleCount < input.minSamples)
        return ChannelState::Warming;
    return ChannelState::Ok;
}

} // namespace pluginnetwork
