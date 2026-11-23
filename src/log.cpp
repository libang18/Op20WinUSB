#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace oplog {

static std::mutex g_mutex;
static FILE *g_file = nullptr;
static bool g_enabled = false;
static bool g_initDone = false;

static std::wstring configuredPath() {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"OP20WINUSB_LOG", buf, 1024);
    if (n > 0 && n < 1024) return buf;

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Op20WinUSB", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
        LSTATUS st = RegQueryValueExW(key, L"LogFile", nullptr, &type, reinterpret_cast<BYTE *>(buf), &size);
        RegCloseKey(key);
        if (st == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t)) {
            buf[size / sizeof(wchar_t)] = 0;
            return buf;
        }
    }
    return L"";
}

void init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initDone) return;
    g_initDone = true;
    std::wstring path = configuredPath();
    if (path.empty()) return;
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    g_enabled = g_file != nullptr;
}

bool enabled() { return g_enabled; }

static void prefixLocked() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
}

void write(const char *fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    prefixLocked();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_file);
    fflush(g_file);
}

void dump(const char *prefix, const uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    prefixLocked();
    fprintf(g_file, "%s %zu bytes:", prefix, len);
    const size_t shown = len > 96 ? 96 : len;
    for (size_t i = 0; i < shown; i++) fprintf(g_file, " %02X", data[i]);
    if (shown < len) fputs(" ...", g_file);
    fputs("  |", g_file);
    for (size_t i = 0; i < shown; i++) fputc(data[i] >= 0x20 && data[i] < 0x7F ? data[i] : '.', g_file);
    fputs("|\n", g_file);
    fflush(g_file);
}

}  // namespace oplog
