// Internal bridge used by the merged host. The child implementations keep
// their tested lifecycles and expose only the TrafficMonitor ABI object here.
#pragma once

#include "../ping_src/include/PluginInterface.h"

namespace pluginping {
ITMPlugin* GetPluginInstanceInternal();
void ShutdownPluginInternal();
}

namespace pluginnetwork {
ITMPlugin* GetPluginInstanceInternal();
void ShutdownPluginInternal();
}
