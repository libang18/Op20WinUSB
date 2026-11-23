#include "device.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace {

const size_t kMaxQueuedMsgs = 8192;
const DWORD kCommandTimeoutMs = 2000;
const DWORD kInitTimeoutMs = 5000;   // five-baud / fast init, as in the original DLL
const DWORD kWriteTimeoutMs = 2000;  // USB write
const size_t kMaxLine = 256;

// Packet type bits sent by the firmware in "ar<ch><len><type>..." packets.
const uint8_t kTypeStart = 0x80;      // start-of-message indication
const uint8_t kTypeEnd = 0x40;        // end-of-message indication
const uint8_t kTypeLoopback = 0x20;   // echo of a transmitted message
const uint8_t kTypeTxDone = 0x10;     // ISO15765 transmit complete
const uint8_t kTypeNoDeliver = 0x08;  // not passed on to the application
const uint8_t kTypeExtAddr = 0x04;    // ISO15765 extended addressing
const uint8_t kType29Bit = 0x02;      // 29-bit CAN ID

// J2534 protocol ID -> channel number used on the wire.
int wireChannel(unsigned long protocol) {
    switch (protocol) {
        case J1850VPW:
        case J1850VPW_CH1: return 1;
        case J1850PWM:
        case J1850PWM_CH1: return 2;
        case ISO9141:
        case ISO9141_CH1: return 3;
        case ISO14230:
        case ISO14230_CH1: return 4;
        case CAN:
        case CAN_CH1: return 5;
        case ISO15765:
        case ISO15765_CH1: return 6;
        case ISO9141_CH2: return 7;   // L line
        case ISO14230_CH2: return 8;  // L line
        case ISO9141_CH3: return 9;   // 2.5 mm jack, receive only
        default: return 0;
    }
}

bool isKLineWire(int wire) { return wire == 3 || wire == 4 || wire == 7 || wire == 8 || wire == 9; }

unsigned long defaultTxTimeoutUs(int wire) { return wire == 5 ? 50000UL : 1000000UL; }

uint32_t be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

std::vector<long long> parseInts(const std::string &text) {
    std::vector<long long> out;
    const char *p = text.c_str();
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        char *end = nullptr;
        long long v = strtoll(p, &end, 10);
        if (end == p) break;
        out.push_back(v);
        p = end;
    }
    return out;
}

const char *errorName(long code) {
    static const char *const names[] = {
        "STATUS_NOERROR",          "ERR_NOT_SUPPORTED",      "ERR_INVALID_CHANNEL_ID",    "ERR_INVALID_PROTOCOL_ID",
        "ERR_NULL_PARAMETER",      "ERR_INVALID_IOCTL_VALUE", "ERR_INVALID_FLAGS",         "ERR_FAILED",
        "ERR_DEVICE_NOT_CONNECTED", "ERR_TIMEOUT",            "ERR_INVALID_MSG",           "ERR_INVALID_TIME_INTERVAL",
        "ERR_EXCEEDED_LIMIT",      "ERR_INVALID_MSG_ID",     "ERR_DEVICE_IN_USE",         "ERR_INVALID_IOCTL_ID",
        "ERR_BUFFER_EMPTY",        "ERR_BUFFER_FULL",        "ERR_BUFFER_OVERFLOW",       "ERR_PIN_INVALID",
        "ERR_CHANNEL_IN_USE",      "ERR_MSG_PROTOCOL_ID",    "ERR_INVALID_FILTER_ID",     "ERR_NO_FLOW_CONTROL",
        "ERR_NOT_UNIQUE",          "ERR_INVALID_BAUDRATE",   "ERR_INVALID_DEVICE_ID"};
    if (code >= 0 && code < static_cast<long>(sizeof(names) / sizeof(names[0]))) return names[code];
    if (code == 0x78) return "ERR_OEM_VOLTAGE_TOO_LOW";
    if (code == 0x77) return "ERR_OEM_VOLTAGE_TOO_HIGH";
    return "device error";
}

std::string fmt(const char *format, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return buf;
}

// "ari" carries "main code version : <n>"; Tactrix prints it as M.mm.bbbb.
std::string firmwareFromIdent(const std::string &text) {
    size_t colon = text.rfind(':');
    std::string tail = colon == std::string::npos ? text : text.substr(colon + 1);
    size_t a = tail.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "unknown";
    size_t b = tail.find_last_not_of(" \t\r");
    tail = tail.substr(a, b - a + 1);
    if (tail.find_first_not_of("0123456789") != std::string::npos) return tail;
    unsigned long v = strtoul(tail.c_str(), nullptr, 10);
    return fmt("%lu.%02lu.%04lu", v / 1000000, (v / 10000) % 100, v % 10000);
}

}  // namespace

Device::Device() {}

Device::~Device() { close(); }

std::string Device::error() {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

long Device::fail(long code, const std::string &message) {
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_ = message;
    }
    OPLOG("error %ld (%s): %s", code, errorName(code), message.c_str());
    return code;
}

// ------------------------------------------------------------ open/close --

long Device::open() {
    oplog::init();
    OpenResult r = openOpenport();
    if (r.status != OpenStatus::Ok) {
        return fail(r.status == OpenStatus::InUse ? ERR_DEVICE_IN_USE : ERR_DEVICE_NOT_CONNECTED, r.message);
    }
    return attach(std::move(r.transport));
}

long Device::attach(std::unique_ptr<Transport> transport) {
    transport_ = std::move(transport);
    transportName_ = transport_->description();
    rx_.clear();
    alive_ = true;
    reader_ = std::thread(&Device::readerLoop, this);

    // Identify. The leading blank lines terminate any half-typed command a
    // previous session may have left in the device's input buffer.
    static const char ident[] = "\r\n\r\nati\r\n";
    bool identified = false;
    std::string identText;
    for (int attempt = 0; attempt < 3 && !identified && alive_; attempt++) {
        std::lock_guard<std::mutex> cmd(cmdMutex_);
        {
            std::lock_guard<std::mutex> lock(replyMutex_);
            pendingActive_ = true;
            pendingDone_ = false;
            pendingAnySeq_ = true;
            pendingExpect_ = 'i';
            pendingReply_ = Reply();
        }
        OPLOG("TX ati (attempt %d)", attempt + 1);
        bool sent = transport_->write(reinterpret_cast<const uint8_t *>(ident), sizeof(ident) - 1, kWriteTimeoutMs);
        std::unique_lock<std::mutex> lock(replyMutex_);
        if (sent)
            replyCv_.wait_for(lock, std::chrono::milliseconds(1000), [&] { return pendingDone_ || !alive_; });
        pendingActive_ = false;
        if (pendingDone_) {
            identified = true;
            identText = pendingReply_.text;
        }
    }
    if (!identified) {
        close();
        return fail(ERR_DEVICE_NOT_CONNECTED, "device not responding");
    }
    firmware_ = firmwareFromIdent(identText);
    OPLOG("identified: '%s' -> firmware %s via %s", identText.c_str(), firmware_.c_str(), transportName_.c_str());

    long rc = command('o', "ata", nullptr, 0, kCommandTimeoutMs, nullptr);
    if (rc != STATUS_NOERROR) {
        std::string message = error();
        close();
        return fail(rc, "device refused to open: " + message);
    }
    return STATUS_NOERROR;
}

void Device::close() {
    if (transport_) {
        if (alive_) command('o', "atz", nullptr, 0, 500, nullptr);
        alive_ = false;
        transport_->cancelRead();
    }
    if (reader_.joinable()) reader_.join();
    transport_.reset();
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        channels_.clear();
    }
    rxCv_.notify_all();
}

// ---------------------------------------------------------- command path --

long Device::command(char expect, const std::string &header, const uint8_t *payload, size_t payloadLen,
                     DWORD timeoutMs, Reply *reply) {
    std::lock_guard<std::mutex> cmd(cmdMutex_);
    if (!alive_ || !transport_) return fail(ERR_DEVICE_NOT_CONNECTED, "device not connected");

    // Every command carries a sequence number that the device echoes in its
    // reply, which is how replies are matched to commands.
    seq_ = seq_ >= 0xFFFF ? 1 : seq_ + 1;
    const unsigned seq = seq_;
    std::string line = header + " " + std::to_string(seq) + "\r\n";
    std::vector<uint8_t> out(line.begin(), line.end());
    if (payloadLen) out.insert(out.end(), payload, payload + payloadLen);

    {
        std::lock_guard<std::mutex> lock(replyMutex_);
        pendingActive_ = true;
        pendingDone_ = false;
        pendingAnySeq_ = false;
        pendingSeq_ = seq;
        pendingExpect_ = expect;
        pendingReply_ = Reply();
    }
    if (oplog::enabled()) oplog::dump("TX", out.data(), out.size());
    if (!transport_->write(out.data(), out.size(), kWriteTimeoutMs)) {
        std::lock_guard<std::mutex> lock(replyMutex_);
        pendingActive_ = false;
        return fail(ERR_DEVICE_NOT_CONNECTED, "USB write failed");
    }

    std::unique_lock<std::mutex> lock(replyMutex_);
    replyCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return pendingDone_ || !alive_; });
    pendingActive_ = false;
    if (!pendingDone_) {
        lock.unlock();
        if (!alive_) return fail(ERR_DEVICE_NOT_CONNECTED, "device disconnected");
        return fail(ERR_TIMEOUT, "no reply from the device to '" + header.substr(0, 3) + "'");
    }
    Reply r = std::move(pendingReply_);
    lock.unlock();

    if (r.err != STATUS_NOERROR)
        return fail(r.err, fmt("device reported %s (%ld) for '%.3s'", errorName(r.err), r.err, header.c_str()));
    if (reply) *reply = std::move(r);
    return STATUS_NOERROR;
}

long Device::sendOnly(const std::string &header, const uint8_t *payload, size_t payloadLen) {
    std::lock_guard<std::mutex> cmd(cmdMutex_);
    if (!alive_ || !transport_) return fail(ERR_DEVICE_NOT_CONNECTED, "device not connected");
    std::string line = header + "\r\n";
    std::vector<uint8_t> out(line.begin(), line.end());
    if (payloadLen) out.insert(out.end(), payload, payload + payloadLen);
    if (oplog::enabled()) oplog::dump("TX", out.data(), out.size());
    if (!transport_->write(out.data(), out.size(), kWriteTimeoutMs))
        return fail(ERR_DEVICE_NOT_CONNECTED, "USB write failed");
    return STATUS_NOERROR;
}

int Device::wireOf(unsigned long channelId) {
    std::lock_guard<std::mutex> lock(rxMutex_);
    for (auto &kv : channels_)
        if (kv.second.id == channelId) return kv.first;
    return 0;
}

bool Device::hasChannel(unsigned long channelId) { return wireOf(channelId) != 0; }

// -------------------------------------------------------------- channels --

long Device::connect(unsigned long protocol, unsigned long flags, unsigned long baud, unsigned long *channelId) {
    const int wire = wireChannel(protocol);
    if (wire == 0) return fail(ERR_INVALID_PROTOCOL_ID, fmt("unknown protocol ID 0x%lX", protocol));
    if (wire == 1 || wire == 2) return fail(ERR_NOT_SUPPORTED, "J1850 is not supported by the Openport 2.0");
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (channels_.count(wire)) return fail(ERR_CHANNEL_IN_USE, "channel already connected");
        // Register first so packets arriving right after the connect are kept.
        Channel &ch = channels_[wire];
        ch.id = protocol;
        ch.wire = wire;
    }
    long rc = command('o', fmt("ato%d %lu %lu %lu", wire, flags, baud, protocol), nullptr, 0, kCommandTimeoutMs,
                      nullptr);
    if (rc != STATUS_NOERROR) {
        std::lock_guard<std::mutex> lock(rxMutex_);
        channels_.erase(wire);
        return rc;
    }
    *channelId = protocol;
    return STATUS_NOERROR;
}

long Device::disconnect(unsigned long channelId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    long rc = command('o', fmt("atc%d", wire), nullptr, 0, kCommandTimeoutMs, nullptr);
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        channels_.erase(wire);
    }
    rxCv_.notify_all();
    return rc;
}

long Device::clearRx(unsigned long channelId) {
    std::lock_guard<std::mutex> lock(rxMutex_);
    for (auto &kv : channels_) {
        if (kv.second.id != channelId) continue;
        kv.second.queue.clear();
        kv.second.overflow = false;
        return STATUS_NOERROR;
    }
    return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
}

// ------------------------------------------------------------ read/write --

long Device::readMsgs(unsigned long channelId, PASSTHRU_MSG *msgs, unsigned long *count, unsigned long timeoutMs) {
    const unsigned long want = *count;
    *count = 0;
    if (want == 0) return STATUS_NOERROR;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    unsigned long got = 0;
    bool overflow = false;

    std::unique_lock<std::mutex> lock(rxMutex_);
    for (;;) {
        // Looked up on every pass: the channel may be disconnected while waiting.
        Channel *ch = nullptr;
        for (auto &kv : channels_)
            if (kv.second.id == channelId) ch = &kv.second;
        if (!ch) {
            *count = got;
            lock.unlock();
            return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        }
        while (got < want && !ch->queue.empty()) {
            RxMsg &m = ch->queue.front();
            PASSTHRU_MSG &dst = msgs[got++];
            dst.ProtocolID = m.protocol ? m.protocol : ch->id;
            dst.RxStatus = m.rxStatus;
            dst.TxFlags = 0;
            dst.Timestamp = m.timestamp;
            const size_t n = m.data.size() > PASSTHRU_MSG_DATA_SIZE ? PASSTHRU_MSG_DATA_SIZE : m.data.size();
            dst.DataSize = static_cast<unsigned long>(n);
            dst.ExtraDataIndex = m.extraDataIndex > n ? static_cast<unsigned long>(n) : m.extraDataIndex;
            if (n) memcpy(dst.Data, m.data.data(), n);
            ch->queue.pop_front();
        }
        if (ch->overflow) {
            overflow = true;
            ch->overflow = false;
        }
        if (got >= want || timeoutMs == 0) break;
        if (!alive_) {
            *count = got;
            lock.unlock();
            return fail(ERR_DEVICE_NOT_CONNECTED, "device disconnected");
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        rxCv_.wait_until(lock, deadline);
    }
    lock.unlock();

    *count = got;
    if (overflow) return fail(ERR_BUFFER_OVERFLOW, "receive buffer overflowed; messages were lost");
    if (got == 0) return fail(ERR_BUFFER_EMPTY, "no data was read");
    if (got < want && timeoutMs != 0)
        return fail(ERR_TIMEOUT, "fewer messages were read than requested before timeout");
    return STATUS_NOERROR;
}

long Device::writeMsgs(unsigned long channelId, const PASSTHRU_MSG *msgs, unsigned long *count,
                       unsigned long timeoutMs) {
    const unsigned long total = *count;
    *count = 0;
    const int channelWire = wireOf(channelId);
    if (!channelWire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");

    const ULONGLONG start = GetTickCount64();
    for (unsigned long i = 0; i < total; i++) {
        const PASSTHRU_MSG &m = msgs[i];
        int wire = channelWire;
        if (wireChannel(m.ProtocolID) != channelWire) {
            // Raw CAN frames may be sent on an ISO15765 channel.
            if (channelWire == 6 && wireChannel(m.ProtocolID) == 5)
                wire = 5;
            else
                return fail(ERR_MSG_PROTOCOL_ID, "message protocol ID does not match the channel");
        }
        if (m.DataSize == 0 || m.DataSize > PASSTHRU_MSG_DATA_SIZE)
            return fail(ERR_INVALID_MSG, fmt("invalid message size %lu", m.DataSize));

        long rc;
        if (timeoutMs == 0) {
            // Queue and return: no sequence number, so the device's
            // acknowledgement is not waited for.
            rc = sendOnly(fmt("att%d %lu %lu %lu", wire, m.DataSize, m.TxFlags, defaultTxTimeoutUs(wire)), m.Data,
                          m.DataSize);
        } else {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) return fail(ERR_TIMEOUT, "timeout before all messages were transmitted");
            const DWORD remaining = static_cast<DWORD>(timeoutMs - elapsed);
            rc = command('o', fmt("att%d %lu %lu %lu", wire, m.DataSize, m.TxFlags, remaining * 1000UL), m.Data,
                         m.DataSize, remaining + 250, nullptr);
        }
        if (rc != STATUS_NOERROR) return rc;
        *count = i + 1;
    }
    return STATUS_NOERROR;
}

// ------------------------------------------------ periodic msgs, filters --

long Device::startPeriodic(unsigned long channelId, const PASSTHRU_MSG *msg, unsigned long *msgId,
                           unsigned long interval) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    if (msg->DataSize == 0 || msg->DataSize > PASSTHRU_MSG_DATA_SIZE)
        return fail(ERR_INVALID_MSG, fmt("invalid message size %lu", msg->DataSize));

    // Tactrix extension: with the top bit set, the low word is the interval
    // and bits 16..30 a second time value, both in ms.
    unsigned long us, us2 = 0;
    if (interval & 0x80000000UL) {
        us = (interval & 0xFFFF) * 1000UL;
        us2 = ((interval >> 16) & 0x7FFF) * 1000UL;
    } else {
        if (interval < 5 || interval > 65535)
            return fail(ERR_INVALID_TIME_INTERVAL, fmt("invalid periodic interval %lu ms", interval));
        us = interval * 1000UL;
    }
    Reply r;
    long rc = command('m', fmt("atm%d %lu %lu %lu %lu", wire, us, us2, msg->TxFlags, msg->DataSize), msg->Data,
                      msg->DataSize, kCommandTimeoutMs, &r);
    if (rc != STATUS_NOERROR) return rc;
    if (r.values.size() < 2) return fail(ERR_FAILED, "malformed periodic message reply");
    *msgId = static_cast<unsigned long>(r.values[1]);
    return STATUS_NOERROR;
}

long Device::stopPeriodic(unsigned long channelId, unsigned long msgId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    return command('o', fmt("atn%d %ld", wire, static_cast<long>(msgId)), nullptr, 0, kCommandTimeoutMs, nullptr);
}

long Device::clearPeriodic(unsigned long channelId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    return command('o', fmt("atl%d", wire), nullptr, 0, kCommandTimeoutMs, nullptr);
}

long Device::startFilter(unsigned long channelId, unsigned long type, const PASSTHRU_MSG *mask,
                         const PASSTHRU_MSG *pattern, const PASSTHRU_MSG *flow, unsigned long *filterId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    if (type < PASS_FILTER || type > FLOW_CONTROL_FILTER) return fail(ERR_INVALID_MSG, "invalid filter type");
    const unsigned long n = mask->DataSize;
    if (n == 0 || n > 12 || pattern->DataSize != n)
        return fail(ERR_INVALID_MSG, "invalid filter mask/pattern size");
    if (type == FLOW_CONTROL_FILTER) {
        if (!flow) return fail(ERR_NULL_PARAMETER, "flow control filter needs a flow control message");
        if (flow->DataSize != n) return fail(ERR_INVALID_MSG, "invalid flow control message size");
    }

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), mask->Data, mask->Data + n);
    payload.insert(payload.end(), pattern->Data, pattern->Data + n);
    if (type == FLOW_CONTROL_FILTER) payload.insert(payload.end(), flow->Data, flow->Data + n);

    Reply r;
    long rc = command('f', fmt("atf%d %lu %lu %lu", wire, type, mask->TxFlags, n), payload.data(), payload.size(),
                      kCommandTimeoutMs, &r);
    if (rc != STATUS_NOERROR) return rc;
    if (r.values.size() < 2) return fail(ERR_FAILED, "malformed filter reply");
    *filterId = static_cast<unsigned long>(r.values[1]);
    return STATUS_NOERROR;
}

long Device::stopFilter(unsigned long channelId, unsigned long filterId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    return command('o', fmt("atk%d %ld", wire, static_cast<long>(filterId)), nullptr, 0, kCommandTimeoutMs, nullptr);
}

long Device::clearFilters(unsigned long channelId) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    return command('o', fmt("atk%d -1", wire), nullptr, 0, kCommandTimeoutMs, nullptr);
}

// ---------------------------------------------------------------- config --

long Device::getConfig(unsigned long channelId, SCONFIG *item) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    Reply r;
    long rc = command('g', fmt("atg%d %lu", wire, item->Parameter), nullptr, 0, kCommandTimeoutMs, &r);
    if (rc != STATUS_NOERROR) return rc;
    if (r.values.size() < 3) return fail(ERR_FAILED, "malformed GET_CONFIG reply");
    item->Value = static_cast<unsigned long>(r.values[2]);
    return STATUS_NOERROR;
}

long Device::setConfig(unsigned long channelId, const SCONFIG *item) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    return command('o', fmt("ats%d %lu %lu", wire, item->Parameter, item->Value), nullptr, 0, kCommandTimeoutMs,
                   nullptr);
}

// ----------------------------------------------------------- K-line init --

long Device::fiveBaudInit(unsigned long channelId, const SBYTE_ARRAY *in, SBYTE_ARRAY *out) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
    if (!in || !in->BytePtr || !out || !out->BytePtr)
        return fail(ERR_NULL_PARAMETER, "FIVE_BAUD_INIT needs input and output byte arrays");
    if (in->NumOfBytes != 1) return fail(ERR_INVALID_MSG, "FIVE_BAUD_INIT takes exactly one address byte");

    Reply r;
    long rc = command('w', fmt("atw%d %u", wire, static_cast<unsigned>(in->BytePtr[0])), nullptr, 0, kInitTimeoutMs,
                      &r);
    if (rc != STATUS_NOERROR) return rc;
    // arw<ch> <keybyte1> <keybyte2> <seq>
    if (r.values.size() < 3) return fail(ERR_FAILED, "malformed five-baud init reply");
    out->BytePtr[0] = static_cast<unsigned char>(r.values[1]);
    out->BytePtr[1] = static_cast<unsigned char>(r.values[2]);
    out->NumOfBytes = 2;
    return STATUS_NOERROR;
}

long Device::fastInit(unsigned long channelId, const PASSTHRU_MSG *in, PASSTHRU_MSG *out) {
    const int wire = wireOf(channelId);
    if (!wire) return fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");

    Reply r;
    long rc;
    if (in) {
        if (in->DataSize > 16) return fail(ERR_INVALID_MSG, "fast init message is too long");
        rc = command('y', fmt("aty%d %lu %lu", wire, in->DataSize, in->TxFlags), in->Data, in->DataSize,
                     kInitTimeoutMs, &r);
    } else {
        rc = command('y', fmt("aty%d 0 0", wire), nullptr, 0, kInitTimeoutMs, &r);
    }
    if (rc != STATUS_NOERROR) return rc;
    if (out) {
        const size_t n = r.data.size() > PASSTHRU_MSG_DATA_SIZE ? PASSTHRU_MSG_DATA_SIZE : r.data.size();
        out->ProtocolID = channelId;
        out->RxStatus = 0;
        out->TxFlags = 0;
        out->Timestamp = 0;
        out->DataSize = static_cast<unsigned long>(n);
        out->ExtraDataIndex = static_cast<unsigned long>(n);
        if (n) memcpy(out->Data, r.data.data(), n);
    }
    return STATUS_NOERROR;
}

// ------------------------------------------------------- pins / services --

long Device::readPinVoltage(unsigned long pin, unsigned long *millivolts) {
    Reply r;
    long rc = command('r', fmt("atr %lu", pin), nullptr, 0, kCommandTimeoutMs, &r);
    if (rc != STATUS_NOERROR) return rc;
    // arr <pin> <millivolts> <seq>
    if (r.values.size() < 2) return fail(ERR_FAILED, "malformed voltage reply");
    *millivolts = static_cast<unsigned long>(r.values[1]);
    return STATUS_NOERROR;
}

long Device::setProgrammingVoltage(unsigned long pin, unsigned long voltage) {
    // Sent signed, so VOLTAGE_OFF is -1 and SHORT_TO_GROUND is -2 on the wire.
    long rc = command('o', fmt("atv %lu %ld", pin, static_cast<long>(voltage)), nullptr, 0, kCommandTimeoutMs,
                      nullptr);
    if (rc == STATUS_NOERROR) {
        lastProgPin_ = pin;
        lastProgVoltage_ = voltage;
    }
    return rc;
}

long Device::readProgVoltage(const unsigned long *pin, unsigned long *millivolts) {
    return readPinVoltage(pin ? *pin : lastProgPin_, millivolts);
}

// TX_IOCTL_APP_SERVICE: in = { ulong length; ulong service; uchar data[length] },
// out = { ulong length; uchar data[] }.
long Device::appService(const void *in, void *out) {
    if (!in || !out) return fail(ERR_FAILED, "APP_SERVICE needs input and output buffers");
    const unsigned long *hdr = static_cast<const unsigned long *>(in);
    const unsigned long len = hdr[0], service = hdr[1];
    if (len > 0x80) return fail(ERR_FAILED, "APP_SERVICE request is too long");

    Reply r;
    long rc = command('p', fmt("atp %lu %lu", service, len), reinterpret_cast<const uint8_t *>(hdr + 2), len,
                      kInitTimeoutMs, &r);
    if (rc != STATUS_NOERROR) return rc;
    unsigned long *outLen = static_cast<unsigned long *>(out);
    *outLen = static_cast<unsigned long>(r.data.size());
    if (!r.data.empty()) memcpy(outLen + 1, r.data.data(), r.data.size());
    return STATUS_NOERROR;
}

// ========================================================== reader thread ==

void Device::readerLoop() {
    std::vector<uint8_t> buf(transport_->readChunk());
    for (;;) {
        int n = transport_->read(buf.data(), buf.size());
        if (n == Transport::ReadCancelled) break;
        if (n < 0) {
            OPLOG("device read failed - treating the device as unplugged");
            break;
        }
        if (n == 0) continue;
        if (oplog::enabled()) oplog::dump("RX", buf.data(), static_cast<size_t>(n));
        rx_.insert(rx_.end(), buf.begin(), buf.begin() + n);
        parse();
    }
    alive_ = false;
    { std::lock_guard<std::mutex> lock(replyMutex_); }
    replyCv_.notify_all();
    { std::lock_guard<std::mutex> lock(rxMutex_); }
    rxCv_.notify_all();
}

// The stream is a mix of
//   text replies:    "ar" <letter> ... "\r\n"      (some followed by raw bytes)
//   message packets: "ar" <digit> <len> <type> <len-1 bytes>
void Device::parse() {
    size_t pos = 0;
    const size_t size = rx_.size();
    for (;;) {
        while (pos + 1 < size && !(rx_[pos] == 'a' && rx_[pos + 1] == 'r')) pos++;
        if (size - pos < 3) {
            if (size - pos == 1 && rx_[pos] != 'a') pos++;
            break;
        }
        const uint8_t c = rx_[pos + 2];

        if (c >= '1' && c <= '9') {
            if (size - pos < 5) break;
            const size_t len = rx_[pos + 3];
            if (len == 0) {  // cannot be a packet; resynchronise
                pos += 2;
                continue;
            }
            if (size - pos < 4 + len) break;
            handlePacket(c - '0', rx_[pos + 4], &rx_[pos + 5], len - 1);
            pos += 4 + len;
            continue;
        }

        if (c == 'b' || c == 'e' || c == 'f' || c == 'g' || c == 'i' || c == 'm' || c == 'o' || c == 'p' ||
            c == 'r' || c == 'w' || c == 'y') {
            size_t nl = pos + 3;
            const size_t limit = pos + 3 + kMaxLine < size ? pos + 3 + kMaxLine : size;
            while (nl < limit && rx_[nl] != '\n') nl++;
            if (nl >= limit) {
                if (limit < size) {  // no line end within reach: not a reply
                    pos += 2;
                    continue;
                }
                break;  // wait for the rest of the line
            }
            std::string text(reinterpret_cast<const char *>(&rx_[pos + 3]), nl - (pos + 3));
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
            size_t next = nl + 1;

            std::vector<uint8_t> data;
            if (c == 'y' || c == 'p') {
                // ary<ch> <len> <seq> / arp <svc> <len> <seq>, then <len> raw bytes
                std::vector<long long> v = parseInts(text);
                size_t extra = v.size() >= 2 && v[1] > 0 && v[1] <= PASSTHRU_MSG_DATA_SIZE
                                   ? static_cast<size_t>(v[1])
                                   : 0;
                if (size - next < extra) break;
                data.assign(rx_.begin() + next, rx_.begin() + next + extra);
                next += extra;
            }
            handleLine(static_cast<char>(c), text, std::move(data));
            pos = next;
            continue;
        }

        pos += 2;  // "ar" followed by something unknown
    }
    if (pos) rx_.erase(rx_.begin(), rx_.begin() + pos);
}

void Device::handleLine(char kind, const std::string &text, std::vector<uint8_t> &&data) {
    if (kind == 'b') return;  // device debug output

    std::vector<long long> values = parseInts(text);
    // Number of values in a reply that includes the echoed sequence number.
    size_t full = 0;
    switch (kind) {
        case 'o': full = 1; break;                     // aro <seq>
        case 'e': full = 2; break;                     // are <error> <seq>
        case 'f': case 'm': case 'r': full = 3; break; // arf<ch> <id> <seq> / arm.. / arr <pin> <mV> <seq>
        case 'y': case 'p': full = 3; break;           // ary<ch> <len> <seq> / arp <svc> <len> <seq>
        case 'g': case 'w': full = 4; break;           // arg<ch> <param> <value> <seq> / arw<ch> <kb1> <kb2> <seq>
        default: break;
    }
    const bool hasSeq = full != 0 && values.size() >= full;
    const unsigned seq = hasSeq ? static_cast<unsigned>(values[full - 1]) : 0;

    std::lock_guard<std::mutex> lock(replyMutex_);
    if (pendingActive_ && !pendingDone_) {
        bool match;
        if (pendingAnySeq_)
            match = kind == pendingExpect_;
        else
            match = hasSeq && seq == pendingSeq_;
        if (match) {
            if (kind == 'e') {
                pendingReply_.err = !values.empty() && values[0] > 0 ? static_cast<long>(values[0]) : ERR_FAILED;
            } else if (kind != pendingExpect_ && kind != 'o') {
                OPLOG("unexpected reply kind '%c' (wanted '%c')", kind, pendingExpect_);
                pendingReply_.err = ERR_FAILED;
            }
            pendingReply_.values = std::move(values);
            pendingReply_.data = std::move(data);
            pendingReply_.text = text;
            pendingDone_ = true;
            replyCv_.notify_all();
            return;
        }
    }
    if (kind == 'e')
        OPLOG("device error without a waiting command: are%s", text.c_str());
}

void Device::handlePacket(int wire, uint8_t type, const uint8_t *payload, size_t len) {
    std::lock_guard<std::mutex> lock(rxMutex_);
    if (wire == 5) {
        handleCan(type, payload, len);
        return;
    }
    auto it = channels_.find(wire);
    if (it == channels_.end()) return;
    if (wire == 6)
        handleIso15765(it->second, type, payload, len);
    else if (isKLineWire(wire))
        handleKLine(it->second, type, payload, len);
}

void Device::deliver(Channel &ch, RxMsg &&msg) {
    if (ch.queue.size() >= kMaxQueuedMsgs) {
        ch.queue.pop_front();
        ch.overflow = true;
    }
    ch.queue.push_back(std::move(msg));
    rxCv_.notify_all();
}

// K-line / L-line / aux serial. A message arrives as
//   start indication (timestamp) -> data packet(s), no timestamp -> end indication (timestamp)
// and is handed to the application when the end indication arrives.
void Device::handleKLine(Channel &ch, uint8_t type, const uint8_t *payload, size_t len) {
    if ((type & kTypeStart) && len == 4) {
        ch.partial.clear();
        RxMsg m;
        m.rxStatus = START_OF_MESSAGE;
        m.timestamp = be32(payload);
        deliver(ch, std::move(m));
        return;
    }
    if ((type & kTypeEnd) && len == 4) {
        RxMsg m;
        m.rxStatus = (type & kTypeLoopback) ? TX_MSG_TYPE : 0;
        m.timestamp = be32(payload);
        m.data = std::move(ch.partial);
        m.extraDataIndex = static_cast<unsigned long>(m.data.size());
        ch.partial.clear();
        if (!(type & kTypeNoDeliver)) deliver(ch, std::move(m));
        return;
    }
    // Data. Very long streams are flushed in pieces, as the original DLL does.
    if (ch.partial.size() + len > 0xFE0) {
        RxMsg m;
        m.rxStatus = (type & kTypeLoopback) ? TX_MSG_TYPE : 0;
        m.data = std::move(ch.partial);
        m.extraDataIndex = static_cast<unsigned long>(m.data.size());
        ch.partial.clear();
        deliver(ch, std::move(m));
    }
    ch.partial.insert(ch.partial.end(), payload, payload + len);
}

// Raw CAN: <timestamp:4> <id:4> <data>. Goes to the CAN channel, or to an
// ISO15765 channel running in mixed mode.
void Device::handleCan(uint8_t type, const uint8_t *payload, size_t len) {
    if (len < 4 || (type & kTypeNoDeliver)) return;
    Channel *ch = nullptr;
    unsigned long protocol = 0;
    auto it = channels_.find(5);
    if (it != channels_.end()) {
        ch = &it->second;
    } else if ((it = channels_.find(6)) != channels_.end()) {
        ch = &it->second;
        protocol = CAN;
    }
    if (!ch) return;
    RxMsg m;
    m.protocol = protocol;
    m.timestamp = be32(payload);
    m.data.assign(payload + 4, payload + len);
    m.extraDataIndex = static_cast<unsigned long>(m.data.size());
    m.rxStatus = ((type & kTypeLoopback) ? TX_MSG_TYPE : 0) | ((type & kType29Bit) ? CAN_29BIT_ID : 0);
    deliver(*ch, std::move(m));
}

// ISO15765: every packet is <timestamp:4> <address header:4 or 5> <data>.
void Device::handleIso15765(Channel &ch, uint8_t type, const uint8_t *payload, size_t len) {
    if (len < 4) return;
    const unsigned long ts = be32(payload);
    const uint8_t *body = payload + 4;
    const size_t bodyLen = len - 4;
    const size_t hdr = (type & kTypeExtAddr) ? 5 : 4;
    const unsigned long flags =
        ((type & kTypeExtAddr) ? ISO15765_ADDR_TYPE : 0) | ((type & kType29Bit) ? CAN_29BIT_ID : 0);

    if (type & kTypeTxDone) {
        RxMsg m;
        m.timestamp = ts;
        m.data.assign(body, body + bodyLen);
        m.extraDataIndex = 0;
        m.rxStatus = flags | TX_DONE | TX_MSG_TYPE;
        ch.partial.clear();
        deliver(ch, std::move(m));
        return;
    }
    if (type & kTypeStart) {  // first frame of a segmented message: address only
        ch.partial.assign(body, body + bodyLen);
        RxMsg m;
        m.timestamp = ts;
        m.data = ch.partial;
        m.extraDataIndex = 0;
        m.rxStatus = flags | START_OF_MESSAGE;
        deliver(ch, std::move(m));
        return;
    }
    if (type & kTypeEnd) {
        // The address header is already in place if earlier pieces arrived.
        const size_t skip = ch.partial.empty() ? 0 : (hdr < bodyLen ? hdr : bodyLen);
        ch.partial.insert(ch.partial.end(), body + skip, body + bodyLen);
        RxMsg m;
        m.timestamp = ts;
        m.data = std::move(ch.partial);
        m.extraDataIndex = static_cast<unsigned long>(m.data.size());
        m.rxStatus = flags | ((type & kTypeLoopback) ? TX_MSG_TYPE : 0);
        ch.partial.clear();
        deliver(ch, std::move(m));
        return;
    }
    // Intermediate piece of a segmented message.
    if (bodyLen <= hdr) return;
    if (ch.partial.size() + (bodyLen - hdr) > 0x1020) {
        ch.partial.clear();
        return;
    }
    ch.partial.insert(ch.partial.end(), body + hdr, body + bodyLen);
}
