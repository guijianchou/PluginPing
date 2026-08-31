// PluginPing - narrow interface consumed by the merged row compositor.
#pragma once

#include "../ping_src/include/PluginInterface.h"
#include <string>

namespace pluginping_shared {

class ICompositeSection
{
public:
    virtual ~ICompositeSection() = default;
    virtual IPluginItem* HostItem() = 0;
    virtual int MeasureCompositeWidth(void* hDC) const = 0;
    // One bounded, route-local tooltip fragment for the merged row.
    virtual std::wstring CompositeTooltip() const = 0;
};

} // namespace pluginping_shared
