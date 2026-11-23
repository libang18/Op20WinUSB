# Openport 2.0 wire protocol

The device (USB `VID_0403&PID_CC4D`) is a USB CDC-ACM device; its data interface (bulk
IN/OUT) carries a mixed text/binary stream. This description follows the behaviour of the
original `op20pt32.dll` (2016 firmware/DLL) and of open-source implementations
(dschultzca/j2534, FastECU), and was confirmed against firmware 1.17.4877.

## Commands (PC → device)

An ASCII line terminated by `\r\n`, optionally followed by binary data. The last number,
`<seq>` (1..65535), is an optional sequence number; if present the device repeats it in
its reply. `<ch>` is the channel number on the wire (see below).

| Command | Meaning | Reply |
|---|---|---|
| `\r\n\r\nati\r\n` | identify | `ari main code version : <version>` |
| `ata <seq>` | open the device | `aro <seq>` |
| `atz <seq>` | close | `aro <seq>` |
| `ato<ch> <flags> <baud> <protocolID> <seq>` | PassThruConnect | `aro <seq>` |
| `atc<ch> <seq>` | PassThruDisconnect | `aro <seq>` |
| `att<ch> <len> <txflags> <timeout_us> [<seq>]` + data | PassThruWriteMsgs | `aro [<seq>]` |
| `atf<ch> <type> <txflags> <len> <seq>` + mask + pattern [+ flow control] | StartMsgFilter | `arf<ch> <id> <seq>` |
| `atk<ch> <id> <seq>` | StopMsgFilter (`id` = -1: all) | `aro <seq>` |
| `atm<ch> <interval_us> <0> <txflags> <len> <seq>` + data | StartPeriodicMsg | `arm<ch> <id> <seq>` |
| `atn<ch> <id> <seq>` | StopPeriodicMsg | `aro <seq>` |
| `atl<ch> <seq>` | CLEAR_PERIODIC_MSGS | `aro <seq>` |
| `ats<ch> <param> <value> <seq>` | SET_CONFIG | `aro <seq>` |
| `atg<ch> <param> <seq>` | GET_CONFIG | `arg<ch> <param> <value> <seq>` |
| `atw<ch> <address> <seq>` | FIVE_BAUD_INIT | `arw<ch> <kb1> <kb2> <seq>` |
| `aty<ch> <len> <txflags> <seq>` + data | FAST_INIT | `ary<ch> <len> <seq>` + data |
| `atr <pin> <seq>` | read pin voltage (16 = VBATT) | `arr <pin> <mV> <seq>` |
| `atv <pin> <voltage> <seq>` | SetProgrammingVoltage (mV; -1 = off, -2 = short to ground) | `aro <seq>` |
| `atp <service> <len> <seq>` + data | TX_IOCTL_APP_SERVICE | `arp <service> <len> <seq>` + data |

Error: `are <J2534 code> [<seq>]`. Lines starting with `arb` are debug output and ignored.

`att` without `<seq>` is a non-blocking write (Timeout 0); `timeout_us` is then the
default (50 000 for CAN, 1 000 000 for everything else).

## Channels

| ProtocolID | `<ch>` |
|---|---|
| ISO9141 (3), ISO9141_CH1 (0x9240) – K-line | 3 |
| ISO14230 (4), ISO14230_CH1 (0x9320) – K-line | 4 |
| CAN (5), CAN_CH1 (0x9000) | 5 |
| ISO15765 (6), ISO15765_CH1 (0x9400) | 6 |
| ISO9141_CH2 (0x9241) – L-line | 7 |
| ISO14230_CH2 (0x9321) – L-line | 8 |
| ISO9141_CH3 (0x9242) – RS-232 on the 2.5 mm jack, receive only | 9 |

The ChannelID returned to the application equals the ProtocolID (as in the original DLL).

## Message packets (device → PC)

```
'a' 'r' <'0'+ch> <len> <type> <len-1 bytes>
```

Bits of `type`: `0x80` start of message, `0x40` end of message, `0x20` loopback (echo of a
transmission), `0x10` TX done (ISO15765), `0x08` do not pass to the application, `0x04`
extended addressing, `0x02` 29-bit ID. Timestamps are 4 bytes, big-endian, in microseconds.

**K-line (channels 3, 4, 7, 8, 9)** – a message arrives as:

1. start (`0x80`, payload = timestamp) → the application gets a `START_OF_MESSAGE` message,
2. data packets (`0x00` / `0x20`, payload = bare data, no timestamp),
3. end (`0x40` / `0x60`, payload = timestamp) → the application gets the assembled message.

**CAN (5)** – one packet = one frame: `<timestamp:4> <ID:4> <data>`.

**ISO15765 (6)** – every packet: `<timestamp:4> <address:4 or 5> <data>`; `0x10` = TX done,
`0x80` = first-frame indication (address only), no flag = continuation of a segmented
message, `0x40` = end (the only packet of a single-frame message).
