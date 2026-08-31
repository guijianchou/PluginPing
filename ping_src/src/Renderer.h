// PluginPing - custom renderer
#pragma once
#include "Common.h"
#include "StatusData.h"
#include "ConfigManager.h"

namespace pluginping {

// Measure the widest aligned row using both Local and Remote values.
int MeasureSnapshotWidth(HDC hDC, const LocalRemoteSnapshot& snapshot, const PluginConfig& cfg);

// Draw one Local or Remote status row inside (x,y,w,h). dark_mode selects palette.
void DrawStatusRow(HDC hDC, int x, int y, int w, int h, bool darkMode,
                   const LocalRemoteSnapshot& snapshot, bool isLocal, const PluginConfig& cfg);

} // namespace pluginping
