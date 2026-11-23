# OPTA vs. Op20WinUSB

Both drivers are the same kind of thing: a user-mode J2534 library (DLL) that talks to the
Openport 2.0 through WinUSB (`winusb.sys`, part of Windows), so neither needs the blocked
`openport.sys`. They differ in how much of the device protocol they implement.

**Where the data comes from:** the OPTA column is based on the messages inside
`opta_j2534_0404_tactrix.dll` (version 1.0.0.3) – for unsupported functions the library
itself says so. Where there is no trace of a function in it, the table says "not found";
this was not verified by calling it. The Op20WinUSB column is based on the source code and
on tests with real hardware.

## J2534 functions

| Function | OPTA | Op20WinUSB |
|---|---|---|
| Open / Close, Connect / Disconnect | yes | yes |
| ReadMsgs / WriteMsgs | yes | yes |
| StartMsgFilter (pass / block / flow control) | yes | yes |
| **StopMsgFilter** | no – "not supported by Tactrix wire protocol", advises to disconnect and reconnect the channel | yes (`atk`) |
| **CLEAR_MSG_FILTERS** | host side only – the filters stay in the device | yes, in the device (`atk -1`) |
| **FIVE_BAUD_INIT** (5-baud init, ISO9141 / KW1281) | no – "not implemented for Tactrix" | yes (`atw`) |
| FAST_INIT (KWP2000) | yes | yes |
| **Periodic messages** (Start / Stop / CLEAR_PERIODIC_MSGS) | no – "not implemented for Tactrix" | yes (`atm`, `atn`, `atl`) |
| SET_CONFIG | yes | yes |
| **GET_CONFIG** | not found | yes (`atg`) |
| READ_VBATT | yes | yes |
| **READ_PROG_VOLTAGE** (voltage on any pin) | not found | yes |
| **PassThruSetProgrammingVoltage** (OBD pins, aux jack) | not found | yes (`atv`) |
| **L-line** (ISO9141_CH2, ISO14230_CH2) | not found | yes |
| **Aux input** – RS-232 on the 2.5 mm jack (ISO9141_CH3) | not found | yes |
| CAN, ISO15765 | yes | yes |
| **CAN frames on an ISO15765 channel** (mixed mode) | not found | yes |
| **Blocking write** (WriteMsgs with a timeout waits for transmission) | not found | yes |
| **TX_IOCTL_APP_SERVICE** (e.g. the cable's serial number) | not found | yes |
| J1850 VPW / PWM | no | no |

## Other differences

| | OPTA | Op20WinUSB |
|---|---|---|
| 32-bit applications | yes | yes |
| **64-bit applications** | no (32-bit DLL only) | yes (second DLL) |
| USB access | libusb (bundled in the DLL) | WinUSB API directly |
| **Fallback without an INF** | no | yes – also works through the Windows USB serial driver (COM port) |
| Signature of `.cat` and DLL | commercial EV certificate (MCU Innovations) | self-signed catalog, unsigned DLL |
| Installer | MSI (32-bit) | MSI (64-bit), also removes the blocked Tactrix driver |
| Debug log of all traffic | yes | yes (`OP20WINUSB_LOG`) |
| Source code | not available | part of this project |
| Test tool | no | `optest` |

## Why this matters for K-line

A K-line ECU has to be woken up before it talks. There are two ways:

- **fast init** (KWP2000) – OPTA can do this,
- **5-baud init** (ISO9141-2, KW1281 and older ECUs) – OPTA cannot.

A Bosch EDC15P, for example, only answers a 5-baud init (address `0x01`, 9600 baud, key
bytes `01 8A`) and rejects a fast init. OPTA therefore cannot connect to it over K-line;
Op20WinUSB can.

The second point is periodic messages: a K-line session expires after a few seconds of
silence and programs keep it alive with a periodic "tester present" message. Without
periodic messages in the driver the program has to send the keep-alive itself, and not
every program does.

## Verified on real hardware

Openport 2.0, firmware 1.17.4877, Bosch EDC15P on the bench:

- opening and configuring every channel type (K, L, aux, CAN, ISO15765), filters including
  removal,
- 5-baud init on address `0x33` (key bytes `08 08`) and `0x01` (key bytes `01 8A`),
- transmitting on K-line with correct timing and receiving the echo,
- receiving the bytes the ECU sends on its own after the init.

Not verified: a complete request/response exchange inside an application, fast init with
an ECU that supports it, CAN / ISO15765 traffic, periodic messages on the bus, programming
voltage.
