#include "transport.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winusb.h>

#include <cwchar>
#include <cwctype>
#include <vector>

#include "log.h"

namespace {

const wchar_t *const kHardwareId = L"VID_0403&PID_CC4D";

std::string narrow(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring upper(std::wstring s) {
    for (auto &c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

bool parseGuid(const wchar_t *text, GUID *out) {
    unsigned long d1;
    unsigned int d2, d3, b[8];
    if (swscanf_s(text, L"{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
                  &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    out->Data1 = d1;
    out->Data2 = static_cast<unsigned short>(d2);
    out->Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; i++) out->Data4[i] = static_cast<unsigned char>(b[i]);
    return true;
}

// ---------------------------------------------------------------- WinUSB --

class WinUsbTransport : public Transport {
public:
    WinUsbTransport(HANDLE file, WINUSB_INTERFACE_HANDLE base, WINUSB_INTERFACE_HANDLE iface, UCHAR epIn, UCHAR epOut,
                    USHORT maxPacket, std::string desc)
        : file_(file), base_(base), iface_(iface), epIn_(epIn), epOut_(epOut), maxPacket_(maxPacket),
          desc_(std::move(desc)) {
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~WinUsbTransport() override {
        if (iface_ && iface_ != base_) WinUsb_Free(iface_);
        if (base_) WinUsb_Free(base_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        CloseHandle(readEvent_);
        CloseHandle(writeEvent_);
        CloseHandle(stopEvent_);
    }

    int read(uint8_t *buf, size_t cap) override {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return ReadCancelled;
        OVERLAPPED ov = {};
        ov.hEvent = readEvent_;
        ResetEvent(readEvent_);
        ULONG got = 0;
        if (WinUsb_ReadPipe(iface_, epIn_, buf, static_cast<ULONG>(cap), &got, &ov)) return static_cast<int>(got);
        if (GetLastError() != ERROR_IO_PENDING) {
            OPLOG("WinUsb_ReadPipe failed: %lu", GetLastError());
            return ReadError;
        }
        HANDLE handles[2] = {readEvent_, stopEvent_};
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            WinUsb_AbortPipe(iface_, epIn_);
            WinUsb_GetOverlappedResult(iface_, &ov, &got, TRUE);
            return ReadCancelled;
        }
        if (!WinUsb_GetOverlappedResult(iface_, &ov, &got, FALSE)) {
            OPLOG("WinUSB read completed with error %lu", GetLastError());
            return ReadError;
        }
        return static_cast<int>(got);
    }

    bool write(const uint8_t *data, size_t len, DWORD timeoutMs) override {
        OVERLAPPED ov = {};
        ov.hEvent = writeEvent_;
        ResetEvent(writeEvent_);
        ULONG sent = 0;
        if (!WinUsb_WritePipe(iface_, epOut_, const_cast<PUCHAR>(data), static_cast<ULONG>(len), &sent, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                OPLOG("WinUsb_WritePipe failed: %lu", GetLastError());
                return false;
            }
            if (WaitForSingleObject(writeEvent_, timeoutMs) != WAIT_OBJECT_0) {
                OPLOG("WinUSB write timed out");
                WinUsb_AbortPipe(iface_, epOut_);
                WinUsb_GetOverlappedResult(iface_, &ov, &sent, TRUE);
                return false;
            }
            if (!WinUsb_GetOverlappedResult(iface_, &ov, &sent, FALSE)) {
                OPLOG("WinUSB write completed with error %lu", GetLastError());
                return false;
            }
        }
        return sent == len;
    }

    void cancelRead() override { SetEvent(stopEvent_); }

    // One USB packet per read: a bulk transfer only completes on a short packet
    // or a full buffer, so a larger buffer would hold back replies whose length
    // happens to be a multiple of the packet size.
    size_t readChunk() const override { return maxPacket_; }
    const std::string &description() const override { return desc_; }

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE base_;
    WINUSB_INTERFACE_HANDLE iface_;
    UCHAR epIn_, epOut_;
    USHORT maxPacket_;
    std::string desc_;
    HANDLE readEvent_, writeEvent_, stopEvent_;
};

// Looks for an interface with a bulk IN and a bulk OUT endpoint (the CDC data
// interface). The device is not bound as a composite device, so WinUSB exposes
// the first interface through the base handle and the rest as "associated".
bool findBulkPair(WINUSB_INTERFACE_HANDLE h, UCHAR *epIn, UCHAR *epOut, USHORT *maxPacket) {
    USB_INTERFACE_DESCRIPTOR ifd = {};
    if (!WinUsb_QueryInterfaceSettings(h, 0, &ifd)) return false;
    UCHAR in = 0, out = 0;
    USHORT mps = 0;
    for (UCHAR i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(h, 0, i, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            in = pipe.PipeId;
            mps = pipe.MaximumPacketSize;
        } else {
            out = pipe.PipeId;
        }
    }
    if (!in || !out) return false;
    *epIn = in;
    *epOut = out;
    *maxPacket = mps ? mps : 64;
    return true;
}

OpenResult openWinUsb(const std::wstring &path) {
    OpenResult r;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        OPLOG("CreateFile(%ls) failed: %lu", path.c_str(), err);
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION || err == ERROR_BUSY) {
            r.status = OpenStatus::InUse;
            r.message = "Openport 2.0 is in use by another application";
        } else {
            r.status = OpenStatus::Failed;
            r.message = "cannot open the Openport 2.0 USB interface (error " + std::to_string(err) + ")";
        }
        return r;
    }
    WINUSB_INTERFACE_HANDLE base = nullptr;
    if (!WinUsb_Initialize(file, &base)) {
        DWORD err = GetLastError();
        OPLOG("WinUsb_Initialize failed: %lu", err);
        CloseHandle(file);
        r.status = OpenStatus::Failed;
        r.message = "WinUsb_Initialize failed (error " + std::to_string(err) + ")";
        return r;
    }

    UCHAR epIn = 0, epOut = 0;
    USHORT mps = 64;
    WINUSB_INTERFACE_HANDLE iface = nullptr;
    if (findBulkPair(base, &epIn, &epOut, &mps)) {
        iface = base;
    } else {
        for (UCHAR idx = 0; idx < 8 && !iface; idx++) {
            WINUSB_INTERFACE_HANDLE assoc = nullptr;
            if (!WinUsb_GetAssociatedInterface(base, idx, &assoc)) break;
            if (findBulkPair(assoc, &epIn, &epOut, &mps))
                iface = assoc;
            else
                WinUsb_Free(assoc);
        }
    }
    if (!iface) {
        WinUsb_Free(base);
        CloseHandle(file);
        r.status = OpenStatus::Failed;
        r.message = "Openport 2.0 bulk endpoints not found";
        return r;
    }

    // Drop anything a previous session left in the pipes.
    WinUsb_FlushPipe(iface, epIn);
    OPLOG("WinUSB open: %ls  IN=%02X OUT=%02X maxPacket=%u", path.c_str(), epIn, epOut, mps);
    r.status = OpenStatus::Ok;
    r.transport.reset(new WinUsbTransport(file, base, iface, epIn, epOut, mps, "WinUSB"));
    return r;
}

// ---------------------------------------------------------------- serial --

class SerialTransport : public Transport {
public:
    SerialTransport(HANDLE file, std::string desc) : file_(file), desc_(std::move(desc)) {
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~SerialTransport() override {
        CloseHandle(file_);
        CloseHandle(readEvent_);
        CloseHandle(writeEvent_);
        CloseHandle(stopEvent_);
    }

    int read(uint8_t *buf, size_t cap) override {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return ReadCancelled;
        OVERLAPPED ov = {};
        ov.hEvent = readEvent_;
        ResetEvent(readEvent_);
        DWORD got = 0;
        if (ReadFile(file_, buf, static_cast<DWORD>(cap), &got, &ov)) return static_cast<int>(got);
        if (GetLastError() != ERROR_IO_PENDING) {
            OPLOG("serial ReadFile failed: %lu", GetLastError());
            return ReadError;
        }
        HANDLE handles[2] = {readEvent_, stopEvent_};
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(file_, &ov);
            GetOverlappedResult(file_, &ov, &got, TRUE);
            return ReadCancelled;
        }
        if (!GetOverlappedResult(file_, &ov, &got, FALSE)) {
            OPLOG("serial read completed with error %lu", GetLastError());
            return ReadError;
        }
        return static_cast<int>(got);
    }

    bool write(const uint8_t *data, size_t len, DWORD timeoutMs) override {
        OVERLAPPED ov = {};
        ov.hEvent = writeEvent_;
        ResetEvent(writeEvent_);
        DWORD sent = 0;
        if (!WriteFile(file_, data, static_cast<DWORD>(len), &sent, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                OPLOG("serial WriteFile failed: %lu", GetLastError());
                return false;
            }
            if (WaitForSingleObject(writeEvent_, timeoutMs) != WAIT_OBJECT_0) {
                OPLOG("serial write timed out");
                CancelIoEx(file_, &ov);
                GetOverlappedResult(file_, &ov, &sent, TRUE);
                return false;
            }
            if (!GetOverlappedResult(file_, &ov, &sent, FALSE)) {
                OPLOG("serial write completed with error %lu", GetLastError());
                return false;
            }
        }
        return sent == len;
    }

    void cancelRead() override { SetEvent(stopEvent_); }
    size_t readChunk() const override { return 4096; }
    const std::string &description() const override { return desc_; }

private:
    HANDLE file_;
    std::string desc_;
    HANDLE readEvent_, writeEvent_, stopEvent_;
};

OpenResult openSerial(const std::wstring &port) {
    OpenResult r;
    std::wstring path = L"\\\\.\\" + port;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        OPLOG("CreateFile(%ls) failed: %lu", path.c_str(), err);
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION || err == ERROR_BUSY) {
            r.status = OpenStatus::InUse;
            r.message = "Openport 2.0 (" + narrow(port) + ") is in use by another application";
        } else {
            r.status = OpenStatus::Failed;
            r.message = "cannot open " + narrow(port) + " (error " + std::to_string(err) + ")";
        }
        return r;
    }

    // The line settings are irrelevant for a USB CDC device, but usbser wants
    // a valid state and the firmware expects DTR to be raised.
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(file, &dcb)) {
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(file, &dcb);
    }
    // Return from ReadFile as soon as at least one byte is available.
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = MAXDWORD - 1;
    SetCommTimeouts(file, &to);
    SetupComm(file, 65536, 65536);
    PurgeComm(file, PURGE_RXCLEAR | PURGE_TXCLEAR);

    OPLOG("serial open: %ls", path.c_str());
    r.status = OpenStatus::Ok;
    r.transport.reset(new SerialTransport(file, "USB serial " + narrow(port)));
    return r;
}

// ----------------------------------------------------------- enumeration --

std::vector<std::wstring> interfacePaths(const GUID &guid, const wchar_t *instanceId) {
    std::vector<std::wstring> paths;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<LPGUID>(&guid), const_cast<DEVINSTID_W>(instanceId),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        len <= 1)
        return paths;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&guid), const_cast<DEVINSTID_W>(instanceId), buf.data(), len,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return paths;
    for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) paths.emplace_back(p);
    return paths;
}

std::vector<std::wstring> readMultiSz(HKEY key, const wchar_t *name) {
    std::vector<std::wstring> out;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0) return out;
    if (type != REG_MULTI_SZ && type != REG_SZ) return out;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buf.data()), &size) != ERROR_SUCCESS)
        return out;
    for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

}  // namespace

OpenResult openOpenport() {
    OpenResult result;
    result.status = OpenStatus::NotFound;
    result.message = "Openport 2.0 not found (is it plugged in?)";

    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) {
        result.status = OpenStatus::Failed;
        result.message = "cannot enumerate USB devices";
        return result;
    }

    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); i++) {
        wchar_t id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(devs, &dev, id, 511, nullptr)) continue;
        if (upper(id).find(kHardwareId) == std::wstring::npos) continue;

        wchar_t serviceBuf[128] = {};
        SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_SERVICE, nullptr, reinterpret_cast<BYTE *>(serviceBuf),
                                          sizeof(serviceBuf) - sizeof(wchar_t), nullptr);
        std::wstring service = upper(serviceBuf);
        OPLOG("found %ls, service '%ls'", id, serviceBuf);

        HKEY key = SetupDiOpenDevRegKey(devs, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        OpenResult attempt;
        attempt.status = OpenStatus::NotFound;

        if (service == L"WINUSB") {
            std::vector<std::wstring> guids;
            if (key != INVALID_HANDLE_VALUE) {
                guids = readMultiSz(key, L"DeviceInterfaceGUIDs");
                auto single = readMultiSz(key, L"DeviceInterfaceGUID");
                guids.insert(guids.end(), single.begin(), single.end());
            }
            // Fallbacks: the OPTA INF's GUID, then the generic USB device
            // interface every USB devnode exposes.
            guids.push_back(L"{C2E6EBE6-2FC5-4781-A4E5-B993B37CB069}");
            guids.push_back(L"{A5DCBF10-6530-11D2-901F-00C04FB951ED}");
            for (const auto &text : guids) {
                GUID guid;
                if (!parseGuid(text.c_str(), &guid)) continue;
                for (const auto &path : interfacePaths(guid, id)) {
                    attempt = openWinUsb(path);
                    if (attempt.status == OpenStatus::Ok || attempt.status == OpenStatus::InUse) break;
                }
                if (attempt.status == OpenStatus::Ok || attempt.status == OpenStatus::InUse) break;
            }
            if (attempt.status == OpenStatus::NotFound) {
                attempt.status = OpenStatus::Failed;
                attempt.message = "Openport 2.0 found but its WinUSB interface could not be located";
            }
        } else if (service == L"USBSER") {
            std::vector<std::wstring> port;
            if (key != INVALID_HANDLE_VALUE) port = readMultiSz(key, L"PortName");
            if (!port.empty()) {
                attempt = openSerial(port[0]);
            } else {
                attempt.status = OpenStatus::Failed;
                attempt.message = "Openport 2.0 found as a serial device but it has no COM port name";
            }
        } else if (service == L"OPENPORT") {
            attempt.status = OpenStatus::WrongDriver;
            attempt.message =
                "Openport 2.0 is bound to the original Tactrix kernel driver (openport.sys), which Windows 11 "
                "blocks. Switch the device to a WinUSB driver or uninstall the Tactrix driver package.";
        } else if (service == L"USBCCGP") {
            // Composite parent; its children are enumerated separately.
        } else {
            attempt.status = OpenStatus::WrongDriver;
            attempt.message = "Openport 2.0 is bound to an unsupported driver ('" + narrow(serviceBuf) +
                              "'); it needs WinUSB or the inbox USB serial driver";
        }
        if (key != INVALID_HANDLE_VALUE) RegCloseKey(key);

        if (attempt.status == OpenStatus::Ok) {
            SetupDiDestroyDeviceInfoList(devs);
            return attempt;
        }
        if (attempt.status != OpenStatus::NotFound) {
            // Remember the most useful failure but keep looking for another unit.
            result.status = attempt.status;
            result.message = attempt.message;
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    return result;
}
