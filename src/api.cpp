// SAE J2534-1 (04.04) API of the Openport 2.0 WinUSB pass-thru DLL.
//
// Device and channel IDs follow the original Tactrix DLL: one device, and the
// ChannelID returned by PassThruConnect equals the ProtocolID.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "device.h"
#include "j2534.h"
#include "log.h"

#define OP20_DLL_VERSION "1.0.0"
#define OP20_API_VERSION "04.04"

namespace {

const unsigned long kDeviceId = 1;

std::mutex g_deviceMutex;
std::shared_ptr<Device> g_device;
HMODULE g_selfReference = nullptr;

std::mutex g_errorMutex;
std::string g_lastError;

void setError(const std::string &message) {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_lastError = message;
}

std::shared_ptr<Device> device() {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    return g_device;
}

long finish(const std::shared_ptr<Device> &dev, long rc) {
    if (rc != STATUS_NOERROR) setError(dev->error());
    return rc;
}

long noDevice() {
    setError("device is not open");
    return ERR_DEVICE_NOT_CONNECTED;
}

long invalidDevice() {
    setError("invalid device ID");
    return ERR_INVALID_DEVICE_ID;
}

long nullParameter(const char *what) {
    setError(std::string(what) + " must not be NULL");
    return ERR_NULL_PARAMETER;
}

void copyString(char *dst, const std::string &src) {
    // J2534 string buffers are 80 characters.
    strncpy_s(dst, 80, src.c_str(), _TRUNCATE);
}

}  // namespace

extern "C" {

long __stdcall PassThruOpen(const void *pName, unsigned long *pDeviceID) {
    (void)pName;
    oplog::init();
    OPLOG("PassThruOpen");
    if (!pDeviceID) return nullParameter("pDeviceID");

    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_device) {
        setError("device is already open");
        return ERR_DEVICE_IN_USE;
    }
    auto dev = std::make_shared<Device>();
    long rc = dev->open();
    if (rc != STATUS_NOERROR) {
        setError(dev->error());
        return rc;
    }
    // Keep the DLL loaded while the reader thread runs, even if the
    // application unloads it without calling PassThruClose.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PassThruOpen),
                       &g_selfReference);
    g_device = dev;
    *pDeviceID = kDeviceId;
    return STATUS_NOERROR;
}

long __stdcall PassThruClose(unsigned long DeviceID) {
    OPLOG("PassThruClose(%lu)", DeviceID);
    std::shared_ptr<Device> dev;
    HMODULE self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (!g_device) return noDevice();
        if (DeviceID != kDeviceId) return invalidDevice();
        dev.swap(g_device);
        self = g_selfReference;
        g_selfReference = nullptr;
    }
    dev->close();
    dev.reset();
    if (self) FreeLibrary(self);
    return STATUS_NOERROR;
}

long __stdcall PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                               unsigned long BaudRate, unsigned long *pChannelID) {
    OPLOG("PassThruConnect(protocol 0x%lX, flags 0x%lX, baud %lu)", ProtocolID, Flags, BaudRate);
    auto dev = device();
    if (!dev) return noDevice();
    if (DeviceID != kDeviceId) return invalidDevice();
    if (!pChannelID) return nullParameter("pChannelID");
    return finish(dev, dev->connect(ProtocolID, Flags, BaudRate, pChannelID));
}

long __stdcall PassThruDisconnect(unsigned long ChannelID) {
    OPLOG("PassThruDisconnect(0x%lX)", ChannelID);
    auto dev = device();
    if (!dev) return noDevice();
    return finish(dev, dev->disconnect(ChannelID));
}

long __stdcall PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                                unsigned long Timeout) {
    auto dev = device();
    if (!dev) return noDevice();
    if (!pMsg) return nullParameter("pMsg");
    if (!pNumMsgs) return nullParameter("pNumMsgs");
    return finish(dev, dev->readMsgs(ChannelID, pMsg, pNumMsgs, Timeout));
}

long __stdcall PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                                 unsigned long Timeout) {
    auto dev = device();
    if (!dev) return noDevice();
    if (!pMsg) return nullParameter("pMsg");
    if (!pNumMsgs) return nullParameter("pNumMsgs");
    return finish(dev, dev->writeMsgs(ChannelID, pMsg, pNumMsgs, Timeout));
}

long __stdcall PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID,
                                        unsigned long TimeInterval) {
    OPLOG("PassThruStartPeriodicMsg(0x%lX, %lu ms)", ChannelID, TimeInterval);
    auto dev = device();
    if (!dev) return noDevice();
    if (!pMsg) return nullParameter("pMsg");
    if (!pMsgID) return nullParameter("pMsgID");
    return finish(dev, dev->startPeriodic(ChannelID, pMsg, pMsgID, TimeInterval));
}

long __stdcall PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID) {
    OPLOG("PassThruStopPeriodicMsg(0x%lX, %lu)", ChannelID, MsgID);
    auto dev = device();
    if (!dev) return noDevice();
    return finish(dev, dev->stopPeriodic(ChannelID, MsgID));
}

long __stdcall PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PASSTHRU_MSG *pMaskMsg,
                                      const PASSTHRU_MSG *pPatternMsg, const PASSTHRU_MSG *pFlowControlMsg,
                                      unsigned long *pFilterID) {
    OPLOG("PassThruStartMsgFilter(0x%lX, type %lu)", ChannelID, FilterType);
    auto dev = device();
    if (!dev) return noDevice();
    if (!pMaskMsg) return nullParameter("pMaskMsg");
    if (!pPatternMsg) return nullParameter("pPatternMsg");
    if (!pFilterID) return nullParameter("pFilterID");
    return finish(dev, dev->startFilter(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pFilterID));
}

long __stdcall PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID) {
    OPLOG("PassThruStopMsgFilter(0x%lX, %lu)", ChannelID, FilterID);
    auto dev = device();
    if (!dev) return noDevice();
    return finish(dev, dev->stopFilter(ChannelID, FilterID));
}

long __stdcall PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long PinNumber, unsigned long Voltage) {
    OPLOG("PassThruSetProgrammingVoltage(pin %lu, 0x%lX)", PinNumber, Voltage);
    auto dev = device();
    if (!dev) return noDevice();
    if (DeviceID != kDeviceId) return invalidDevice();
    return finish(dev, dev->setProgrammingVoltage(PinNumber, Voltage));
}

long __stdcall PassThruReadVersion(unsigned long DeviceID, char *pFirmwareVersion, char *pDllVersion,
                                   char *pApiVersion) {
    auto dev = device();
    if (!dev) return noDevice();
    if (DeviceID != kDeviceId) return invalidDevice();
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion) return nullParameter("version buffer");
    copyString(pFirmwareVersion, dev->firmwareVersion());
    copyString(pDllVersion, std::string(OP20_DLL_VERSION " (Op20WinUSB, ") + dev->transportName() + ")");
    copyString(pApiVersion, OP20_API_VERSION);
    return STATUS_NOERROR;
}

long __stdcall PassThruGetLastError(char *pErrorDescription) {
    if (!pErrorDescription) return ERR_NULL_PARAMETER;
    std::lock_guard<std::mutex> lock(g_errorMutex);
    copyString(pErrorDescription, g_lastError);
    return STATUS_NOERROR;
}

long __stdcall PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput) {
    OPLOG("PassThruIoctl(0x%lX, ioctl 0x%lX)", ChannelID, IoctlID);
    auto dev = device();
    if (!dev) return noDevice();

    // Device-level requests; applications pass either the device ID or a
    // channel ID as the first argument.
    switch (IoctlID) {
        case READ_VBATT:
            if (!pOutput) return nullParameter("pOutput");
            return finish(dev, dev->readPinVoltage(J1962_PIN_16, static_cast<unsigned long *>(pOutput)));
        case READ_PROG_VOLTAGE:
            if (!pOutput) return nullParameter("pOutput");
            return finish(dev, dev->readProgVoltage(static_cast<const unsigned long *>(pInput),
                                                    static_cast<unsigned long *>(pOutput)));
        case TX_IOCTL_APP_SERVICE:
            return finish(dev, dev->appService(pInput, pOutput));
        case TX_IOCTL_SET_DLL_DEBUG_FLAGS:
        case TX_IOCTL_SET_DEV_DEBUG_FLAGS:
        case TX_IOCTL_SET_DLL_STATUS_CALLBACK:
            return STATUS_NOERROR;  // accepted and ignored
        default:
            break;
    }

    if (!dev->hasChannel(ChannelID)) {
        setError("invalid channel ID");
        return ERR_INVALID_CHANNEL_ID;
    }
    switch (IoctlID) {
        case GET_CONFIG: {
            const SCONFIG_LIST *list = static_cast<const SCONFIG_LIST *>(pInput);
            if (!list || (list->NumOfParams && !list->ConfigPtr)) return nullParameter("pInput");
            for (unsigned long i = 0; i < list->NumOfParams; i++) {
                long rc = dev->getConfig(ChannelID, &list->ConfigPtr[i]);
                if (rc != STATUS_NOERROR) return finish(dev, rc);
            }
            return STATUS_NOERROR;
        }
        case SET_CONFIG: {
            const SCONFIG_LIST *list = static_cast<const SCONFIG_LIST *>(pInput);
            if (!list || (list->NumOfParams && !list->ConfigPtr)) return nullParameter("pInput");
            for (unsigned long i = 0; i < list->NumOfParams; i++) {
                OPLOG("  SET_CONFIG param 0x%lX = %lu", list->ConfigPtr[i].Parameter, list->ConfigPtr[i].Value);
                long rc = dev->setConfig(ChannelID, &list->ConfigPtr[i]);
                if (rc != STATUS_NOERROR) return finish(dev, rc);
            }
            return STATUS_NOERROR;
        }
        case FIVE_BAUD_INIT:
            return finish(dev, dev->fiveBaudInit(ChannelID, static_cast<const SBYTE_ARRAY *>(pInput),
                                                 static_cast<SBYTE_ARRAY *>(pOutput)));
        case FAST_INIT:
            return finish(dev, dev->fastInit(ChannelID, static_cast<const PASSTHRU_MSG *>(pInput),
                                             static_cast<PASSTHRU_MSG *>(pOutput)));
        case CLEAR_TX_BUFFER:
            return STATUS_NOERROR;
        case CLEAR_RX_BUFFER:
            return finish(dev, dev->clearRx(ChannelID));
        case CLEAR_PERIODIC_MSGS:
            return finish(dev, dev->clearPeriodic(ChannelID));
        case CLEAR_MSG_FILTERS:
            return finish(dev, dev->clearFilters(ChannelID));
        case CLEAR_FUNCT_MSG_LOOKUP_TABLE:
        case ADD_TO_FUNCT_MSG_LOOKUP_TABLE:
        case DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE:
            return STATUS_NOERROR;  // J1850 only; no-ops in the original DLL too
        default:
            setError("unknown ioctl ID");
            return ERR_INVALID_IOCTL_ID;
    }
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
