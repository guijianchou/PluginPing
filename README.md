# PluginPing

`pluginping` is the canonical development directory. The Visual Studio project,
DLL, TrafficMonitor plugin name, item IDs and configuration surface remain
`PluginPing`.

Version: `1.0.14`
Platform: Windows x64 / TrafficMonitor plugin API 7  
Toolchain: Visual Studio 2026, C++17

## Display model

TrafficMonitor sees exactly two custom-drawn items:

| Item | One-row content | Stable item ID |
| --- | --- | --- |
| `DIRECT` | Direct route code + IP, then TCP status/RTT/jitter | `PluginPingLocal` |
| `PROXY` | Cloudflare route code + IP, then UDP status/RTT/jitter | `PluginPingRemote` |

The embedded probe sections do not draw separate `TCP` or `UDP` labels. A normal
row therefore reads conceptually as:

```text
DIRECT CN  203.0.113.10   RTT: 31 ms   JIT: 2
PROXY  US  198.51.100.8   RTT: 146 ms  JIT: 14
```

This is not four adjacent items. Width measurement and painting happen through
two composite `IPluginItem` objects, so the status bar does not pay for four host
items or duplicate route labels. IP text uses the same normal font weight as the
route code. DIRECT and PROXY share the widest measured IP-section geometry, so their
RTT/JIT columns align. Like TrafficMonitor's CPU/GPU and traffic items, RTT uses
a label plus one complete value cell: `31 ms`. The unit follows the actual value
with one space, while the cell reserves the width of `999 ms`; JIT therefore
starts at the same X coordinate on both rows. IP and RTT use two measured
host-font spaces, matching RTT and JIT; each group uses one internally. The
standalone IP `Width` setting is not applied as a composite minimum. Each
composite row also owns a DPI-scaled trailing gap. The two renderers inherit TrafficMonitor's current
`HFONT` and text colour from the HDC, making the host the single source for font
family, size, weight and normal text colour. RTT and JIT independently apply
PluginPing's numeric latency bands to their own values: 1-70 ms is green,
71-149 ms is yellow, and 150 ms or above is red. One value never inherits the
other's resolved colour. Rolling stability remains available in the
optional resource graph and debug diagnostics rather than being drawn as percentage text.
`DOWN`, `N/A` and `off` replace the RTT value and suppress the unit and JIT field.

## Route logic

### DIRECT IP

- Primary source: `https://api.bilibili.com/x/web-interface/zone`
- Fallback source: `https://whois.pconline.com.cn/ipJson.jsp?json=true`
- Displayed route code is always `CN` unless `DirectDisplayIp` supplies a manual IP.
- The old IP latency, ICMP, HTTP latency and MMDB/GeoLite paths are removed.

### PROXY IP

- Only source: `https://www.cloudflare.com/cdn-cgi/trace`
- Displayed IP is the parsed `ip` field.
- Displayed route code is the parsed `loc` field.
- `loc=CN` remains a valid Cloudflare result and is displayed as one `N/A`
  token for the combined ASN/IP group; it is not treated as a failed request.
- There is no Ipify or GeoIP fallback.
- Named proxy routes ignore HTTP bypass entries for this request, so the
  authoritative Cloudflare trace cannot silently become a direct request.
- Each trace request disables HTTP keep-alive so a stable local proxy address
  cannot retain a tunnel associated with the previously selected node.
- A transient trace failure keeps the last confirmed IP visible and continues
  retrying at `ProxyResolveIntervalMs`; DIRECT resolution alone uses
  `FailureBackoffMs`. A confirmed PROXY exit matching DIRECT is still rejected
  and cleared.

### TCP

- Authoritative target: `https://www.google.com/generate_204`
- Expected status: `204`
- Cloudflare `generate_204` remains diagnostic only and cannot restore TCP health.

### UDP

- Primary: UDP Echo at `sg.falsemeet.site:3478`
- Fallback: STUN at `stun.cloudflare.com:3478`
- The Echo response must exactly match the sent payload.
- After the first failed Echo round, STUN is tested before fallback is reported.
- While STUN is active, the Echo primary is rechecked every 15 seconds.
- Three consecutive successful Echo recovery checks are required before
  switching back to the primary.
- A failed Echo recovery check does not alter the active STUN route's RTT,
  jitter, stability or failure streak.

### Route and proxy-node changes

TCP follows the HTTP route while UDP follows its direct or SOCKS5 route. Their
transport identities are shared when both are direct, their normalized named
proxy endpoints match, or `ShareProxyIdentityForUdp=true` explicitly declares
that split HTTP/SOCKS endpoints select the same upstream node.

- Each confirmed Cloudflare trace publishes `proxy|<ip>` from the raw IP before
  the `loc=CN` display collapse. Country-only changes do not invalidate RTT.
- The first confirmed identity and every later identity change advance the
  proxy generation. Empty observations are ignored, so a transient trace
  failure cannot trigger a rebuild.
- A confirmed PROXY exit matching DIRECT publishes `direct|<ip>`, preserving
  both the leak state and the actual egress address.
- A bounded Windows best-route observer tracks IPv4/IPv6 interface, source and
  next hop. A local interface or default-route change advances
  a separate network generation even when the public egress IP is unchanged.
- TCP keys include the TCP route plus proxy and network generations. UDP keys
  include the UDP route and network generation, and include the proxy generation
  only for a proven shared route.
- Workers poll the route during interruptible waits, reject probe results that
  cross a generation, rebuild pooled transports and DNS state, and immediately
  publish an empty RTT window. A changed STUN egress performs the same UDP reset.
- Idle TCP/UDP workers wait on row-activity plus Windows interface, route and
  address-change events; the first visible frame or route notification wakes
  the corresponding worker. If notification registration fails, they retain
  interruptible one-second route polling.
- The PROXY identity remains refreshed whenever either row is visible, so a
  DIRECT-only layout still observes TCP node changes.
- DIRECT and PROXY IP requests use independent workers. A changed Cloudflare
  result is displayed immediately, while its route epoch waits for a fresh
  DIRECT comparison before transports inherit the new identity. The completed
  comparison releases that confirmed identity without requiring another
  successful Cloudflare request.
- With `DebugLog` enabled every log entry records `proxy node: <identity>
  (epoch N)`, and a rebuilt round reports `transport rebuilt after route change`.

## Configuration

Both internal engines read the same `PluginPing.ini`:

- `[IP]`: Direct/Proxy refresh, timeout and IP display fallback behavior. The
  default Proxy refresh cadence is 1 second so a router-side egress switch is
  reflected quickly; increase it when reducing remote request volume matters.
- `[Network]`: proxy routing shared by HTTP, TCP and UDP
- `[TCP_Settings]`: Google TCP probe
- `[UDP_Settings]`: Echo primary and STUN fallback
- `[Color_Rules]`: stability thresholds and optional colors
- `[Style]`: compact mode and optional resource graph; typography follows TrafficMonitor

Hot reload publishes one immutable file snapshot and generation to both
internal engines, so a save cannot leave the IP and probe workers on different
configuration generations.

`ConfigVersion=1` files that still contain the former 30-second DIRECT and
5-second PROXY defaults are migrated in memory to the new defaults. Version 2
files retain explicitly configured interval values.

TrafficMonitor's options command opens this INI with `ShellExecuteW`. The DLL
does not load CLR, C++/CLI or WPF into the TrafficMonitor process.

## Host safety rules

The implementation retains the startup fixes documented by `Pluginnetwork`:

- hidden rows do not start their IP/TCP/UDP network paths;
- hidden rows contribute an empty tooltip;
- UI callbacks only read immutable snapshots and do no disk or network I/O;
- all allocating, locking, drawing and worker callbacks seal C++ exceptions at
  the TrafficMonitor ABI boundary;
- renderers directly import `USER32.dll!DrawTextW` and restore HDC state with
  `SaveDC` / `RestoreDC`;
- headers that include `windows.h` define `WIN32_LEAN_AND_MEAN` first, while both
  network-facing common headers include `winsock2.h` before `windows.h`; this prevents
  the legacy `winsock.h` declarations from entering merged translation units;
- shutdown cancels WinHTTP/socket work, joins workers, then releases handles.
- IP worker recovery is limited to three retries with a five-second backoff;
  network workers retain their independent bounded recovery loops.

## Reused code

From `PluginPing-claude`:

- WinHTTP session and proxy handling;
- Direct/Cloudflare IP parsing;
- immutable IP snapshots;
- DPI-aware GDI renderer and width cache;
- TrafficMonitor callback and worker lifetime guards.

From `Pluginnetwork`:

- Google TCP probe and diagnostic fallback behavior;
- proxy/SOCKS routing;
- sliding-window RTT, jitter and stability metrics;
- UDP Echo/STUN transport;
- cancellation, idle gating and worker shutdown order.

The shared route planner is the single source for `ProxyMode=Auto`, HTTP/TCP
and UDP routing. With no enabled local proxy, Auto is Direct so router-side
transparent proxying remains usable. A local static proxy must expose a
SOCKS5-capable endpoint before UDP is treated as proxied.

The shared route generation combines the PROXY IP worker's confirmed egress
with the Windows best-route signature. Channel-specific keys prevent an
independent UDP SOCKS route from inheriting unrelated HTTP node changes.

The child adapters remain internal implementation units. Only the composite
`PluginPing` adapter and its two items are exported to TrafficMonitor.

## Build and verify

From this directory in a Visual Studio developer shell:

```powershell
msbuild .\PluginPing.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild .\PluginPing.sln /t:Rebuild /p:Configuration=Debug /p:Platform=x64
powershell -ExecutionPolicy Bypass -File .\tools\check_merge_contract.ps1
msbuild .\tools\test_core.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
.\tools\bin\test_core.exe
.\tools\bin\probe_plugin_runtime.exe `
  (Resolve-Path .\bin\x64\Release\PluginPing.dll) 30
```

Deploy only:

```text
PluginPing.dll
PluginPing.ini
```

Do not load the old `PluginPing-claude` DLL or `Pluginnetwork` DLL alongside this
replacement. The new DLL deliberately reuses the original PluginPing item IDs.

## Source consolidation

The former `PluginPing`, `PluginPing-claude` and `Pluginnetwork` development
directories were retired after the merged build, contract, route and runtime
checks passed. This directory is the only active PluginPing source tree.
