# Op20WinUSB – technical notes

Details for people who write J2534 applications for the driver, build it or debug an installation. The overview is in [README.md](README.md).

## K-line with a Bosch EDC15

### KW1281 from an application

KW1281 is byte-oriented: every byte the ECU sends has to be answered with its complement
before the next one comes. It works over a plain raw channel with these settings:

- `PassThruConnect(ISO9141, ISO9141_NO_CHECKSUM | ISO9141_K_LINE_ONLY, 9600)`, pass-all filter
- `SET_CONFIG`: `LOOPBACK=0`, `P1_MAX=2` (a received byte is delivered after 1 ms of
  silence), `P3_MIN=0`, `P4_MIN=0`, `PARITY=0`
- `FIVE_BAUD_INIT` with the ECU address, then read one byte / write its complement with a
  blocking `PassThruWriteMsgs` (non-zero timeout)

One byte takes about 16 ms end to end, well inside the ECU's timeout.

### FAST_INIT on the Bosch EDC15P

`FAST_INIT` returning `ERR_FAILED` on this ECU is not a driver fault. Measured with
`optest fastdiag` / `optest kwp`:

- The firmware runs the init: the time to the error is exactly `TWUP` + the time to send
  the request + a 50 ms wait (126 ms by default, 376 ms with `TWUP=300`). A command the
  firmware rejects comes back within 1 ms.
- The firmware returns `ERR_FAILED` (7) when it ran the init and got no complete reply.
- With the default 5 ms between request bytes (`P4_MIN=10`) the ECU stays silent; it needs
  10 ms (`P4_MIN=20`).
- The request on the wire (`aty<ch> <length> <TxFlags> <seq>` followed by the message) is
  the same one the original Tactrix DLL builds.

**KWP2000 on the EDC15P works reliably** (`optest edc15`: 14 of 14 sessions, every frame
complete) when the session is handled the way the ECU expects:

- `PassThruConnect(ISO14230, ISO9141_NO_CHECKSUM | ISO9141_K_LINE_ONLY, 10400)`, pass-all
  filter, `SET_CONFIG`: `LOOPBACK=0`, `P1_MAX=40`, `P3_MIN=0`, `P4_MIN=20`.
- `FAST_INIT` with `81 01 F1 81 F4` → `83 F1 01 C1 6B 8F 30`.
- Requests with `PassThruWriteMsgs` (non-zero timeout), ~75 ms apart; e.g.
  `82 01 F1 10 85 09` → `83 F1 01 7F 10 78 7C` (pending) then `83 F1 01 50 85 2F 79`.
- **End every session with StopCommunication** (`81 01 F1 82 F5` → `81 F1 01 C2 35`).

The last point is what makes it reliable. If a session is left open, the ECU is still in
it when the next wake-up pulse arrives and answers with refusals such as
`83 00 01 7F 00 13 16` or `83 F1 01 7F 81 13 88`, which then overlap the request and show
up as cut or shifted frames. When that happens, send StopCommunication (or wait for the
ECU's session to time out) and start again.

## Installer details

`Op20WinUSB-Setup.msi` is a single self-contained Windows Installer package. It:

1. copies the files to `C:\Program Files\Op20WinUSB`,
2. installs the WinUSB driver package for the cable (`op20winusb.inf` + `.cat`); because
   the catalog is self-signed, its certificate is first added to this PC's trusted
   certificates,
3. removes the original Tactrix kernel driver (`openport.sys`) if present, since Windows 11
   blocks it,
4. registers the DLLs under `PassThruSupport.04.04` for 32-bit and 64-bit programs,
5. appears in "Installed apps" as "Op20WinUSB - Tactrix Openport 2.0 J2534 driver".

The installer is a wizard in a single window, without message boxes:

1. **Welcome** – what Setup is going to do, with **Install** and **Cancel**.
2. **Progress** – a progress bar and the current step.
3. **Result** – whether Op20WinUSB was installed, which driver the cable is bound to and
   what to select in your J2534 program, with **Exit**. If the driver step fails, the
   reason is shown here; if Setup fails or is cancelled, the page says so.

Running the package again when this version is already installed offers **Repair** and
**Remove**. Uninstalling ends with a result page too. Silent installs (`/qn`) show nothing.
Installing a newer build replaces the previous one automatically.

The only separate prompt is the Windows request for administrator rights (UAC), which no
installer can draw inside its own window.

In J2534 applications then select **"Op20WinUSB - Openport 2.0 (K-line, CAN)"**.

Because the package is not signed with a commercial certificate, Windows shows "Unknown
publisher" in the UAC prompt and SmartScreen may ask for confirmation.

Command line:

```
msiexec /i Op20WinUSB-Setup.msi                     install
msiexec /i Op20WinUSB-Setup.msi /qn                 install silently (from an elevated prompt)
msiexec /i Op20WinUSB-Setup.msi /l*v install.log    install with a verbose log
msiexec /x Op20WinUSB-Setup.msi                     uninstall
```

The driver and certificate steps are logged to `Op20WinUSB-driver-setup.log` in the SYSTEM
account's temp folder (`C:\Windows\SystemTemp` on Windows 11, `C:\Windows\Temp` on older
versions); reading it needs administrator rights.

**Uninstall:** Settings → Apps → Installed apps → "Op20WinUSB - Tactrix Openport 2.0 J2534
driver". It removes the registration, the files, this project's INF and its certificate;
OPTA and Tactrix drivers are left alone.

Check after installing:

```powershell
& "C:\Program Files\Op20WinUSB\optest32.exe" info
```

It should print the firmware version, the voltage on pin 16 and the serial number.

### Programs that load `op20pt32.dll` by name

Some programs (e.g. EcuFlash) load the Tactrix DLL by file name and do not use the J2534
registry. For those, `replace-op20pt32.ps1` puts this DLL in place of
`C:\Windows\SysWOW64\op20pt32.dll`; the original is backed up and put back by
`replace-op20pt32.ps1 -Restore` or by uninstalling. The script asks for administrator
rights itself. This is experimental.

## Driver package (INF + CAT + DLL)

The `driver` folder holds the package in the same form as OPTA's; the MSI contains exactly
these files.

| File | Purpose |
|---|---|
| `op20winusb.inf` | binds the Windows WinUSB driver to the Openport 2.0 |
| `op20winusb.cat` | signed catalog with the hashes of the package files |
| `op20winusb32.dll` | J2534 library for 32-bit programs |
| `op20winusb64.dll` | J2534 library for 64-bit programs (OPTA has none) |

The catalog is signed with a self-signed certificate because there is no commercial one.
Windows installs such a package only after the certificate has been added to "Trusted Root
Certification Authorities" and "Trusted Publishers", which the installer does. The
certificate's private key was deleted right after signing, so it cannot sign anything else.

A comparison with OPTA is in [COMPARISON.md](COMPARISON.md).

## Test tool `optest`

```
optest32 info                      versions, battery voltage, serial number
optest32 check                     opens every channel type; transmits nothing
optest32 obd [baud] [pause_ms] [raw]   5-baud init + read-only OBD requests (ISO9141-2)
optest32 sniff [k|l|aux] [baud]    listen on K-line (no checksum handling)
optest32 5baud 33 [baud] [raw]     5-baud init on address 0x33, prints the key bytes and
                                   what the ECU sends (raw = no checksum handling;
                                   e.g. EDC15P: 5baud 01 9600 raw)
optest32 fastinit [baud]           ISO14230 fast init (StartCommunication)
optest32 fastdiag                  times FAST_INIT under different settings: shows whether the
                                   firmware rejected the command or ran the init and got no answer
optest32 kwp [baud] [addr] [idle_ms] [attempts] [raw|auto] [P4_MIN] [request hex]
                                   repeated KWP2000 fast inits in raw mode, printing every byte
optest32 edc15 [aty|baud|pin] [attempts] [pause_ms] [P4_MIN]
                                   Bosch EDC15 KWP2000 entry: wake-up, StartDiagnosticSession,
                                   StopCommunication; reports how many attempts succeeded
optest32 kw1281 [baud] [addr]      complete KW1281 session (5-baud init, byte-by-byte handshake),
                                   prints the ECU identification blocks
optest32 can [baud]                listen on CAN
```

`--secs <n>` before the command stops reading after n seconds instead of waiting for a key.

## Debug log

Set the environment variable `OP20WINUSB_LOG` to a file path (or the value `LogFile` under
`HKCU\Software\Op20WinUSB`). API calls and all USB traffic are logged.

```powershell
$env:OP20WINUSB_LOG = "$env:TEMP\op20winusb.log"
```

The program has to be started from the same PowerShell window to see the variable.

## What is implemented

| Function | Status |
|---|---|
| ISO9141, ISO14230 (K-line), L-line, aux RS-232 | yes |
| FIVE_BAUD_INIT, FAST_INIT | yes |
| CAN, ISO15765 (incl. flow control filters and mixed mode) | yes |
| Filters (pass / block / flow control), StopMsgFilter, CLEAR_MSG_FILTERS | yes |
| Periodic messages | yes |
| GET_CONFIG / SET_CONFIG, READ_VBATT, READ_PROG_VOLTAGE | yes |
| PassThruSetProgrammingVoltage (incl. the aux jack) | yes |
| TX_IOCTL_APP_SERVICE (e.g. serial number) | yes |
| J1850 VPW/PWM | no (not supported by the device) |
| Firmware update, several devices at once | no |

## Building

Needs Visual Studio with the C++ workload and the Windows SDK.

1. `build.cmd` – builds the DLLs and tools into `out\x86` and `out\x64` and runs the
   self-test (`tests\selftest.cpp`).
2. `make-driver-package.ps1 -SelfSign` – copies the DLLs into `driver`, creates the catalog
   and signs it with a new one-off certificate (the private key is deleted afterwards).
3. `build-installer.cmd` – builds the helper program and then `Op20WinUSB-Setup.msi`
   (`build-msi.ps1`). The MSI is created with the Windows Installer automation interface
   and `makecab`, both part of Windows; no WiX or other tooling is needed.

After rebuilding the DLLs, steps 2 and 3 have to be repeated, otherwise the hashes in the
catalog no longer match.

## Checking a package without installing it

```
msiexec /a Op20WinUSB-Setup.msi /qn TARGETDIR=C:\some\folder    unpack the files only
C:\some\folder\Op20WinUSB\op20winusb-driver-setup.exe /check     report what the driver step would do
```

## Layout

- `Op20WinUSB-Setup.msi` – the installer, built by `build-installer.cmd` and published on the
  Releases page (it is not kept in git)
- `driver/` – the driver package (INF + CAT + DLLs)
- `src/transport.*` – WinUSB and USB serial
- `src/device.*` – device protocol (see [PROTOCOL.md](PROTOCOL.md))
- `src/api.cpp` – the exported `PassThru*` functions
- `installer/` – helper program the MSI runs for the driver and certificate steps, and the
  small DLL that fills in the text of the result page
- `build-msi.ps1` – builds the MSI
- `tools/optest.cpp` – test tool
