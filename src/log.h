// Optional debug log. Enabled by the environment variable OP20WINUSB_LOG
// (path of the file to append to) or by the registry value
// HKCU\Software\Op20WinUSB\LogFile (REG_SZ).
#pragma once

#include <cstddef>
#include <cstdint>

namespace oplog {

void init();
bool enabled();
void write(const char *fmt, ...);
// Logs "<prefix> <n> bytes: hex..." (printable ASCII shown alongside).
void dump(const char *prefix, const uint8_t *data, size_t len);

}  // namespace oplog

#define OPLOG(...) do { if (oplog::enabled()) oplog::write(__VA_ARGS__); } while (0)
