// optest - small console tester for the Openport 2.0 WinUSB J2534 DLL.
//
//   optest [--dll <path>] [--secs <n>] <command>
//   optest [--dll <path>] info
//   optest [--dll <path>] check          (opens every channel type; transmits nothing)
//   optest [--dll <path>] sniff [k|l|aux] [baud] [none|odd|even]
//   optest [--dll <path>] 5baud <address hex> [baud]
//   optest [--dll <path>] fastinit [baud]
//   optest [--dll <path>] can [baud]
//
// Without --dll it loads op20winusb32.dll / op20winusb64.dll from its own folder.

#include <windows.h>

#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/j2534.h"

#define P1_MAX 0x07
#define PARITY 0x16
#define ISO9141_NO_CHECKSUM 0x00000200
#define K_LINE_ONLY 0x00001000

typedef long(__stdcall *FnOpen)(const void *, unsigned long *);
typedef long(__stdcall *FnClose)(unsigned long);
typedef long(__stdcall *FnConnect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long(__stdcall *FnDisconnect)(unsigned long);
typedef long(__stdcall *FnReadMsgs)(unsigned long, PASSTHRU_MSG *, unsigned long *, unsigned long);
typedef long(__stdcall *FnWriteMsgs)(unsigned long, const PASSTHRU_MSG *, unsigned long *, unsigned long);
typedef long(__stdcall *FnStartFilter)(unsigned long, unsigned long, const PASSTHRU_MSG *, const PASSTHRU_MSG *,
                                       const PASSTHRU_MSG *, unsigned long *);
typedef long(__stdcall *FnReadVersion)(unsigned long, char *, char *, char *);
typedef long(__stdcall *FnGetLastError)(char *);
typedef long(__stdcall *FnIoctl)(unsigned long, unsigned long, const void *, void *);

static FnOpen ptOpen;
static FnClose ptClose;
static FnConnect ptConnect;
static FnDisconnect ptDisconnect;
static FnReadMsgs ptReadMsgs;
static FnWriteMsgs ptWriteMsgs;
static FnStartFilter ptStartFilter;
static FnReadVersion ptReadVersion;
static FnGetLastError ptGetLastError;
static FnIoctl ptIoctl;

static unsigned long g_dev = 0;
static unsigned long g_seconds = 0;  // --secs: stop reading after this long instead of on a key press

static bool check(long rc, const char *what) {
    if (rc == STATUS_NOERROR) return true;
    char err[80] = "";
    ptGetLastError(err);
    printf("%s failed: error 0x%02lX [%s]\n", what, rc, err);
    return false;
}

static void usage() {
    printf("optest [--dll <path>] [--secs <n>] <command>\n"
           "optest [--dll <path>] info\n"
           "optest [--dll <path>] check\n"
           "optest [--dll <path>] sniff [k|l|aux] [baud] [none|odd|even]\n"
           "optest [--dll <path>] 5baud <address hex> [baud]\n"
           "optest [--dll <path>] obd [baud]\n"
           "optest [--dll <path>] fastinit [baud]\n"
           "optest [--dll <path>] fastdiag\n"
           "optest [--dll <path>] kw1281 [baud] [address hex]\n"
           "optest [--dll <path>] kwp [baud] [address hex] [idle ms] [attempts]\n"
           "optest [--dll <path>] can [baud]\n");
}

static void printMsg(const PASSTHRU_MSG &m) {
    printf("[%10lu] rx=%04lX ", m.Timestamp, m.RxStatus);
    for (unsigned long i = 0; i < m.DataSize; i++) printf("%02X ", m.Data[i]);
    printf("\n");
}

static bool passAllFilter(unsigned long chan, unsigned long protocol, unsigned long size) {
    PASSTHRU_MSG mask = {}, pattern = {};
    mask.ProtocolID = pattern.ProtocolID = protocol;
    mask.DataSize = pattern.DataSize = size;
    unsigned long id = 0;
    return check(ptStartFilter(chan, PASS_FILTER, &mask, &pattern, nullptr, &id), "PassThruStartMsgFilter");
}

static void readLoop(unsigned long chan, bool skipStart) {
    if (g_seconds) printf("Reading for %lu s...\n", g_seconds);
    else printf("Reading. Press any key to stop...\n");
    unsigned long total = 0;
    const ULONGLONG stopAt = GetTickCount64() + g_seconds * 1000ULL;
    while (g_seconds ? GetTickCount64() < stopAt : !_kbhit()) {
        static PASSTHRU_MSG msg;
        unsigned long n = 1;
        ptReadMsgs(chan, &msg, &n, 250);
        if (!n) continue;
        if (skipStart && (msg.RxStatus & START_OF_MESSAGE)) continue;
        printMsg(msg);
        total++;
    }
    if (!g_seconds) (void)_getch();
    printf("%lu messages\n", total);
}

static int cmdInfo() {
    char fw[80], dll[80], api[80];
    if (!check(ptReadVersion(g_dev, fw, dll, api), "PassThruReadVersion")) return 1;
    printf("Firmware : %s\nDLL      : %s\nAPI      : %s\n", fw, dll, api);

    unsigned long mv = 0;
    if (check(ptIoctl(g_dev, READ_VBATT, nullptr, &mv), "READ_VBATT"))
        printf("Pin 16   : %lu.%03lu V\n", mv / 1000, mv % 1000);

    // Serial number through the Tactrix application service (service 5, item 1).
    struct {
        unsigned long length, service;
        unsigned short item;
    } in = {2, 5, 1};
    struct {
        unsigned long length;
        unsigned char data[256];
    } out = {sizeof(out.data), {}};
    if (ptIoctl(g_dev, TX_IOCTL_APP_SERVICE, &in, &out) == STATUS_NOERROR && out.length < sizeof(out.data)) {
        out.data[out.length] = 0;
        printf("Serial   : %s\n", out.data);
    }
    return 0;
}

// Opens each channel type and exercises configuration and filters. Nothing is
// transmitted on the vehicle side.
static int cmdCheck() {
    struct {
        const char *name;
        unsigned long protocol, flags, baud, filterSize;
        bool kline;
    } const cases[] = {
        {"ISO9141 (K)", ISO9141, 0, 10400, 1, true},
        {"ISO14230 (K)", ISO14230, 0, 10400, 1, true},
        {"ISO9141_CH2 (L)", ISO9141_CH2, ISO9141_NO_CHECKSUM, 10400, 1, true},
        {"ISO9141_CH3 (aux)", ISO9141_CH3, ISO9141_NO_CHECKSUM, 19200, 1, true},
        {"CAN", CAN, 0, 500000, 4, false},
        {"ISO15765", ISO15765, 0, 500000, 4, false},
    };
    int failures = 0;
    for (const auto &c : cases) {
        printf("%-18s ", c.name);
        unsigned long chan = 0;
        if (!check(ptConnect(g_dev, c.protocol, c.flags, c.baud, &chan), "connect")) {
            failures++;
            continue;
        }
        bool ok = true;
        SCONFIG get[4] = {{0x01, 0}, {0x03, 0}, {P1_MAX, 0}, {PARITY, 0}};  // DATA_RATE, LOOPBACK, ...
        SCONFIG_LIST getList = {c.kline ? 4UL : 2UL, get};
        ok &= check(ptIoctl(chan, GET_CONFIG, &getList, nullptr), "GET_CONFIG");
        SCONFIG set[1] = {{0x03, 0}};  // LOOPBACK off
        SCONFIG_LIST setList = {1, set};
        ok &= check(ptIoctl(chan, SET_CONFIG, &setList, nullptr), "SET_CONFIG");

        static PASSTHRU_MSG mask, pattern, flow;
        memset(&mask, 0, sizeof(mask));
        memset(&pattern, 0, sizeof(pattern));
        memset(&flow, 0, sizeof(flow));
        mask.ProtocolID = pattern.ProtocolID = flow.ProtocolID = c.protocol;
        mask.DataSize = pattern.DataSize = flow.DataSize = c.filterSize;
        unsigned long f1 = 0, f2 = 0;
        if (c.protocol != ISO15765) {
            ok &= check(ptStartFilter(chan, PASS_FILTER, &mask, &pattern, nullptr, &f1), "StartMsgFilter");
            ok &= check(ptStartFilter(chan, PASS_FILTER, &mask, &pattern, nullptr, &f2), "StartMsgFilter #2");
        } else {
            const unsigned char rxId[4] = {0x00, 0x00, 0x07, 0xE8}, txId[4] = {0x00, 0x00, 0x07, 0xE0};
            memset(mask.Data, 0xFF, 4);
            memcpy(pattern.Data, rxId, 4);
            memcpy(flow.Data, txId, 4);
            ok &= check(ptStartFilter(chan, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &f1), "flow control filter");
        }
        ok &= check(ptIoctl(chan, CLEAR_MSG_FILTERS, nullptr, nullptr), "CLEAR_MSG_FILTERS");
        ok &= check(ptIoctl(chan, CLEAR_PERIODIC_MSGS, nullptr, nullptr), "CLEAR_PERIODIC_MSGS");
        ok &= check(ptIoctl(chan, CLEAR_RX_BUFFER, nullptr, nullptr), "CLEAR_RX_BUFFER");
        ok &= check(ptDisconnect(chan), "disconnect");
        if (ok) {
            printf("OK   rate=%lu loopback=%lu", get[0].Value, get[1].Value);
            if (c.kline) printf(" P1_MAX=%lu parity=%lu", get[2].Value, get[3].Value);
            printf("  filter ids %lu,%lu\n", f1, f2);
        } else {
            failures++;
        }
    }
    if (failures) printf("check: %d channel type(s) FAILED\n", failures);
    else printf("check: all channel types OK\n");
    return failures ? 1 : 0;
}

static int cmdSniff(int argc, char **argv) {
    unsigned long protocol = ISO9141, baud = 10400, parity = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "k")) protocol = ISO9141_CH1;
        else if (!strcmp(argv[i], "l")) protocol = ISO9141_CH2;
        else if (!strcmp(argv[i], "aux")) protocol = ISO9141_CH3;
        else if (!strcmp(argv[i], "none")) parity = 0;
        else if (!strcmp(argv[i], "odd")) parity = 1;
        else if (!strcmp(argv[i], "even")) parity = 2;
        else baud = strtoul(argv[i], nullptr, 10);
    }
    unsigned long chan = 0;
    if (!check(ptConnect(g_dev, protocol, ISO9141_NO_CHECKSUM, baud, &chan), "PassThruConnect")) return 1;
    SCONFIG cfg[2] = {{P1_MAX, 40}, {PARITY, parity}};  // 20 ms gap ends a message
    SCONFIG_LIST list = {2, cfg};
    if (!check(ptIoctl(chan, SET_CONFIG, &list, nullptr), "SET_CONFIG")) return 1;
    if (!passAllFilter(chan, protocol, 1)) return 1;
    printf("K-line sniff, protocol 0x%lX, %lu baud\n", protocol, baud);
    readLoop(chan, true);
    check(ptDisconnect(chan), "PassThruDisconnect");
    return 0;
}

static int cmdFiveBaud(int argc, char **argv) {
    if (argc < 1) {
        usage();
        return 1;
    }
    unsigned char address = static_cast<unsigned char>(strtoul(argv[0], nullptr, 16));
    unsigned long baud = argc > 1 ? strtoul(argv[1], nullptr, 10) : 10400;
    // "raw": no checksum handling, so single bytes and non-OBD protocols (KW1281) are shown
    const bool raw = argc > 2 && !strcmp(argv[2], "raw");
    unsigned long chan = 0;
    if (!check(ptConnect(g_dev, ISO9141, raw ? ISO9141_NO_CHECKSUM : 0, baud, &chan), "PassThruConnect")) return 1;
    if (!passAllFilter(chan, ISO9141, 1)) return 1;

    unsigned char keys[8] = {};
    SBYTE_ARRAY in = {1, &address}, out = {0, keys};
    printf("5-baud init, address 0x%02X ...\n", address);
    if (check(ptIoctl(chan, FIVE_BAUD_INIT, &in, &out), "FIVE_BAUD_INIT")) {
        printf("Key bytes:");
        for (unsigned long i = 0; i < out.NumOfBytes; i++) printf(" %02X", keys[i]);
        printf("\n");
        readLoop(chan, true);
    }
    check(ptDisconnect(chan), "PassThruDisconnect");
    return 0;
}

// ISO9141-2 session: 5-baud init on 0x33, then a few read-only OBD requests.
static int cmdObd(int argc, char **argv) {
    unsigned long baud = argc > 0 ? strtoul(argv[0], nullptr, 10) : 10400;
    unsigned long delayMs = argc > 1 ? strtoul(argv[1], nullptr, 10) : 300;  // pause between init and first request
    // "raw": the device neither adds nor checks checksums, so every byte on the line is shown
    const bool raw = argc > 2 && !strcmp(argv[2], "raw");
    unsigned long chan = 0;
    if (!check(ptConnect(g_dev, ISO9141, raw ? ISO9141_NO_CHECKSUM : 0, baud, &chan), "PassThruConnect")) return 1;
    if (!passAllFilter(chan, ISO9141, 1)) return 1;
    SCONFIG loopback[1] = {{0x03, 1}};  // LOOPBACK on: our own requests come back marked as echo
    SCONFIG_LIST loopbackList = {1, loopback};
    check(ptIoctl(chan, SET_CONFIG, &loopbackList, nullptr), "SET_CONFIG LOOPBACK");
    // Timing the device will use (units per J2534: P* and W* in 0.5 ms / 1 ms steps).
    SCONFIG timing[] = {{0x0A, 0}, {0x0C, 0}, {0x07, 0}, {0x0E, 0}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {0x12, 0}, {0x21, 0}};
    SCONFIG_LIST timingList = {sizeof(timing) / sizeof(timing[0]), timing};
    if (ptIoctl(chan, GET_CONFIG, &timingList, nullptr) == STATUS_NOERROR)
        printf("P3_MIN=%lu P4_MIN=%lu P1_MAX=%lu W1=%lu W2=%lu W3=%lu W4=%lu W5=%lu FIVE_BAUD_MOD=%lu\n", timing[0].Value,
               timing[1].Value, timing[2].Value, timing[3].Value, timing[4].Value, timing[5].Value, timing[6].Value,
               timing[7].Value, timing[8].Value);

    unsigned char address = 0x33, keys[8] = {};
    SBYTE_ARRAY in = {1, &address}, out = {0, keys};
    int rc = 1;
    if (check(ptIoctl(chan, FIVE_BAUD_INIT, &in, &out), "FIVE_BAUD_INIT")) {
        printf("Key bytes: %02X %02X\n", keys[0], keys[1]);
        // The ECU ignores a request sent too soon after the init. Anything it
        // sends in the meantime is shown.
        const ULONGLONG quietUntil = GetTickCount64() + delayMs;
        while (GetTickCount64() < quietUntil) {
            static PASSTHRU_MSG idle;
            unsigned long n = 1;
            ptReadMsgs(chan, &idle, &n, 50);
            if (n && !(idle.RxStatus & START_OF_MESSAGE)) {
                printf("  after init: ");
                printMsg(idle);
            }
        }
        // mode/PID pairs: supported PIDs, coolant temperature, RPM, vehicle speed
        const unsigned char requests[][2] = {{0x01, 0x00}, {0x01, 0x05}, {0x01, 0x0C}, {0x01, 0x0D}};
        rc = 0;
        for (const auto &r : requests) {
            static PASSTHRU_MSG tx, rx;
            memset(&tx, 0, sizeof(tx));
            tx.ProtocolID = ISO9141;
            tx.DataSize = 5;  // checksum is appended by the device
            tx.Data[0] = 0x68;
            tx.Data[1] = 0x6A;
            tx.Data[2] = 0xF1;
            tx.Data[3] = r[0];
            tx.Data[4] = r[1];
            if (raw) {
                unsigned char sum = 0;
                for (int i = 0; i < 5; i++) sum = static_cast<unsigned char>(sum + tx.Data[i]);
                tx.Data[5] = sum;
                tx.DataSize = 6;
            }
            printf("> 68 6A F1 %02X %02X\n", r[0], r[1]);
            unsigned long n = 1;
            if (!check(ptWriteMsgs(chan, &tx, &n, 500), "PassThruWriteMsgs")) {
                rc = 1;
                break;
            }
            const ULONGLONG until = GetTickCount64() + 600;
            while (GetTickCount64() < until) {
                n = 1;
                ptReadMsgs(chan, &rx, &n, 100);
                if (!n) continue;
                printf("  %s ", (rx.RxStatus & START_OF_MESSAGE) ? "start" : (rx.RxStatus & TX_MSG_TYPE) ? "echo " : "ECU  ");
                printMsg(rx);
            }
        }
    }
    check(ptDisconnect(chan), "PassThruDisconnect");
    return rc;
}

static int cmdFastInit(int argc, char **argv) {
    unsigned long baud = argc > 0 ? strtoul(argv[0], nullptr, 10) : 10400;
    unsigned long chan = 0;
    if (!check(ptConnect(g_dev, ISO14230, 0, baud, &chan), "PassThruConnect")) return 1;
    if (!passAllFilter(chan, ISO14230, 1)) return 1;

    static PASSTHRU_MSG req, resp;
    req.ProtocolID = ISO14230;
    req.DataSize = 4;  // StartCommunication, functional address; checksum is added by the device
    req.Data[0] = 0xC1;
    req.Data[1] = 0x33;
    req.Data[2] = 0xF1;
    req.Data[3] = 0x81;
    printf("Fast init (StartCommunication C1 33 F1 81) ...\n");
    if (check(ptIoctl(chan, FAST_INIT, &req, &resp), "FAST_INIT")) {
        printf("Response: ");
        printMsg(resp);
        readLoop(chan, true);
    }
    check(ptDisconnect(chan), "PassThruDisconnect");
    return 0;
}

// ---------------------------------------------------------------------------
// fastdiag: how the firmware behaves on FAST_INIT. For each case it reports the
// J2534 result and how long the device took to answer, then whatever appeared
// on the line. A rejected command comes back within a millisecond or two; an
// init that was actually performed takes at least TINIL + TWUP + the request +
// the wait for the ECU.

static double nowMs() {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return 1000.0 * static_cast<double>(count.QuadPart) / static_cast<double>(freq.QuadPart);
}

static long setCfg(unsigned long chan, unsigned long param, unsigned long value) {
    SCONFIG cfg = {param, value};
    SCONFIG_LIST list = {1, &cfg};
    return ptIoctl(chan, SET_CONFIG, &list, nullptr);
}

static unsigned long getCfg(unsigned long chan, unsigned long param) {
    SCONFIG cfg = {param, 0};
    SCONFIG_LIST list = {1, &cfg};
    return ptIoctl(chan, GET_CONFIG, &list, nullptr) == STATUS_NOERROR ? cfg.Value : 0xFFFFFFFF;
}

static void drain(unsigned long chan, unsigned long ms, const char *indent) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        static PASSTHRU_MSG m;
        unsigned long n = 1;
        ptReadMsgs(chan, &m, &n, 50);
        if (!n || (m.RxStatus & START_OF_MESSAGE)) continue;
        printf("%s%s ", indent, (m.RxStatus & TX_MSG_TYPE) ? "echo" : "line");
        printMsg(m);
    }
}

struct FastCase {
    const char *name;
    unsigned long protocol, baud;
    int msgLen;  // -1: NULL message
    unsigned char msg[8];
    unsigned long tinil, twup;  // 0: leave the default
    bool loopback;
    unsigned long p4min, p2max;  // 0: leave the default (units of 0.5 ms)
};

static void runFastCase(const FastCase &c) {
    unsigned long chan = 0;
    printf("%-44s ", c.name);
    if (ptConnect(g_dev, c.protocol, K_LINE_ONLY, c.baud, &chan) != STATUS_NOERROR) {
        check(1, "connect");
        return;
    }
    if (c.protocol != CAN) passAllFilter(chan, c.protocol, 1);
    if (c.loopback) setCfg(chan, 0x03, 1);
    if (c.tinil) setCfg(chan, 0x14, c.tinil);
    if (c.twup) setCfg(chan, 0x15, c.twup);
    if (c.p4min) setCfg(chan, 0x0C, c.p4min);
    long p2rc = 0;
    if (c.p2max) p2rc = setCfg(chan, 0x09, c.p2max);
    const unsigned long tinil = getCfg(chan, 0x14), twup = getCfg(chan, 0x15);

    static PASSTHRU_MSG req, resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.ProtocolID = c.protocol;
    if (c.msgLen > 0) {
        req.DataSize = static_cast<unsigned long>(c.msgLen);
        memcpy(req.Data, c.msg, static_cast<size_t>(c.msgLen));
    }
    const double start = nowMs();
    const long rc = ptIoctl(chan, FAST_INIT, c.msgLen < 0 ? nullptr : &req, &resp);
    const double took = nowMs() - start;
    printf("rc=0x%02lX after %6.1f ms  (TINIL=%lu TWUP=%lu P4_MIN=%lu P2_MAX=%lu", rc, took, tinil, twup,
           getCfg(chan, 0x0C), getCfg(chan, 0x09));
    if (c.p2max) printf(", setting P2_MAX returned 0x%02lX", p2rc);
    printf(")\n");
    if (rc == STATUS_NOERROR) {
        printf("      response: ");
        printMsg(resp);
    }
    if (c.protocol != CAN) drain(chan, 250, "      ");
    ptDisconnect(chan);
}

static int cmdFastDiag() {
    const FastCase cases[] = {
        {"ISO14230 @10400  81 10 F1 81  (defaults)", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false},
        {"same, TWUP=300 ms", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 300, false},
        {"same, TINIL=100 TWUP=150 ms", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 100, 150, false},
        {"same, LOOPBACK on (shows what was sent)", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, true},
        {"same, P4_MIN=40 (20 ms between sent bytes)", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false, 40, 0},
        {"same, 8-byte request", ISO14230, 10400, 8, {0x85, 0x10, 0xF1, 0x81, 0, 0, 0, 0}, 0, 0, false},
        {"same, P2_MAX=800 (wait 400 ms for the ECU)", ISO14230, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false, 0, 800},
        {"ISO14230 @10400  81 01 F1 81", ISO14230, 10400, 4, {0x81, 0x01, 0xF1, 0x81}, 0, 0, false},
        {"ISO14230 @10400  C1 33 F1 81  (functional)", ISO14230, 10400, 4, {0xC1, 0x33, 0xF1, 0x81}, 0, 0, false},
        {"ISO14230 @9600   81 10 F1 81", ISO14230, 9600, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false},
        {"ISO14230 @9600   81 01 F1 81", ISO14230, 9600, 4, {0x81, 0x01, 0xF1, 0x81}, 0, 0, false},
        {"ISO14230 @10400  no message (aty 0 0)", ISO14230, 10400, -1, {}, 0, 0, false},
        {"ISO9141  @10400  81 10 F1 81", ISO9141, 10400, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false},
        {"CAN channel (the firmware must reject it)", CAN, 500000, 4, {0x81, 0x10, 0xF1, 0x81}, 0, 0, false},
    };
    for (const auto &c : cases) runFastCase(c);

    // Reference timings: a command the firmware rejects, and one it accepts.
    unsigned long chan = 0;
    if (ptConnect(g_dev, ISO14230, K_LINE_ONLY, 10400, &chan) == STATUS_NOERROR) {
        double start = nowMs();
        long rc = setCfg(chan, 0x7777, 1);
        printf("%-44s rc=0x%02lX after %6.1f ms\n", "reference: SET_CONFIG of an unknown parameter", rc, nowMs() - start);
        start = nowMs();
        rc = setCfg(chan, 0x03, 0);
        printf("%-44s rc=0x%02lX after %6.1f ms\n", "reference: SET_CONFIG LOOPBACK=0", rc, nowMs() - start);
        ptDisconnect(chan);
    }

    // Independent witness: a raw ISO9141 channel listening on the same K line
    // while the ISO14230 channel runs the fast init.
    unsigned long sniff = 0, kwp = 0;
    printf("\nK line watched by a second (raw ISO9141) channel during the fast init:\n");
    if (ptConnect(g_dev, ISO9141, ISO9141_NO_CHECKSUM | K_LINE_ONLY, 10400, &sniff) != STATUS_NOERROR) {
        check(1, "  connect raw ISO9141");
        return 0;
    }
    passAllFilter(sniff, ISO9141, 1);
    if (ptConnect(g_dev, ISO14230, K_LINE_ONLY, 10400, &kwp) != STATUS_NOERROR) {
        check(1, "  connect ISO14230 alongside ISO9141");
    } else {
        static PASSTHRU_MSG req, resp;
        memset(&req, 0, sizeof(req));
        req.ProtocolID = ISO14230;
        req.DataSize = 4;
        memcpy(req.Data, "\x81\x10\xF1\x81", 4);
        const long rc = ptIoctl(kwp, FAST_INIT, &req, &resp);
        printf("  FAST_INIT rc=0x%02lX\n", rc);
        drain(sniff, 400, "  ");
        ptDisconnect(kwp);
    }
    ptDisconnect(sniff);
    return 0;
}

// ---------------------------------------------------------------------------
// kwp: repeated KWP2000 fast inits in raw mode (the application adds the
// checksum and sees every byte), with a chosen idle time before each attempt.
// After a positive StartCommunication response it reads the ECU identification.

static unsigned char kwpSum(const unsigned char *data, int len) {
    unsigned char sum = 0;
    for (int i = 0; i < len; i++) sum = static_cast<unsigned char>(sum + data[i]);
    return sum;
}

// Collects raw bytes received within the time window; returns the count.
static int kwpCollect(unsigned long chan, unsigned long ms, unsigned char *out, int cap) {
    int count = 0;
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        static PASSTHRU_MSG m;
        unsigned long n = 1;
        ptReadMsgs(chan, &m, &n, 20);
        if (!n || (m.RxStatus & (START_OF_MESSAGE | TX_MSG_TYPE))) continue;
        for (unsigned long i = 0; i < m.DataSize && count < cap; i++) out[count++] = m.Data[i];
    }
    return count;
}

static void printBytes(const char *label, const unsigned char *data, int len) {
    printf("%s", label);
    for (int i = 0; i < len; i++) printf("%02X ", data[i]);
    if (!len) printf("(nothing)");
    printf("\n");
}

static int kwpRequest(unsigned long chan, unsigned char address, const unsigned char *service, int serviceLen,
                      unsigned char *reply, int cap) {
    static PASSTHRU_MSG m;
    memset(&m, 0, 24);
    m.ProtocolID = ISO14230;
    m.Data[0] = static_cast<unsigned char>(0x80 | serviceLen);
    m.Data[1] = address;
    m.Data[2] = 0xF1;
    memcpy(m.Data + 3, service, static_cast<size_t>(serviceLen));
    m.Data[3 + serviceLen] = kwpSum(m.Data, 3 + serviceLen);
    m.DataSize = static_cast<unsigned long>(4 + serviceLen);
    printBytes("      > ", m.Data, static_cast<int>(m.DataSize));
    unsigned long n = 1;
    if (ptWriteMsgs(chan, &m, &n, 200) != STATUS_NOERROR) {
        check(1, "      write");
        return 0;
    }
    const int got = kwpCollect(chan, 400, reply, cap);
    printBytes("      < ", reply, got);
    return got;
}

static int cmdKwp(int argc, char **argv) {
    const unsigned long baud = argc > 0 ? strtoul(argv[0], nullptr, 10) : 10400;
    const unsigned char address = argc > 1 ? static_cast<unsigned char>(strtoul(argv[1], nullptr, 16)) : 0x01;
    const unsigned long idle = argc > 2 ? strtoul(argv[2], nullptr, 10) : 1000;
    const int count = argc > 3 ? atoi(argv[3]) : 5;
    // "raw": the application supplies the checksum; "auto": the device adds and checks it
    const bool raw = !(argc > 4 && !strcmp(argv[4], "auto"));
    const unsigned long p4min = argc > 5 ? strtoul(argv[5], nullptr, 10) : 0;
    // Optional request bytes in hex (without checksum) instead of "81 <addr> F1 81".
    unsigned char custom[16];
    int customLen = 0;
    if (argc > 6)
        for (const char *p = argv[6]; p[0] && p[1] && customLen < 15; p += 2) {
            char hex[3] = {p[0], p[1], 0};
            custom[customLen++] = static_cast<unsigned char>(strtoul(hex, nullptr, 16));
        }
    int positive = 0;
    printf("KWP2000 fast init, %lu baud, ECU address %02X, %lu ms idle before each attempt, checksum by %s",
           baud, address, idle, raw ? "application" : "device");
    if (p4min) printf(", P4_MIN=%lu", p4min);
    printf("\n");
    for (int attempt = 1; attempt <= count; attempt++) {
        unsigned long chan = 0;
        if (!check(ptConnect(g_dev, ISO14230, (raw ? ISO9141_NO_CHECKSUM : 0) | K_LINE_ONLY, baud, &chan),
                   "PassThruConnect"))
            return 1;
        passAllFilter(chan, ISO14230, 1);
        setCfg(chan, 0x03, 0);
        setCfg(chan, P1_MAX, 80);  // accept up to 40 ms between the ECU's bytes within one reply
        if (p4min) setCfg(chan, 0x0C, p4min);
        Sleep(idle);

        static PASSTHRU_MSG req, resp;
        memset(&req, 0, 24);
        memset(&resp, 0, sizeof(resp));
        req.ProtocolID = ISO14230;
        const unsigned char start[4] = {0x81, address, 0xF1, 0x81};
        const int reqLen = customLen ? customLen : 4;
        memcpy(req.Data, customLen ? custom : start, static_cast<size_t>(reqLen));
        req.Data[reqLen] = kwpSum(req.Data, reqLen);
        req.DataSize = static_cast<unsigned long>(raw ? reqLen + 1 : reqLen);
        if (attempt == 1) printBytes("  request: ", req.Data, static_cast<int>(req.DataSize));
        const double t0 = nowMs();
        const long rc = ptIoctl(chan, FAST_INIT, &req, &resp);
        printf("  #%d  rc=0x%02lX after %6.1f ms\n", attempt, rc, nowMs() - t0);
        if (rc == STATUS_NOERROR) printBytes("      FAST_INIT returned: ", resp.Data, static_cast<int>(resp.DataSize));
        unsigned char line[128];
        const int got = kwpCollect(chan, 200, line, sizeof(line));
        printBytes("      on the line:        ", line, got);

        // Positive StartCommunication response: 8x F1 <addr> C1 <key bytes> <checksum>
        bool started = false;
        for (int i = 0; i + 3 < got; i++)
            if ((line[i] & 0xC0) == 0x80 && line[i + 1] == 0xF1 && line[i + 2] == address && line[i + 3] == 0xC1) started = true;
        if (rc == STATUS_NOERROR && resp.DataSize >= 4 && resp.Data[1] == 0xF1 && resp.Data[3] == 0xC1) started = true;
        if (started) {
            positive++;
            unsigned char reply[128];
            const unsigned char ident[2] = {0x1A, 0x9B};   // readECUIdentification
            const unsigned char present[1] = {0x3E};       // testerPresent
            const unsigned char stop[1] = {0x82};          // stopCommunication
            int n = kwpRequest(chan, address, ident, 2, reply, sizeof(reply));
            if (n > 6) {
                printf("        text: \"");
                for (int i = 0; i < n; i++) putchar(reply[i] >= 0x20 && reply[i] < 0x7F ? reply[i] : '.');
                printf("\"\n");
            }
            kwpRequest(chan, address, present, 1, reply, sizeof(reply));
            kwpRequest(chan, address, stop, 1, reply, sizeof(reply));
        }
        ptDisconnect(chan);
    }
    printf("positive StartCommunication responses: %d of %d\n", positive, count);
    return 0;
}

// ---------------------------------------------------------------------------
// edc15: the Bosch EDC15 KWP2000 entry. This ECU answers the bare wake-up pulse
// by itself (83 00 01 7F 00 13 16), so the pulse must go out WITHOUT a request
// attached - a request sent right behind the pulse collides with that frame.
//   method "aty":  FAST_INIT with no message (firmware makes the pulse)
//   method "baud": a 00 byte at 360 baud is a 25 ms low pulse, then back to 10400
//   method "pin":  short pin 7 (K) to ground for 25 ms via the programming-voltage switch

typedef long(__stdcall *FnSetVoltage)(unsigned long, unsigned long, unsigned long);
static FnSetVoltage ptSetVoltage;

static int cmdEdc15(int argc, char **argv) {
    const char *method = argc > 0 ? argv[0] : "aty";
    const int count = argc > 1 ? atoi(argv[1]) : 3;
    const unsigned long pause = argc > 2 ? strtoul(argv[2], nullptr, 10) : 6000;
    const unsigned long p4min = argc > 3 ? strtoul(argv[3], nullptr, 10) : 0;
    const unsigned char address = 0x01;
    int woke = 0, session = 0;
    for (int attempt = 1; attempt <= count; attempt++) {
        if (attempt > 1) Sleep(pause);
        unsigned long chan = 0;
        if (!check(ptConnect(g_dev, ISO14230, ISO9141_NO_CHECKSUM | K_LINE_ONLY, 10400, &chan), "PassThruConnect"))
            return 1;
        passAllFilter(chan, ISO14230, 1);
        setCfg(chan, 0x03, 0);      // LOOPBACK off
        setCfg(chan, P1_MAX, 40);   // 20 ms of silence ends a received frame
        setCfg(chan, 0x0A, 0);      // P3_MIN: we pace the requests ourselves
        if (p4min) setCfg(chan, 0x0C, p4min);

        setCfg(chan, 0x0C, p4min ? p4min : 20);  // 10 ms between request bytes, as the reference tool
        static PASSTHRU_MSG start;
        memset(&start, 0, 24);
        start.ProtocolID = ISO14230;
        memcpy(start.Data, "\x81\x01\xF1\x81\xF4", 5);
        start.DataSize = 5;

        // Like the reference tool: burst (pulse + StartCommunication), wait for
        // an answer, and repeat a few times 100 ms apart until the ECU answers.
        unsigned char buf[128];
        int got = 0;
        bool wake = false;
        for (int burst = 1; burst <= 6 && !wake; burst++) {
            const double t0 = nowMs();
            long rc = 0;
            unsigned long n = 1;
            if (!strcmp(method, "aty")) {
                static PASSTHRU_MSG resp;
                memset(&resp, 0, sizeof(resp));
                rc = ptIoctl(chan, FAST_INIT, &start, &resp);
                got = 0;
                if (rc == STATUS_NOERROR)
                    for (unsigned long i = 0; i < resp.DataSize && got < 64; i++) buf[got++] = resp.Data[i];
                got += kwpCollect(chan, 300, buf + got, static_cast<int>(sizeof(buf)) - got);
            } else {
                if (!strcmp(method, "baud")) {
                    static PASSTHRU_MSG brk;
                    memset(&brk, 0, 24);
                    brk.ProtocolID = ISO14230;
                    brk.DataSize = 1;  // 00 at 360 baud: start bit + 8 zero bits = 25 ms low
                    setCfg(chan, 0x01, 360);
                    rc = ptWriteMsgs(chan, &brk, &n, 200);
                    setCfg(chan, 0x01, 10400);
                } else {
                    rc = ptSetVoltage(g_dev, 7, SHORT_TO_GROUND);
                    Sleep(25);
                    ptSetVoltage(g_dev, 7, VOLTAGE_OFF);
                }
                const double pulseEnd = nowMs();
                while (nowMs() - pulseEnd < 24.0) Sleep(1);  // line high for 25 ms
                n = 1;
                ptWriteMsgs(chan, &start, &n, 300);
                got = kwpCollect(chan, 400, buf, sizeof(buf));
            }
            printf("#%d burst %d (%s) rc=0x%02lX, %.0f ms: ", attempt, burst, method, rc, nowMs() - t0);
            printBytes("", buf, got);
            for (int i = 0; i + 3 < got; i++)
                if (buf[i] == 0x83 && buf[i + 2] == 0x01 && (buf[i + 3] == 0x7F || buf[i + 3] == 0xC1)) wake = true;
            if (!wake) Sleep(100);
        }
        if (wake) {
            woke++;
            const unsigned char diag[2] = {0x10, 0x85};  // startDiagnosticSession
            const unsigned char stop[1] = {0x82};        // stopCommunication
            got = kwpRequest(chan, address, diag, 2, buf, sizeof(buf));
            for (int i = 0; i + 1 < got; i++)
                if (buf[i] == 0x50 && buf[i + 1] == 0x85) {
                    session++;
                    break;
                }
            Sleep(75);
            kwpRequest(chan, address, stop, 1, buf, sizeof(buf));
        }
        ptDisconnect(chan);
    }
    printf("wake-up frame seen: %d of %d, diagnostic session (50 85): %d of %d\n", woke, count, session, count);
    return 0;
}

// ---------------------------------------------------------------------------
// kw1281: a KW1281 session done by the application on a raw ISO9141 channel.
// After the 5-baud init the ECU sends blocks one byte at a time and expects the
// complement of every byte (except a block's last) before it continues.
//   block = <length> <counter> <title> <data...> 03

static unsigned long g_kwChan = 0;
static unsigned char g_kwPending[64];
static int g_kwPendingCount = 0, g_kwPendingPos = 0;

static bool kwReadByte(unsigned char *out, unsigned long timeoutMs) {
    const double until = nowMs() + timeoutMs;
    for (;;) {
        if (g_kwPendingPos < g_kwPendingCount) {
            *out = g_kwPending[g_kwPendingPos++];
            return true;
        }
        if (nowMs() >= until) return false;
        static PASSTHRU_MSG m;
        unsigned long n = 1;
        ptReadMsgs(g_kwChan, &m, &n, 20);
        if (!n || (m.RxStatus & (START_OF_MESSAGE | TX_MSG_TYPE)) || !m.DataSize) continue;
        g_kwPendingCount = m.DataSize > sizeof(g_kwPending) ? static_cast<int>(sizeof(g_kwPending))
                                                             : static_cast<int>(m.DataSize);
        memcpy(g_kwPending, m.Data, static_cast<size_t>(g_kwPendingCount));
        g_kwPendingPos = 0;
    }
}

static bool kwWriteByte(unsigned char b) {
    static PASSTHRU_MSG m;
    memset(&m, 0, 24);
    m.ProtocolID = ISO9141;
    m.DataSize = 1;
    m.Data[0] = b;
    unsigned long n = 1;
    return ptWriteMsgs(g_kwChan, &m, &n, 100) == STATUS_NOERROR;
}

// Receives one block; false if the ECU stopped talking.
static bool kwReceiveBlock(unsigned char *block, int *len, unsigned long firstTimeoutMs) {
    unsigned char b;
    if (!kwReadByte(&b, firstTimeoutMs)) return false;
    const int total = b + 1;  // the length byte counts everything after itself
    block[0] = b;
    if (!kwWriteByte(static_cast<unsigned char>(~b))) return false;
    for (int i = 1; i < total; i++) {
        if (!kwReadByte(&b, 300)) {
            printf("  (ECU stopped after %d of %d bytes)\n", i, total);
            return false;
        }
        block[i] = b;
        if (i < total - 1 && !kwWriteByte(static_cast<unsigned char>(~b))) return false;
    }
    *len = total;
    return true;
}

// Sends one block; the ECU answers every byte but the last with its complement.
static bool kwSendBlock(const unsigned char *block, int len) {
    for (int i = 0; i < len; i++) {
        if (!kwWriteByte(block[i])) return false;
        if (i == len - 1) break;
        unsigned char echo;
        if (!kwReadByte(&echo, 300)) {
            printf("  (no complement from the ECU for byte %d)\n", i);
            return false;
        }
        if (echo != static_cast<unsigned char>(~block[i]))
            printf("  (complement mismatch: sent %02X, got %02X)\n", block[i], echo);
    }
    return true;
}

static int cmdKw1281(int argc, char **argv) {
    const unsigned long baud = argc > 0 ? strtoul(argv[0], nullptr, 10) : 9600;
    unsigned char address = argc > 1 ? static_cast<unsigned char>(strtoul(argv[1], nullptr, 16)) : 0x01;
    if (!check(ptConnect(g_dev, ISO9141, ISO9141_NO_CHECKSUM | K_LINE_ONLY, baud, &g_kwChan), "PassThruConnect"))
        return 1;
    passAllFilter(g_kwChan, ISO9141, 1);
    // No echo, no enforced gaps, and a received byte is handed over after 1 ms of silence.
    SCONFIG cfg[] = {{0x03, 0}, {P1_MAX, 2}, {0x0A, 0}, {0x0C, 0}, {PARITY, 0}};
    SCONFIG_LIST list = {sizeof(cfg) / sizeof(cfg[0]), cfg};
    check(ptIoctl(g_kwChan, SET_CONFIG, &list, nullptr), "SET_CONFIG");

    unsigned char keys[8] = {};
    SBYTE_ARRAY in = {1, &address}, out = {0, keys};
    int rc = 1;
    if (check(ptIoctl(g_kwChan, FIVE_BAUD_INIT, &in, &out), "FIVE_BAUD_INIT")) {
        printf("Key bytes: %02X %02X%s\n", keys[0], keys[1], keys[0] == 0x01 && keys[1] == 0x8A ? "  (KW1281)" : "");
        unsigned char counter = 0;
        int blocks = 0;
        for (; blocks < 16; blocks++) {
            unsigned char block[260];
            int len = 0;
            const double start = nowMs();
            if (!kwReceiveBlock(block, &len, 1500)) break;
            counter = block[1];
            printf("ECU block #%u title %02X, %d bytes in %.0f ms: ", block[1], block[2], len, nowMs() - start);
            for (int i = 0; i < len; i++) printf("%02X ", block[i]);
            if (block[2] == 0xF6) {
                printf(" \"");
                for (int i = 3; i < len - 1; i++) putchar(block[i] >= 0x20 && block[i] < 0x7F ? block[i] : '.');
                printf("\"");
            }
            printf("\n");
            if (block[2] == 0x09 && blocks > 0) {  // the ECU acknowledges: identification is complete
                rc = 0;
                break;
            }
            const unsigned char ack[4] = {0x03, static_cast<unsigned char>(counter + 1), 0x09, 0x03};
            if (!kwSendBlock(ack, 4)) break;
        }
        if (rc == 0) {
            // End the session politely ("end output").
            const unsigned char end[4] = {0x03, static_cast<unsigned char>(counter + 1), 0x06, 0x03};
            kwSendBlock(end, 4);
            printf("KW1281 exchange complete (%d blocks).\n", blocks + 1);
        } else {
            printf("KW1281 exchange did not complete (%d blocks received).\n", blocks);
        }
    }
    ptDisconnect(g_kwChan);
    return rc;
}

static int cmdCan(int argc, char **argv) {
    unsigned long baud = argc > 0 ? strtoul(argv[0], nullptr, 10) : 500000;
    unsigned long chan = 0;
    if (!check(ptConnect(g_dev, CAN, 0, baud, &chan), "PassThruConnect")) return 1;
    if (!passAllFilter(chan, CAN, 4)) return 1;
    printf("CAN sniff, %lu baud\n", baud);
    readLoop(chan, false);
    check(ptDisconnect(chan), "PassThruDisconnect");
    return 0;
}

int main(int argc, char **argv) {
    std::string dllPath;
    int argi = 1;
    while (argi + 1 < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--dll")) dllPath = argv[argi + 1];
        else if (!strcmp(argv[argi], "--secs")) g_seconds = strtoul(argv[argi + 1], nullptr, 10);
        else break;
        argi += 2;
    }
    if (argi >= argc) {
        usage();
        return 1;
    }
    if (dllPath.empty()) {
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        dllPath = self;
        dllPath.erase(dllPath.find_last_of("\\/") + 1);
        dllPath += sizeof(void *) == 8 ? "op20winusb64.dll" : "op20winusb32.dll";
    }

    HMODULE dll = LoadLibraryA(dllPath.c_str());
    if (!dll) {
        printf("cannot load %s (error %lu)\n", dllPath.c_str(), GetLastError());
        return 1;
    }
#define RESOLVE(var, type, name)                                  \
    var = reinterpret_cast<type>(GetProcAddress(dll, name));     \
    if (!var) {                                                   \
        printf("%s is not exported by the DLL\n", name);          \
        return 1;                                                 \
    }
    RESOLVE(ptOpen, FnOpen, "PassThruOpen")
    RESOLVE(ptClose, FnClose, "PassThruClose")
    RESOLVE(ptConnect, FnConnect, "PassThruConnect")
    RESOLVE(ptDisconnect, FnDisconnect, "PassThruDisconnect")
    RESOLVE(ptReadMsgs, FnReadMsgs, "PassThruReadMsgs")
    RESOLVE(ptWriteMsgs, FnWriteMsgs, "PassThruWriteMsgs")
    RESOLVE(ptStartFilter, FnStartFilter, "PassThruStartMsgFilter")
    RESOLVE(ptReadVersion, FnReadVersion, "PassThruReadVersion")
    RESOLVE(ptGetLastError, FnGetLastError, "PassThruGetLastError")
    RESOLVE(ptIoctl, FnIoctl, "PassThruIoctl")

    if (!check(ptOpen(nullptr, &g_dev), "PassThruOpen")) return 1;

    const char *cmd = argv[argi++];
    int rc = 1;
    if (!strcmp(cmd, "info")) rc = cmdInfo();
    else if (!strcmp(cmd, "check")) rc = cmdCheck();
    else if (!strcmp(cmd, "sniff")) rc = cmdSniff(argc - argi, argv + argi);
    else if (!strcmp(cmd, "5baud")) rc = cmdFiveBaud(argc - argi, argv + argi);
    else if (!strcmp(cmd, "obd")) rc = cmdObd(argc - argi, argv + argi);
    else if (!strcmp(cmd, "fastinit")) rc = cmdFastInit(argc - argi, argv + argi);
    else if (!strcmp(cmd, "fastdiag")) rc = cmdFastDiag();
    else if (!strcmp(cmd, "kw1281")) rc = cmdKw1281(argc - argi, argv + argi);
    else if (!strcmp(cmd, "kwp")) rc = cmdKwp(argc - argi, argv + argi);
    else if (!strcmp(cmd, "edc15")) {
        ptSetVoltage = reinterpret_cast<FnSetVoltage>(GetProcAddress(dll, "PassThruSetProgrammingVoltage"));
        rc = cmdEdc15(argc - argi, argv + argi);
    }
    else if (!strcmp(cmd, "can")) rc = cmdCan(argc - argi, argv + argi);
    else usage();

    check(ptClose(g_dev), "PassThruClose");
    return rc;
}
