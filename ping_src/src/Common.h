// PluginPing - common Windows headers and import libraries
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define PLUGINPING_HMODULE (reinterpret_cast<HMODULE>(&__ImageBase))

// Single source of truth for the plugin version shown to the host and the
// WinHTTP User-Agent. Keep src/PluginPing.rc in sync when bumping.
inline constexpr wchar_t kPluginPingVersion[] = L"1.0.14";
inline constexpr wchar_t kPluginPingUserAgent[] = L"PluginPing/1.0.14";
