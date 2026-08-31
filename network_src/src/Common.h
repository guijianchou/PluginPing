// pluginNetwork - common Windows headers and import libraries
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// winsock2.h must precede windows.h. WIN32_LEAN_AND_MEAN already keeps the
// legacy winsock.h out, but the explicit order also protects translation units
// that include this header after something else pulled windows.h in.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define PLUGINNETWORK_HMODULE (reinterpret_cast<HMODULE>(&__ImageBase))

// Single source of truth for the plugin version shown to the host and used in
// the WinHTTP User-Agent. Keep src/PluginNetwork.rc in sync when bumping.
inline constexpr wchar_t kPluginNetworkVersion[] = L"1.0.14";
inline constexpr wchar_t kPluginNetworkUserAgent[] = L"PluginPing/1.0.14";
