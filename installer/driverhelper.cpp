// op20winusb-driver-setup.exe - helper run by the MSI package.
//
// Windows Installer copies the files and writes the registry itself; this
// program does the two things an MSI cannot do natively:
//
//   /driver-install    trust the self-signed certificate of the driver package
//                      (op20winusb.cat next to this exe), add op20winusb.inf to
//                      the driver store, and remove the original Tactrix kernel
//                      driver package, which Windows 11 blocks
//   /driver-uninstall  remove this project's INF from the driver store and its
//                      certificate from the trusted stores
//   /check             report what /driver-install would do; changes nothing
//
// It shows no windows. Exit code 0 = success. The log goes to
// Op20WinUSB-driver-setup.log in the temp folder of the account they run as
// (the SYSTEM account's when run by the installer). The outcome of
// /driver-install is also recorded in the registry for the installer's result page.

#include <windows.h>

#include <shellapi.h>
#include <wincrypt.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "devicestatus.h"

#define CERT_NAME L"Op20WinUSB driver package (self-signed)"

namespace {

std::wstring g_log;
std::wstring g_firstError;
bool g_failed = false;

void note(const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log += buf;
    g_log += L"\r\n";
}

void fail(const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_firstError.empty()) g_firstError = buf;
    g_log += L"ERROR: ";
    g_log += buf;
    g_log += L"\r\n";
    g_failed = true;
}

std::wstring windowsDir() {
    wchar_t buf[MAX_PATH];
    GetWindowsDirectoryW(buf, MAX_PATH);
    return buf;
}

std::wstring systemDir() {
    wchar_t buf[MAX_PATH];
    GetSystemDirectoryW(buf, MAX_PATH);
    return buf;
}

// Folder this program was installed to (where the INF and the catalog are).
std::wstring ownDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    return path.substr(0, path.find_last_of(L"\\/"));
}

bool fileExists(const std::wstring &path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs a console program hidden and copies its output into the log.
DWORD run(const std::wstring &commandLine) {
    note(L"> %s", commandLine.c_str());
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return GetLastError();
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(0);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        note(L"  could not start (error %lu)", err);
        return err ? err : 1;
    }
    CloseHandle(writeEnd);

    std::string output;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got) output.append(buf, got);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!output.empty()) {
        int n = MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
        std::wstring wide(n, 0);
        MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), &wide[0], n);
        g_log += wide;
        if (wide.empty() || wide.back() != L'\n') g_log += L"\r\n";
    }
    note(L"  exit code %lu", exitCode);
    return exitCode;
}

// True if the file contains the (lower-case ASCII) text, in ANSI or UTF-16.
bool fileContains(const std::wstring &path, const char *needle) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::string data(size, '\0');
    DWORD got = 0;
    ReadFile(file, &data[0], size, &got, nullptr);
    CloseHandle(file);
    data.resize(got);
    for (auto &c : data)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);

    std::string ansi(needle), wide;
    for (char c : ansi) {
        wide.push_back(c);
        wide.push_back('\0');
    }
    return data.find(ansi) != std::string::npos || data.find(wide) != std::string::npos;
}

// Published driver packages (oemNN.inf) whose INF mentions the given text.
std::vector<std::wstring> oemInfsContaining(const char *needle) {
    std::vector<std::wstring> found;
    std::wstring dir = windowsDir() + L"\\INF\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"oem*.inf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (fileContains(dir + fd.cFileName, needle)) found.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// The signer certificate embedded in the catalog, checked to be ours.
PCCERT_CONTEXT packageCertificate(const std::wstring &catPath) {
    // A catalog is a PKCS#7 signed message; the signer's certificate travels inside it.
    HANDLE file = CreateFileW(catPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail(L"cannot open %s (error %lu)", catPath.c_str(), GetLastError());
        return nullptr;
    }
    std::vector<BYTE> data(GetFileSize(file, nullptr));
    DWORD got = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(file);

    const DWORD encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    HCRYPTMSG msg = CryptMsgOpenToDecode(encoding, 0, 0, 0, nullptr, nullptr);
    HCERTSTORE store = nullptr;
    if (msg && CryptMsgUpdate(msg, data.data(), got, TRUE))
        store = CertOpenStore(CERT_STORE_PROV_MSG, encoding, 0, 0, msg);
    if (!store) {
        fail(L"cannot read the signature of %s (error %lu)", catPath.c_str(), GetLastError());
        if (msg) CryptMsgClose(msg);
        return nullptr;
    }
    PCCERT_CONTEXT result = nullptr, cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
        wchar_t name[256] = L"";
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, _countof(name));
        const bool selfSigned = CertCompareCertificateName(X509_ASN_ENCODING, &cert->pCertInfo->Subject,
                                                           &cert->pCertInfo->Issuer) != FALSE;
        note(L"  certificate in catalog: '%s'%s", name, selfSigned ? L" (self-signed)" : L"");
        if (selfSigned && wcscmp(name, CERT_NAME) == 0) {
            result = CertDuplicateCertificateContext(cert);
            CertFreeCertificateContext(cert);
            break;
        }
    }
    CertCloseStore(store, 0);
    CryptMsgClose(msg);
    if (!result) fail(L"the catalog is not signed with the expected package certificate");
    return result;
}

std::wstring thumbprint(PCCERT_CONTEXT cert) {
    BYTE hash[20];
    DWORD size = sizeof(hash);
    std::wstring out;
    if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, hash, &size)) {
        wchar_t hex[3];
        for (DWORD i = 0; i < size; i++) {
            swprintf_s(hex, L"%02X", hash[i]);
            out += hex;
        }
    }
    return out;
}

bool addToMachineStore(PCCERT_CONTEXT cert, const wchar_t *storeName) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (!store) return false;
    BOOL ok = CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    CertCloseStore(store, 0);
    return ok != FALSE;
}

int removeFromMachineStore(const wchar_t *storeName) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (!store) return 0;
    int removed = 0;
    PCCERT_CONTEXT cert;
    while ((cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                              CERT_FIND_SUBJECT_STR_W, CERT_NAME, nullptr)) != nullptr) {
        if (!CertDeleteCertificateFromStore(cert)) break;  // frees the context either way
        removed++;
    }
    CertCloseStore(store, 0);
    return removed;
}

void reportDevice() { note(L"%s", op20DeviceStatus().c_str()); }

// Leaves the outcome of the driver step where the installer's result page reads it.
void recordOutcome() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, OP20_STATE_KEY, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    const DWORD ok = g_failed ? 0 : 1;
    RegSetValueExW(key, OP20_STATE_OK, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&ok), sizeof(ok));
    RegSetValueExW(key, OP20_STATE_MESSAGE, 0, REG_SZ, reinterpret_cast<const BYTE *>(g_firstError.c_str()),
                   static_cast<DWORD>((g_firstError.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void writeLog() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring path = std::wstring(temp) + L"Op20WinUSB-driver-setup.log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD written;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    WriteFile(file, g_log.data(), static_cast<DWORD>(g_log.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

void driverInstall(bool checkOnly) {
    const std::wstring dir = ownDir();
    const std::wstring pnputil = L"\"" + systemDir() + L"\\pnputil.exe\"";
    const std::wstring inf = dir + L"\\op20winusb.inf";
    note(L"Driver package folder: %s", dir.c_str());
    if (!fileExists(inf)) {
        fail(L"%s is missing", inf.c_str());
        return;
    }

    // Trust the package certificate, then add the package to the driver store.
    PCCERT_CONTEXT cert = packageCertificate(dir + L"\\op20winusb.cat");
    if (!cert) return;
    note(L"Package certificate thumbprint: %s", thumbprint(cert).c_str());
    if (checkOnly) {
        note(L"(check) would trust this certificate and install op20winusb.inf");
    } else if (!addToMachineStore(cert, L"Root") || !addToMachineStore(cert, L"TrustedPublisher")) {
        fail(L"cannot add the package certificate to the trusted stores (error %lu)", GetLastError());
    } else {
        note(L"Certificate added to LocalMachine\\Root and LocalMachine\\TrustedPublisher");
        // 259 = nothing to update (already installed), 3010 = installed, reboot pending.
        DWORD rc = run(pnputil + L" /add-driver \"" + inf + L"\" /install");
        if (rc == 0 || rc == ERROR_NO_MORE_ITEMS || rc == ERROR_SUCCESS_REBOOT_REQUIRED)
            note(L"Driver package installed (WinUSB binding for the Openport 2.0)");
        else
            fail(L"pnputil could not install the driver package (exit code %lu)", rc);
    }
    CertFreeCertificateContext(cert);
    if (g_failed) return;

    // The original Tactrix kernel driver is blocked by Windows 11; remove it so
    // the device binds to WinUSB. Not being able to remove it is not fatal.
    std::vector<std::wstring> tactrix = oemInfsContaining("openport.sys");
    if (tactrix.empty()) note(L"Original Tactrix kernel driver is not installed - nothing to remove");
    for (const auto &oem : tactrix) {
        if (checkOnly) {
            note(L"(check) would remove the Tactrix driver package %s", oem.c_str());
            continue;
        }
        DWORD rc = run(pnputil + L" /delete-driver " + oem + L" /uninstall");
        note(rc == 0 || rc == ERROR_SUCCESS_REBOOT_REQUIRED ? L"Removed the blocked Tactrix driver package %s"
                                                             : L"Could not remove the Tactrix driver package %s",
             oem.c_str());
    }
    if (!checkOnly) run(pnputil + L" /scan-devices");

    // Leftovers of the earlier stand-alone exe installer: its "Installed apps"
    // entry and its copy of itself. The MSI owns the installation from now on.
    const wchar_t *legacyKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Op20WinUSB";
    const std::wstring legacyExe = dir + L"\\Op20WinUSB-Setup.exe";
    HKEY key;
    const bool hasLegacyKey =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, legacyKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS;
    if (hasLegacyKey) RegCloseKey(key);
    if (hasLegacyKey || fileExists(legacyExe)) {
        if (checkOnly) {
            note(L"(check) would remove the entry and files of the earlier exe installer");
        } else {
            RegDeleteKeyExW(HKEY_LOCAL_MACHINE, legacyKey, KEY_WOW64_64KEY, 0);
            DeleteFileW(legacyExe.c_str());
            note(L"Removed the entry and files of the earlier exe installer");
        }
    }
    reportDevice();
}

void driverUninstall() {
    const std::wstring pnputil = L"\"" + systemDir() + L"\\pnputil.exe\"";

    std::vector<std::wstring> ours = oemInfsContaining("op20winusb.cat");
    if (ours.empty()) note(L"Driver package is not in the driver store");
    for (const auto &oem : ours) {
        DWORD rc = run(pnputil + L" /delete-driver " + oem + L" /uninstall");
        note(rc == 0 || rc == ERROR_SUCCESS_REBOOT_REQUIRED ? L"Driver package %s removed"
                                                             : L"Could not remove driver package %s",
             oem.c_str());
    }
    run(pnputil + L" /scan-devices");

    int removed = removeFromMachineStore(L"Root") + removeFromMachineStore(L"TrustedPublisher");
    note(L"Package certificate removed from the trusted stores (%d entries)", removed);

    // replace-op20pt32.ps1 may have put our DLL in place of the Tactrix one.
    std::wstring sys = windowsDir() + L"\\SysWOW64\\op20pt32.dll";
    std::wstring backup = sys + L".tactrix-backup";
    if (fileExists(backup) && CopyFileW(backup.c_str(), sys.c_str(), FALSE)) {
        DeleteFileW(backup.c_str());
        note(L"Restored the original %s", sys.c_str());
    }
    reportDevice();
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *mode = argv && argc > 1 ? argv[1] : L"";

    if (_wcsicmp(mode, L"/driver-install") == 0) {
        driverInstall(false);
        recordOutcome();
    } else if (_wcsicmp(mode, L"/check") == 0) {
        driverInstall(true);
    } else if (_wcsicmp(mode, L"/driver-uninstall") == 0) {
        driverUninstall();
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, OP20_STATE_KEY, KEY_WOW64_64KEY, 0);
    } else {
        MessageBoxW(nullptr,
                    L"This program is part of the Op20WinUSB installer and is run by it.\n\n"
                    L"To install or remove the driver use Op20WinUSB-Setup.msi or "
                    L"Settings > Apps > Installed apps.",
                    L"Op20WinUSB", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    writeLog();
    return g_failed ? 1 : 0;
}
