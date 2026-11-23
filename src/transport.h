// Byte-stream transport to the Openport 2.0.
//
// The Openport 2.0 (USB VID 0403 / PID CC4D) is a USB CDC-ACM device whose data
// interface carries the Tactrix "at" command stream. No vendor kernel driver is
// needed to reach it; two inbox, Microsoft-signed drivers work:
//   * winusb.sys  - when a WinUSB INF is bound to the device (e.g. the OPTA INF,
//                   Zadig, or "WinUsb Device" picked by hand in Device Manager)
//   * usbser.sys  - what Windows binds on its own when no vendor INF matches;
//                   the device then shows up as a COM port
// Both are supported here, so this DLL needs no driver package of its own.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

class Transport {
public:
    enum : int { ReadError = -1, ReadCancelled = -2 };

    virtual ~Transport() = default;

    // Blocks until bytes arrive. Returns the byte count (0 is possible and means
    // "nothing this time"), ReadError when the device is gone, or ReadCancelled
    // after cancelRead().
    virtual int read(uint8_t *buf, size_t cap) = 0;
    virtual bool write(const uint8_t *data, size_t len, DWORD timeoutMs) = 0;
    // Makes a blocked (and every later) read() return ReadCancelled.
    virtual void cancelRead() = 0;
    // Preferred buffer size for read().
    virtual size_t readChunk() const = 0;
    virtual const std::string &description() const = 0;
};

enum class OpenStatus { Ok, NotFound, InUse, WrongDriver, Failed };

struct OpenResult {
    OpenStatus status = OpenStatus::NotFound;
    std::string message;
    std::unique_ptr<Transport> transport;
};

// Finds the first connected Openport 2.0 and opens it.
OpenResult openOpenport();
