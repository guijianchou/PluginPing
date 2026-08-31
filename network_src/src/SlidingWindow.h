// pluginNetwork - fixed-capacity probe-outcome window
#pragma once
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace pluginnetwork {

// Ring of the probe outcomes covering the configured time window.
// Each worker owns its own instance and is the only thread touching it; only
// the derived values are published into the shared snapshot.
class SlidingWindow
{
public:
    void Resize(int capacity)
    {
        if (capacity < 1)
            capacity = 1;
        if (capacity == Capacity())
            return;

        // Carry over the newest samples that still fit. Dropping everything on
        // a config edit would throw the row back to the warm-up placeholder for
        // a full window even though the measurements are still valid.
        const int keep = (std::min)(m_count, capacity);
        std::vector<Sample> kept;
        kept.reserve(static_cast<size_t>(keep));
        for (int i = m_count - keep; i < m_count; ++i)
            kept.push_back(At(i));

        m_samples.assign(static_cast<size_t>(capacity), Sample{});
        m_head = 0;
        m_count = 0;
        m_success = 0;
        for (const Sample& sample : kept)
        {
            m_samples[static_cast<size_t>(m_head)] = sample;
            m_head = (m_head + 1) % capacity;
            ++m_count;
            if (sample.ok)
                ++m_success;
        }
    }

    // Clears the measurement window without discarding session statistics.
    // Used when a suspend/resume gap makes the retained samples describe a
    // period that no longer says anything about the current link.
    void Reset()
    {
        std::fill(m_samples.begin(), m_samples.end(), Sample{});
        m_head = 0;
        m_count = 0;
        m_success = 0;
        m_consecutiveFailures = 0;
    }

    // A target change invalidates RTT comparisons but not the outcome history.
    // Keeping `ok` preserves the full stability window and failure streak while
    // forcing latency and jitter to warm up from the newly selected endpoint.
    void ClearRttHistory()
    {
        for (Sample& sample : m_samples)
            sample.rttMs = -1;
    }

    void Push(bool ok, int rttMs)
    {
        const int capacity = Capacity();
        if (capacity <= 0)
            return;

        if (m_count == capacity)
        {
            // The slot about to be overwritten is the oldest sample.
            if (m_samples[static_cast<size_t>(m_head)].ok)
                --m_success;
        }
        else
        {
            ++m_count;
        }

        m_samples[static_cast<size_t>(m_head)] = Sample{ ok, ok ? rttMs : -1 };
        m_head = (m_head + 1) % capacity;

        if (ok)
        {
            ++m_success;
            m_consecutiveFailures = 0;
            m_everSucceeded = true;
        }
        else
        {
            ++m_consecutiveFailures;
            m_maxConsecutiveFailures = (std::max)(m_maxConsecutiveFailures, m_consecutiveFailures);
        }
    }

public:
    int Capacity() const { return static_cast<int>(m_samples.size()); }
    int Count() const { return m_count; }
    int SuccessCount() const { return m_success; }
    int ConsecutiveFailures() const { return m_consecutiveFailures; }
    int MaxConsecutiveFailures() const { return m_maxConsecutiveFailures; }
    bool EverSucceeded() const { return m_everSucceeded; }

    // Truncated on purpose: a quality figure should never round up to a
    // friendlier colour band than the samples support.
    int StabilityPercent() const
    {
        if (m_count <= 0)
            return -1;
        return m_success * 100 / m_count;
    }

    // Mean of the newest `count` successful probes.
    int AverageRecentRttMs(int count) const
    {
        if (count <= 0)
            return -1;
        long long sum = 0;
        int taken = 0;
        for (int i = m_count - 1; i >= 0 && taken < count; --i)
        {
            const Sample& sample = At(i);
            if (!sample.ok || sample.rttMs < 0)
                continue;
            sum += sample.rttMs;
            ++taken;
        }
        if (taken == 0)
            return -1;
        return static_cast<int>((sum + taken / 2) / taken);
    }

    // Mean absolute difference between the newest `count` successful RTTs.
    // Timed-out probes carry no RTT, so they are skipped rather than treated as
    // zero. Iterating newest-to-oldest gives the same absolute pair deltas as
    // chronological order while avoiding a temporary buffer.
    int JitterMs(int count) const
    {
        if (count <= 1)
            return -1;
        long long sum = 0;
        int pairs = 0;
        int taken = 0;
        int previous = -1;
        for (int i = m_count - 1; i >= 0 && taken < count; --i)
        {
            const Sample& sample = At(i);
            if (!sample.ok || sample.rttMs < 0)
                continue;
            if (previous >= 0)
            {
                sum += std::abs(sample.rttMs - previous);
                ++pairs;
            }
            previous = sample.rttMs;
            ++taken;
        }
        if (pairs == 0)
            return -1;
        return static_cast<int>((sum + pairs / 2) / pairs);
    }

private:
    struct Sample
    {
        bool ok = false;
        int rttMs = -1;
    };

    // chronologicalIndex 0 is the oldest retained sample.
    const Sample& At(int chronologicalIndex) const
    {
        const int capacity = Capacity();
        const int index = ((m_head - m_count + chronologicalIndex) % capacity + capacity) % capacity;
        return m_samples[static_cast<size_t>(index)];
    }

    std::vector<Sample> m_samples;
    int m_head = 0;                     // next write position
    int m_count = 0;
    int m_success = 0;
    int m_consecutiveFailures = 0;
    int m_maxConsecutiveFailures = 0;
    bool m_everSucceeded = false;
};

} // namespace pluginnetwork
