// pluginNetwork - custom renderer
#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "MetricsData.h"

namespace pluginnetwork {

// Width uses the larger of a compact baseline and the current two-row content.
int MeasureChannelWidth(HDC hDC, ChannelKind kind, const MetricsSnapshot& snapshot,
                        const PluginConfig& cfg, bool showLabel = true,
                        bool currentOnly = false);

// Draws one channel inside (x,y,w,h). RTT carries the semantic quality colour;
// all normal text inherits the host drawing context.
void DrawChannel(HDC hDC, int x, int y, int w, int h, bool darkMode,
                 ChannelKind kind, const MetricsSnapshot& snapshot, const PluginConfig& cfg,
                 bool showLabel = true);

} // namespace pluginnetwork
