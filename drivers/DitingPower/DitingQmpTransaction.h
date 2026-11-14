/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One SM8475 AOSS mailbox owner, shared by WLAN and ADSP. Wire layout follows
 * QcomQmp and Linux qcom_aoss.c. A timed-out message remains owned until AOP
 * clears its length; the host must never manufacture that acknowledgement.
 * The caller serializes the entire handshake, send and completion.
 */
#ifndef DITING_QMP_TRANSACTION_H
#define DITING_QMP_TRANSACTION_H
#include <stdint.h>

namespace DitingQmp {
constexpr unsigned RamBytes = 0x400, MaxMessage = 100;
constexpr uint32_t Up = 0xffff;
enum class Result { Ok, Argument, Descriptor, LinkTimeout, ChannelTimeout, AckTimeout, Busy, SendTimeout };
inline const char *reason(Result r) {
    switch (r) {
    case Result::Ok: return "complete";
    case Result::Argument: return "invalid message";
    case Result::Descriptor: return "invalid or changed mailbox descriptor";
    case Result::LinkTimeout: return "link acknowledgement timeout";
    case Result::ChannelTimeout: return "remote channel timeout";
    case Result::AckTimeout: return "channel acknowledgement timeout";
    case Result::Busy: return "unconsumed mailbox remains owned";
    case Result::SendTimeout: return "message timeout; ownership retained";
    }
    return "unknown result";
}
struct Io {
    void *cookie;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    void (*kick)(void *);
    uint64_t (*nowUs)(void *);
    void (*delayUs)(void *, unsigned);
    void (*record)(void *, const char *, uint32_t, uint32_t);
};
class Controller {
    unsigned phase = 0, box = 0, boxSize = 0;
    bool pending = false;
    uint32_t rd(const Io &io, unsigned off) const { return io.read(io.cookie, off); }
    void wr(const Io &io, unsigned off, uint32_t value) const { io.write(io.cookie, off, value); }
    void record(const Io &io, const char *name, uint32_t value = 0) const {
        if (io.record) io.record(io.cookie, name, box, value);
    }
    bool wait(const Io &io, unsigned off, uint32_t value) const {
        const uint64_t start = io.nowUs(io.cookie);
        for (unsigned i = 0; i < 1000 && io.nowUs(io.cookie) - start < 1000000; ++i) {
            if (rd(io, off) == value) return true;
            io.delayUs(io.cookie, 1000);
        }
        return rd(io, off) == value;
    }
    Result open(const Io &io) {
        if (phase == 0) {
            if (rd(io, 0x0c) == Up && rd(io, 0x10) == Up &&
                rd(io, 0x24) == Up && rd(io, 0x28) == Up &&
                rd(io, 0x14) == Up && rd(io, 0x18) == Up &&
                rd(io, 0x2c) == Up && rd(io, 0x30) == Up) {
                phase = 4;
                return Result::Ok;
            }
            wr(io, 0x10, rd(io, 0x0c));
            wr(io, 0x24, Up); io.kick(io.cookie); phase = 1;
        }
        if (phase == 1) {
            if (!wait(io, 0x28, Up)) return Result::LinkTimeout;
            wr(io, 0x2c, Up); io.kick(io.cookie); phase = 2;
        }
        if (phase == 2) {
            if (!wait(io, 0x14, Up)) return Result::ChannelTimeout;
            wr(io, 0x18, Up); io.kick(io.cookie); phase = 3;
        }
        if (phase == 3) {
            if (!wait(io, 0x30, Up)) return Result::AckTimeout;
            phase = 4;
        }
        return Result::Ok;
    }
public:
    Result send(const Io &io, const char *message, unsigned wireBytes) {
        if (!message || !wireBytes || wireBytes > MaxMessage || (wireBytes & 3)) return Result::Argument;
        uint32_t words[MaxMessage / 4] = {};
        unsigned length = 0;
        for (; length < wireBytes && message[length]; ++length)
            words[length / 4] |= uint32_t(uint8_t(message[length])) << (8 * (length % 4));
        if (!length || length == wireBytes) return Result::Argument;
        const unsigned nextBox = rd(io, 0x38), nextSize = rd(io, 0x34);
        if (rd(io, 0) != 0x4d41494c || rd(io, 4) != 1 ||
            (nextBox & 3) || nextBox < 0x3c || nextBox > RamBytes ||
            nextSize < wireBytes + 4 || nextSize > RamBytes - nextBox ||
            (phase && (nextBox != box || nextSize != boxSize))) return Result::Descriptor;
        box = nextBox; boxSize = nextSize;
        const Result result = open(io);
        if (result != Result::Ok) { record(io, reason(result)); return result; }
        // This covers both our timed-out request and a mailbox inherited
        // from firmware. A nonzero length is never overwritten or cleared.
        if (rd(io, box)) { record(io, "retained-busy", rd(io, box)); return Result::Busy; }
        if (pending) { record(io, "late-complete"); pending = false; }
        for (unsigned i = 0; i < wireBytes / 4; ++i) wr(io, box + 4 + 4 * i, words[i]);
        pending = true;
        record(io, "submit", wireBytes);
        wr(io, box, wireBytes); io.kick(io.cookie);
        if (!wait(io, box, 0)) { record(io, "response-timeout", rd(io, box)); return Result::SendTimeout; }
        pending = false; record(io, "complete", wireBytes);
        return Result::Ok;
    }
};
}
#endif
