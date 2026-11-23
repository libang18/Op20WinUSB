# Op20WinUSB

A J2534 driver for the Tactrix Openport 2.0 that works on Windows 11 and supports K-line.

## Why it exists

The original Tactrix driver relies on a kernel driver signed in 2014. Windows 11 blocks it
when Memory Integrity is on, so the cable stops working.

The WinUSB-based replacement that is available (OPTA) is 32-bit only and has no 5-baud
init and no periodic messages. Older K-line ECUs answer only a 5-baud init, so they cannot
be reached with it. A comparison is in [COMPARISON.md](COMPARISON.md).

## What it does

- It is a J2534 04.04 library for 32-bit and 64-bit programs.
- It talks to the cable through a driver that ships with Windows (WinUSB, or USB serial as
  a fallback). It has no kernel driver of its own, so Secure Boot and Memory Integrity
  stay on.
- It supports K-line (ISO9141 and ISO14230 with 5-baud and fast init), L-line, CAN and
  ISO15765, message filters, periodic messages and pin voltages.
- It writes nothing to the cable itself.

J1850, firmware updates and several cables at once are not supported.

## Installation

Download `Op20WinUSB-Setup.msi` from the
[latest release](https://github.com/libang18/Op20WinUSB/releases/latest) and run it. Then
select **Op20WinUSB - Openport 2.0 (K-line, CAN)** in your J2534 program.

Before you install:

- The package is not signed with a commercial certificate, so Windows shows "Unknown
  publisher".
- The driver catalog is self-signed. The installer adds its certificate to this PC's
  trusted certificates so that Windows accepts the driver. Uninstalling removes it again.
- The installer removes the original Tactrix kernel driver if it is present.

To uninstall, use Settings → Apps → Installed apps.

## Status

The driver is used with an Openport 2.0 (firmware 1.17.4877) and a Bosch EDC15 ECU on
K-line: identification over KW1281, and reading and writing the flash and EEPROM over
KWP2000 at up to 124800 baud.

CAN, ISO15765 and periodic messages have not been tested on a vehicle. Try reading with
your program before you write anything to an ECU.

## Building

You need Visual Studio with the C++ workload and the Windows SDK. Run `build.cmd`, then
`make-driver-package.ps1 -SelfSign`, then `build-installer.cmd`.

## More

- [NOTES.md](NOTES.md) – K-line settings for application authors, installer details, the
  `optest` tool and the debug log
- [PROTOCOL.md](PROTOCOL.md) – the cable's wire protocol
- [COMPARISON.md](COMPARISON.md) – comparison with OPTA
