// Self-test of the protocol engine against a simulated Openport 2.0.
//
// The simulator speaks the wire protocol as documented in PROTOCOL.md and
// hands its output back in small, awkwardly sized pieces so that the stream
// parser is exercised on fragmented input. It checks this DLL's encoding and
// decoding; it cannot prove the real firmware behaves the same way.

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../src/device.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                               \
        }                                                               \
    } while (0)

class SimDevice : public Transport {
public:
    std::vector<std::string> commands;  // command lines received, for assertions
    bool ecuPresent = true;

    int read(uint8_t *buf, size_t cap) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return cancelled_ || !out_.empty(); });
        if (cancelled_) return ReadCancelled;
        // 1, 2, 3, 5, 7 ... byte pieces
        static const size_t sizes[] = {1, 2, 3, 5, 7, 11, 64};
        size_t n = sizes[step_++ % (sizeof(sizes) / sizeof(sizes[0]))];
        if (n > cap) n = cap;
        if (n > out_.size()) n = out_.size();
        for (size_t i = 0; i < n; i++) {
            buf[i] = out_.front();
            out_.pop_front();
        }
        return static_cast<int>(n);
    }

    bool write(const uint8_t *data, size_t len, DWORD) override {
        std::lock_guard<std::mutex> lock(mutex_);
        in_.insert(in_.end(), data, data + len);
        process();
        cv_.notify_all();
        return true;
    }

    void cancelRead() override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        cv_.notify_all();
    }
    size_t readChunk() const override { return 64; }
    const std::string &description() const override { return desc_; }

private:
    void emit(const std::string &s) { out_.insert(out_.end(), s.begin(), s.end()); }
    void emitBytes(const std::vector<uint8_t> &b) { out_.insert(out_.end(), b.begin(), b.end()); }

    void packet(int wire, uint8_t type, const std::vector<uint8_t> &payload) {
        out_.push_back('a');
        out_.push_back('r');
        out_.push_back(static_cast<uint8_t>('0' + wire));
        out_.push_back(static_cast<uint8_t>(payload.size() + 1));
        out_.push_back(type);
        emitBytes(payload);
    }
    static std::vector<uint8_t> ts(uint32_t t) {
        return {uint8_t(t >> 24), uint8_t(t >> 16), uint8_t(t >> 8), uint8_t(t)};
    }

    // A K-line message: start indication, data in two packets, end indication.
    void kMessage(int wire, bool loopback, const std::vector<uint8_t> &bytes, uint32_t t) {
        const uint8_t lb = loopback ? 0x20 : 0x00;
        packet(wire, 0x80 | lb, ts(t));
        size_t half = bytes.size() / 2;
        if (half) packet(wire, lb, std::vector<uint8_t>(bytes.begin(), bytes.begin() + half));
        packet(wire, lb, std::vector<uint8_t>(bytes.begin() + half, bytes.end()));
        packet(wire, 0x40 | lb, ts(t + 1000));
    }

    void process() {
        for (;;) {
            if (need_ > 0) {  // raw bytes that follow a command line
                if (in_.size() < need_) return;
                std::vector<uint8_t> payload(in_.begin(), in_.begin() + need_);
                in_.erase(in_.begin(), in_.begin() + need_);
                need_ = 0;
                execute(pending_, payload);
                continue;
            }
            auto nl = std::find(in_.begin(), in_.end(), '\n');
            if (nl == in_.end()) return;
            std::string line(in_.begin(), nl);
            in_.erase(in_.begin(), nl + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            commands.push_back(line);

            std::vector<long> a = args(line);
            const char c = line.size() > 2 ? line[2] : 0;
            size_t payload = 0;
            if (c == 't' || c == 'y') payload = a.size() > 1 ? a[1] : 0;       // at?<ch> <len> ...
            else if (c == 'f') payload = a.size() > 3 ? a[3] * (a[1] == 3 ? 3 : 2) : 0;
            else if (c == 'm') payload = a.size() > 4 ? a[4] : 0;
            else if (c == 'p') payload = a.size() > 1 ? a[1] : 0;
            if (payload) {
                need_ = payload;
                pending_ = line;
            } else {
                execute(line, {});
            }
        }
    }

    static std::vector<long> args(const std::string &line) {
        std::vector<long> out;
        const char *p = line.c_str() + 3;
        for (;;) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            out.push_back(v);
            p = end;
        }
        return out;
    }

    void execute(const std::string &line, const std::vector<uint8_t> &payload) {
        const char c = line[2];
        std::vector<long> a = args(line);
        auto seqAt = [&](size_t i) { return a.size() > i ? " " + std::to_string(a[i]) : std::string(); };
        char buf[96];
        switch (c) {
            case 'i': emit("ari main code version : 1174877\r\n"); break;
            case 'a':
            case 'z': emit("aro" + seqAt(0) + "\r\n"); break;
            case 'o': emit("aro" + seqAt(4) + "\r\n"); break;
            case 'c':
            case 'l': emit("aro" + seqAt(1) + "\r\n"); break;
            case 's': emit("aro" + seqAt(3) + "\r\n"); break;
            case 'k':
            case 'n':
            case 'v': emit("aro" + seqAt(2) + "\r\n"); break;
            case 'g':
                snprintf(buf, sizeof(buf), "arg%ld %ld 10400%s\r\n", a[0], a[1], seqAt(2).c_str());
                emit(buf);
                break;
            case 'r':
                emit("arb some debug text\r\n");  // must be ignored
                snprintf(buf, sizeof(buf), "arr %ld 12345%s\r\n", a[0], seqAt(1).c_str());
                emit(buf);
                break;
            case 'f':
                snprintf(buf, sizeof(buf), "arf%ld 7%s\r\n", a[0], seqAt(4).c_str());
                emit(buf);
                break;
            case 'm':
                snprintf(buf, sizeof(buf), "arm%ld 3%s\r\n", a[0], seqAt(5).c_str());
                emit(buf);
                break;
            case 'w':
                if (!ecuPresent) {
                    emit("are 7" + seqAt(2) + "\r\n");
                } else {
                    snprintf(buf, sizeof(buf), "arw%ld 8 8%s\r\n", a[0], seqAt(2).c_str());
                    emit(buf);
                }
                break;
            case 'y': {
                const std::vector<uint8_t> resp = {0x83, 0xF1, 0x10, 0xC1, 0xEF, 0x8F, 0xC3};
                snprintf(buf, sizeof(buf), "ary%ld %zu%s\r\n", a[0], resp.size(), seqAt(3).c_str());
                emit(buf);
                emitBytes(resp);
                break;
            }
            case 'p': {
                const std::string serial = "TAabc123";
                snprintf(buf, sizeof(buf), "arp %ld %zu%s\r\n", a[0], serial.size(), seqAt(2).c_str());
                emit(buf);
                emit(serial);
                break;
            }
            case 't': {
                const int wire = static_cast<int>(a[0]);
                emit("aro" + seqAt(4) + "\r\n");  // att<ch> <len> <flags> <timeout us> [<seq>]
                if (wire == 3 || wire == 4) {
                    kMessage(wire, true, payload, 100000);  // echo of the request
                    std::vector<uint8_t> answer = payload;  // "ECU" answers with the bytes inverted
                    for (auto &b : answer) b = static_cast<uint8_t>(~b);
                    kMessage(wire, false, answer, 150000);
                } else if (wire == 5) {
                    std::vector<uint8_t> p = ts(200000);
                    p.insert(p.end(), payload.begin(), payload.end());
                    packet(5, 0x20, p);  // loopback
                    packet(5, 0x00, p);  // "received" frame
                } else if (wire == 6) {
                    std::vector<uint8_t> id(payload.begin(), payload.begin() + 4);
                    std::vector<uint8_t> p = ts(300000);
                    p.insert(p.end(), id.begin(), id.end());
                    packet(6, 0x10, p);  // tx done
                    // Segmented response: first-frame indication, two pieces, end.
                    packet(6, 0x80, p);
                    std::vector<uint8_t> seg = p;
                    seg.insert(seg.end(), {1, 2, 3, 4, 5, 6});
                    packet(6, 0x00, seg);
                    std::vector<uint8_t> last = p;
                    last.insert(last.end(), {7, 8, 9});
                    packet(6, 0x40, last);
                    // Single-frame response.
                    std::vector<uint8_t> single = p;
                    single.insert(single.end(), {0x50, 0x03});
                    packet(6, 0x40, single);
                }
                break;
            }
            default: emit("are 1\r\n"); break;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint8_t> out_;
    std::vector<uint8_t> in_;
    size_t need_ = 0;
    std::string pending_;
    size_t step_ = 0;
    bool cancelled_ = false;
    std::string desc_ = "simulator";
};

static bool hasCommand(SimDevice *sim, const char *prefix) {
    for (auto &c : sim->commands)
        if (c.compare(0, strlen(prefix), prefix) == 0) return true;
    return false;
}

int main() {
    SimDevice *sim = new SimDevice();
    Device dev;
    CHECK(dev.attach(std::unique_ptr<Transport>(sim)) == STATUS_NOERROR);
    CHECK(dev.firmwareVersion() == "1.17.4877");
    CHECK(hasCommand(sim, "ata 1"));

    unsigned long mv = 0;
    CHECK(dev.readPinVoltage(16, &mv) == STATUS_NOERROR);
    CHECK(mv == 12345);

    // ---- ISO9141 (K-line) ----
    unsigned long k = 0;
    CHECK(dev.connect(ISO9141, 0x200, 10400, &k) == STATUS_NOERROR);
    CHECK(k == ISO9141);
    CHECK(hasCommand(sim, "ato3 512 10400 3 "));
    unsigned long other = 0;
    CHECK(dev.connect(ISO9141_CH1, 0, 10400, &other) == ERR_CHANNEL_IN_USE);
    CHECK(dev.connect(0x1234, 0, 10400, &other) == ERR_INVALID_PROTOCOL_ID);

    SCONFIG cfg = {0x16, 2};
    CHECK(dev.setConfig(k, &cfg) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "ats3 22 2 "));
    cfg = {0x01, 0};
    CHECK(dev.getConfig(k, &cfg) == STATUS_NOERROR);
    CHECK(cfg.Value == 10400);

    static PASSTHRU_MSG mask, pattern, msg, rx[8];
    mask.ProtocolID = pattern.ProtocolID = ISO9141;
    mask.DataSize = pattern.DataSize = 1;
    unsigned long filter = 0;
    CHECK(dev.startFilter(k, PASS_FILTER, &mask, &pattern, nullptr, &filter) == STATUS_NOERROR);
    CHECK(filter == 7);

    unsigned char address = 0x33, keys[2] = {0, 0};
    SBYTE_ARRAY in = {1, &address}, out = {0, keys};
    CHECK(dev.fiveBaudInit(k, &in, &out) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atw3 51 "));
    CHECK(out.NumOfBytes == 2 && keys[0] == 8 && keys[1] == 8);
    sim->ecuPresent = false;
    CHECK(dev.fiveBaudInit(k, &in, &out) == ERR_FAILED);
    sim->ecuPresent = true;

    msg.ProtocolID = ISO9141;
    msg.DataSize = 5;
    const unsigned char request[5] = {0x68, 0x6A, 0xF1, 0x01, 0x00};
    memcpy(msg.Data, request, 5);
    unsigned long n = 1;
    CHECK(dev.writeMsgs(k, &msg, &n, 500) == STATUS_NOERROR);  // blocking write
    CHECK(n == 1);
    CHECK(hasCommand(sim, "att3 5 0 "));

    // Expect: start(loopback), loopback msg, start, response.
    n = 4;
    CHECK(dev.readMsgs(k, rx, &n, 1000) == STATUS_NOERROR);
    CHECK(n == 4);
    CHECK(rx[0].RxStatus == START_OF_MESSAGE && rx[0].DataSize == 0 && rx[0].Timestamp == 100000);
    CHECK(rx[1].RxStatus == TX_MSG_TYPE && rx[1].DataSize == 5 && memcmp(rx[1].Data, request, 5) == 0);
    CHECK(rx[1].Timestamp == 101000 && rx[1].ExtraDataIndex == 5 && rx[1].ProtocolID == ISO9141);
    CHECK(rx[2].RxStatus == START_OF_MESSAGE && rx[2].Timestamp == 150000);
    CHECK(rx[3].RxStatus == 0 && rx[3].DataSize == 5 && rx[3].Data[0] == 0x97 && rx[3].Data[4] == 0xFF);
    n = 1;
    CHECK(dev.readMsgs(k, rx, &n, 0) == ERR_BUFFER_EMPTY);
    CHECK(n == 0);
    n = 1;
    CHECK(dev.readMsgs(k, rx, &n, 50) == ERR_BUFFER_EMPTY);

    // Non-blocking write: no sequence number on the wire.
    n = 1;
    CHECK(dev.writeMsgs(k, &msg, &n, 0) == STATUS_NOERROR);
    CHECK(sim->commands.back() == "att3 5 0 1000000");
    n = 8;
    CHECK(dev.readMsgs(k, rx, &n, 300) == ERR_TIMEOUT);
    CHECK(n == 4);

    unsigned long periodic = 0;
    CHECK(dev.startPeriodic(k, &msg, &periodic, 100) == STATUS_NOERROR);
    CHECK(periodic == 3);
    CHECK(hasCommand(sim, "atm3 100000 0 0 5 "));
    CHECK(dev.startPeriodic(k, &msg, &periodic, 2) == ERR_INVALID_TIME_INTERVAL);
    CHECK(dev.stopPeriodic(k, periodic) == STATUS_NOERROR);
    CHECK(dev.clearPeriodic(k) == STATUS_NOERROR);
    CHECK(dev.clearFilters(k) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atk3 -1 "));

    msg.ProtocolID = CAN;
    n = 1;
    CHECK(dev.writeMsgs(k, &msg, &n, 0) == ERR_MSG_PROTOCOL_ID);
    CHECK(dev.disconnect(k) == STATUS_NOERROR);
    CHECK(dev.disconnect(k) == ERR_INVALID_CHANNEL_ID);

    // ---- ISO14230 fast init ----
    unsigned long kwp = 0;
    CHECK(dev.connect(ISO14230, 0, 10400, &kwp) == STATUS_NOERROR);
    static PASSTHRU_MSG start, startResp;
    start.ProtocolID = ISO14230;
    start.DataSize = 4;
    memcpy(start.Data, "\xC1\x33\xF1\x81", 4);
    CHECK(dev.fastInit(kwp, &start, &startResp) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "aty4 4 0 "));
    CHECK(startResp.DataSize == 7 && startResp.Data[0] == 0x83 && startResp.Data[6] == 0xC3);
    CHECK(startResp.ProtocolID == ISO14230);
    CHECK(dev.disconnect(kwp) == STATUS_NOERROR);

    // ---- L line uses wire channel 7 ----
    unsigned long l = 0;
    CHECK(dev.connect(ISO9141_CH2, 0, 10400, &l) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "ato7 0 10400 37441 "));
    CHECK(dev.disconnect(l) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atc7 "));

    // ---- CAN ----
    unsigned long can = 0;
    CHECK(dev.connect(CAN, 0, 500000, &can) == STATUS_NOERROR);
    msg.ProtocolID = CAN;
    msg.DataSize = 6;
    memcpy(msg.Data, "\x00\x00\x07\xE0\x01\x00", 6);
    n = 1;
    CHECK(dev.writeMsgs(can, &msg, &n, 100) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "att5 6 0 "));
    n = 2;
    CHECK(dev.readMsgs(can, rx, &n, 1000) == STATUS_NOERROR);
    CHECK(rx[0].RxStatus == TX_MSG_TYPE && rx[0].DataSize == 6 && rx[0].Timestamp == 200000);
    CHECK(rx[1].RxStatus == 0 && rx[1].Data[3] == 0xE0 && rx[1].ExtraDataIndex == 6);
    CHECK(dev.disconnect(can) == STATUS_NOERROR);

    // ---- ISO15765 ----
    unsigned long iso = 0;
    CHECK(dev.connect(ISO15765, 0, 500000, &iso) == STATUS_NOERROR);
    static PASSTHRU_MSG flow;
    mask.ProtocolID = pattern.ProtocolID = flow.ProtocolID = ISO15765;
    mask.DataSize = pattern.DataSize = flow.DataSize = 4;
    memset(mask.Data, 0xFF, 4);
    memcpy(pattern.Data, "\x00\x00\x07\xE8", 4);
    memcpy(flow.Data, "\x00\x00\x07\xE0", 4);
    CHECK(dev.startFilter(iso, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter) == STATUS_NOERROR);
    CHECK(dev.startFilter(iso, FLOW_CONTROL_FILTER, &mask, &pattern, nullptr, &filter) == ERR_NULL_PARAMETER);
    msg.ProtocolID = ISO15765;
    msg.DataSize = 6;
    memcpy(msg.Data, "\x00\x00\x07\xE0\x10\x03", 6);
    n = 1;
    CHECK(dev.writeMsgs(iso, &msg, &n, 200) == STATUS_NOERROR);
    n = 4;
    CHECK(dev.readMsgs(iso, rx, &n, 1000) == STATUS_NOERROR);
    CHECK(n == 4);
    CHECK(rx[0].RxStatus == (TX_DONE | TX_MSG_TYPE) && rx[0].DataSize == 4 && rx[0].ExtraDataIndex == 0);
    CHECK(rx[1].RxStatus == START_OF_MESSAGE && rx[1].DataSize == 4);
    CHECK(rx[2].RxStatus == 0 && rx[2].DataSize == 13 && rx[2].Data[3] == 0xE0);
    CHECK(rx[2].Data[4] == 1 && rx[2].Data[9] == 6 && rx[2].Data[10] == 7 && rx[2].Data[12] == 9);
    CHECK(rx[3].RxStatus == 0 && rx[3].DataSize == 6 && rx[3].Data[4] == 0x50 && rx[3].Data[5] == 0x03);
    CHECK(dev.disconnect(iso) == STATUS_NOERROR);

    // ---- pins and services ----
    CHECK(dev.setProgrammingVoltage(0, 12000) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atv 0 12000 "));
    CHECK(dev.setProgrammingVoltage(0, VOLTAGE_OFF) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atv 0 -1 "));
    CHECK(dev.setProgrammingVoltage(9, SHORT_TO_GROUND) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atv 9 -2 "));

    struct {
        unsigned long length, service;
        unsigned short item;
    } svcIn = {2, 5, 1};
    struct {
        unsigned long length;
        unsigned char data[64];
    } svcOut = {};
    CHECK(dev.appService(&svcIn, &svcOut) == STATUS_NOERROR);
    CHECK(hasCommand(sim, "atp 5 2 "));
    CHECK(svcOut.length == 8 && memcmp(svcOut.data, "TAabc123", 8) == 0);

    dev.close();  // also frees the simulator, which the device owns
    unsigned long dummy = 0;
    CHECK(dev.readPinVoltage(16, &dummy) == ERR_DEVICE_NOT_CONNECTED);

    if (g_failures == 0) printf("selftest: all checks passed\n");
    else printf("selftest: %d check(s) FAILED\n", g_failures);
    return g_failures ? 1 : 0;
}
