// op20msica.dll - custom action for the MSI's user interface.
//
// SetResultText runs in the UI sequence after the changes were made and fills
// the two properties shown on the final page (OP20_RESULT_TITLE, OP20_RESULT_TEXT)
// with what actually happened: installed / removed, whether the driver step
// worked, and which driver the cable is bound to. It changes nothing on the system.

#include <windows.h>

#include <msi.h>
#include <msiquery.h>

#include <string>

#include "devicestatus.h"

#pragma comment(lib, "msi.lib")
#pragma comment(linker, "/EXPORT:SetResultText")

namespace {

std::wstring property(MSIHANDLE session, const wchar_t *name) {
    wchar_t buf[512] = L"";
    DWORD size = _countof(buf);
    if (MsiGetPropertyW(session, name, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

// Outcome of the driver step, as recorded by op20winusb-driver-setup.exe.
bool driverOutcome(bool *ok, std::wstring *message) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, OP20_STATE_KEY, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0, size = sizeof(value);
    const bool have = RegQueryValueExW(key, OP20_STATE_OK, nullptr, nullptr, reinterpret_cast<BYTE *>(&value), &size) ==
                      ERROR_SUCCESS;
    wchar_t text[512] = L"";
    size = sizeof(text) - sizeof(wchar_t);
    RegQueryValueExW(key, OP20_STATE_MESSAGE, nullptr, nullptr, reinterpret_cast<BYTE *>(text), &size);
    RegCloseKey(key);
    *ok = value != 0;
    *message = text;
    return have;
}

}  // namespace

extern "C" UINT __stdcall SetResultText(MSIHANDLE session) {
    std::wstring title, text;

    if (property(session, L"REMOVE") == L"ALL") {
        title = L"Op20WinUSB was removed";
        text = L"The J2534 registration, the WinUSB driver package, its certificate and the installed files "
               L"were removed.\r\n\r\n"
               L"Other drivers for the cable (OPTA, Tactrix) were left untouched.";
    } else {
        bool ok = false;
        std::wstring message;
        const bool known = driverOutcome(&ok, &message);
        if (known && !ok) {
            title = L"Installed, but the driver step failed";
            text = L"The J2534 libraries were copied and registered, but the WinUSB driver package for the cable "
                   L"could not be installed:\r\n\r\n" +
                   message + L"\r\n\r\n" + op20DeviceStatus() +
                   L"\r\n\r\nThe cable only works with Op20WinUSB once it is bound to the WINUSB or usbser driver.";
        } else {
            title = L"Op20WinUSB was installed";
            text = L"The installation completed successfully.\r\n\r\n" + op20DeviceStatus() +
                   L"\r\n\r\nIn your J2534 program select:\r\n"
                   L"Op20WinUSB - Openport 2.0 (K-line, CAN)\r\n\r\n"
                   L"To check the cable, run optest32.exe info from the Op20WinUSB folder in Program Files.";
        }
    }
    MsiSetPropertyW(session, L"OP20_RESULT_TITLE", title.c_str());
    MsiSetPropertyW(session, L"OP20_RESULT_TEXT", text.c_str());
    return ERROR_SUCCESS;
}
