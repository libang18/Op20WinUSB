// Openport 2.0 protocol engine: turns J2534 operations into the device's
// "at" command stream and its replies / message packets back into J2534
// messages. See PROTOCOL.md for the wire format.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "j2534.h"
#include "transport.h"

class Device {
public:
    Device();
    ~Device();

    // All operations return a J2534 error code; error() describes the last failure.
    long open();
    // Runs the open handshake on an already opened transport (used by open()
    // and by the self-test, which substitutes a simulated device).
    long attach(std::unique_ptr<Transport> transport);
    void close();

    long connect(unsigned long protocol, unsigned long flags, unsigned long baud, unsigned long *channelId);
    long disconnect(unsigned long channelId);
    bool hasChannel(unsigned long channelId);

    long readMsgs(unsigned long channelId, PASSTHRU_MSG *msgs, unsigned long *count, unsigned long timeoutMs);
    long writeMsgs(unsigned long channelId, const PASSTHRU_MSG *msgs, unsigned long *count, unsigned long timeoutMs);

    long startPeriodic(unsigned long channelId, const PASSTHRU_MSG *msg, unsigned long *msgId, unsigned long interval);
    long stopPeriodic(unsigned long channelId, unsigned long msgId);
    long clearPeriodic(unsigned long channelId);

    long startFilter(unsigned long channelId, unsigned long type, const PASSTHRU_MSG *mask, const PASSTHRU_MSG *pattern,
                     const PASSTHRU_MSG *flow, unsigned long *filterId);
    long stopFilter(unsigned long channelId, unsigned long filterId);
    long clearFilters(unsigned long channelId);

    long getConfig(unsigned long channelId, SCONFIG *item);
    long setConfig(unsigned long channelId, const SCONFIG *item);
    long clearRx(unsigned long channelId);

    long fiveBaudInit(unsigned long channelId, const SBYTE_ARRAY *in, SBYTE_ARRAY *out);
    long fastInit(unsigned long channelId, const PASSTHRU_MSG *in, PASSTHRU_MSG *out);

    long readPinVoltage(unsigned long pin, unsigned long *millivolts);
    long setProgrammingVoltage(unsigned long pin, unsigned long voltage);
    long readProgVoltage(const unsigned long *pin, unsigned long *millivolts);
    long appService(const void *in, void *out);

    const std::string &firmwareVersion() const { return firmware_; }
    const std::string &transportName() const { return transportName_; }
    std::string error();

private:
    struct RxMsg {
        unsigned long protocol = 0;  // 0: the channel's own protocol ID
        unsigned long rxStatus = 0;
        unsigned long timestamp = 0;
        unsigned long extraDataIndex = 0;
        std::vector<uint8_t> data;
    };

    struct Channel {
        unsigned long id = 0;  // ChannelID handed to the application (== ProtocolID)
        int wire = 0;          // channel number on the wire, 1..9
        std::deque<RxMsg> queue;
        bool overflow = false;
        std::vector<uint8_t> partial;  // message being assembled from packets
    };

    struct Reply {
        long err = STATUS_NOERROR;
        std::vector<long long> values;
        std::vector<uint8_t> data;
        std::string text;
    };

    // --- command path (caller threads) ---
    long command(char expect, const std::string &header, const uint8_t *payload, size_t payloadLen, DWORD timeoutMs,
                 Reply *reply);
    long sendOnly(const std::string &header, const uint8_t *payload, size_t payloadLen);
    long fail(long code, const std::string &message);
    int wireOf(unsigned long channelId);

    // --- reader thread ---
    void readerLoop();
    void parse();
    void handleLine(char kind, const std::string &text, std::vector<uint8_t> &&data);
    void handlePacket(int wire, uint8_t type, const uint8_t *payload, size_t len);
    void handleKLine(Channel &ch, uint8_t type, const uint8_t *payload, size_t len);
    void handleCan(uint8_t type, const uint8_t *payload, size_t len);
    void handleIso15765(Channel &ch, uint8_t type, const uint8_t *payload, size_t len);
    void deliver(Channel &ch, RxMsg &&msg);

    std::unique_ptr<Transport> transport_;
    std::string transportName_;
    std::string firmware_;
    std::thread reader_;
    std::atomic<bool> alive_{false};
    std::vector<uint8_t> rx_;

    std::mutex cmdMutex_;  // one command on the wire at a time
    unsigned seq_ = 0;

    std::mutex replyMutex_;
    std::condition_variable replyCv_;
    bool pendingActive_ = false;
    bool pendingDone_ = false;
    bool pendingAnySeq_ = false;
    unsigned pendingSeq_ = 0;
    char pendingExpect_ = 0;
    Reply pendingReply_;

    std::mutex rxMutex_;  // channels_ and their queues
    std::condition_variable rxCv_;
    std::map<int, Channel> channels_;  // keyed by wire number

    std::mutex errorMutex_;
    std::string error_;

    unsigned long lastProgVoltage_ = VOLTAGE_OFF;
    unsigned long lastProgPin_ = PIN_VADJ;
};
