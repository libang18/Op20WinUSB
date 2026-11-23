// Shared by the setup helper and the MSI custom-action DLL: a one-line
// description of the driver the Openport 2.0 is bound to right now.
#pragma once

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>

#include <string>

inline std::wstring op20DeviceStatus() {
    std::wstring text;
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return text;
    bool any = false;
    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); i++) {
        wchar_t id[512] = L"";
        if (!SetupDiGetDeviceInstanceIdW(devs, &dev, id, 511, nullptr)) continue;
        CharUpperW(id);
        if (!wcsstr(id, L"VID_0403&PID_CC4D")) continue;
        wchar_t service[128] = L"(none)";
        SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_SERVICE, nullptr, reinterpret_cast<BYTE *>(service),
                                          sizeof(service) - sizeof(wchar_t), nullptr);
        ULONG status = 0, problem = 0;
        const bool present = CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS;
        any = true;
        if (!text.empty()) text += L"\r\n";
        text += std::wstring(L"Openport 2.0 cable: ") + (present ? L"connected" : L"not connected") + L", driver: " +
                service;
    }
    SetupDiDestroyDeviceInfoList(devs);
    if (!any) text = L"No Openport 2.0 has been connected to this PC yet; the driver binds when it is plugged in.";
    return text;
}

// Where the helper records the outcome of the driver step for the result page.
#define OP20_STATE_KEY L"SOFTWARE\\Op20WinUSB"
#define OP20_STATE_OK L"DriverSetupOk"
#define OP20_STATE_MESSAGE L"DriverSetupMessage"
