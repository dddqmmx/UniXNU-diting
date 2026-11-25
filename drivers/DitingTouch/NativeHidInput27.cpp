// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting iOS 27 (24B156) arm64e IOHIDDevice input source (Home button + 10-finger
// multi-touch digitizer, 12200x27120 Redmi K50 Ultra ST61Y portrait axes).
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>

extern "C" void native_hid_input_observed(unsigned, int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" void native_touch_set_sink(void (*)(void *), void *);
extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *);
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile uint32_t native_touch_edge_stats[4];
volatile uint32_t native_touch_hid_multi[3];
}

// Report 1: boot keyboard (for generic HID matching). Report 2: Consumer buttons (bit 0 = Home/Menu, bit 1 = Power, bit 2 = Vol+, bit 3 = Vol-).
static const unsigned char descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
    0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0x85, 0x02, 0x05, 0x0c, 0x09, 0x40, 0x09, 0x30, 0x09, 0xe9, 0x09, 0xea, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0
};

#define FINGER_DESC \
    0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x09, 0x47, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, \
    0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x02, \
    0x05, 0x01, 0x09, 0x30, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0x05, 0x0d, 0xc0,

static const unsigned char touchDescriptor[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09, 0x54, 0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x02, 0xc0
};
#define TOUCH_FINGERS 10

class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool down = false;
    uint8_t buttons = 0;
    bool touchEnabled = false;
    bool touchOnly = false;
    bool eventDriverFound = false;
    uint32_t lastSeq = 0;
    int prevIds[TOUCH_FINGERS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int prevX[TOUCH_FINGERS] = {}, prevY[TOUCH_FINGERS] = {};

    static unsigned transformCoordinate(int value, int maximum) {
        if (value < 0) value = 0;
        if (value >= maximum) value = maximum - 1;
        return (unsigned)((long long)value * 32767 / (maximum - 1));
    }

    static void touchNotify(void *context) {
        NativeHidInput *self = static_cast<NativeHidInput *>(context);
        if (self && self->inputLoop) self->inputLoop->runAction(&NativeHidInput::touchAction, self);
    }

    static IOReturn touchAction(OSObject *owner, void *, void *, void *, void *) {
        NativeHidInput *self = OSDynamicCast(NativeHidInput, owner);
        if (!self) return kIOReturnSuccess;
        self->touchSend();
        return kIOReturnSuccess;
    }

    void submitTouchReport(unsigned char (&report)[1 + TOUCH_FINGERS * 6 + 1], unsigned count) {
        if (!count) return;
        report[1 + TOUCH_FINGERS * 6] = (unsigned char)count;
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        IOReturn result = buffer ? handleReport(buffer, kIOHIDReportTypeInput, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6, result);
        native_touch_hid_reports = native_touch_hid_reports + 1;
    }

    void touchSend() {
        if (!hasEventDriverCached()) return;
        uint32_t before = 0;
        unsigned count = 0;
        int ids[TOUCH_FINGERS], xs[TOUCH_FINGERS], ys[TOUCH_FINGERS];
        bool snapped = false;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            before = native_touch_seq;
            if ((before & 1) || before == lastSeq) return;
            __asm__ volatile("dmb ish" ::: "memory");
            count = native_touch_count;
            if (count > TOUCH_FINGERS) count = TOUCH_FINGERS;
            for (unsigned i = 0; i < count; ++i) {
                ids[i] = native_touch_ids[i];
                xs[i] = native_touch_xs[i];
                ys[i] = native_touch_ys[i];
            }
            __asm__ volatile("dmb ish" ::: "memory");
            if (native_touch_seq == before) {
                snapped = true;
                break;
            }
        }
        if (!snapped) return;
        lastSeq = before;

        // Collect lifted previous contacts so Tip Up transitions are never lost
        // even when all 10 slots are simultaneously occupied.
        unsigned liftedCount = 0;
        int liftedIds[TOUCH_FINGERS], liftedX[TOUCH_FINGERS], liftedY[TOUCH_FINGERS];
        for (unsigned j = 0; j < TOUCH_FINGERS; ++j) {
            const int id = prevIds[j];
            if (id < 0) continue;
            bool kept = false;
            for (unsigned i = 0; i < count && !kept; ++i) kept = (ids[i] & 0xff) == (id & 0xff);
            if (!kept) {
                liftedIds[liftedCount] = id;
                liftedX[liftedCount] = prevX[j];
                liftedY[liftedCount] = prevY[j];
                ++liftedCount;
            }
        }
        if (liftedCount && count + liftedCount > TOUCH_FINGERS) {
            unsigned char upReport[1 + TOUCH_FINGERS * 6 + 1] = {3};
            unsigned upSlot = 0;
            for (unsigned j = 0; j < liftedCount && upSlot < TOUCH_FINGERS; ++j, ++upSlot) {
                unsigned char *f = upReport + 1 + upSlot * 6;
                f[0] = 2; // Tip Up + Touch Valid
                f[1] = (unsigned char)(liftedIds[j] & 0xff);
                f[2] = (unsigned char)liftedX[j];
                f[3] = (unsigned char)(liftedX[j] >> 8);
                f[4] = (unsigned char)liftedY[j];
                f[5] = (unsigned char)(liftedY[j] >> 8);
                native_touch_hid_multi[2] = native_touch_hid_multi[2] + 1;
            }
            submitTouchReport(upReport, upSlot);
            liftedCount = 0;
        }

        // DitingSt61yProtocol.h (diting_st61y_event) already normalizes raw ST61Y
        // super-resolution coordinates (0..12200, 0..27120) to 0..65535 on both axes.
        unsigned char report[1 + TOUCH_FINGERS * 6 + 1] = {3};
        unsigned slot = 0;
        int nextIds[TOUCH_FINGERS], nextX[TOUCH_FINGERS], nextY[TOUCH_FINGERS];
        for (unsigned i = 0; i < count && slot < TOUCH_FINGERS; ++i, ++slot) {
            unsigned x = transformCoordinate(xs[i], 65536);
            unsigned y = transformCoordinate(ys[i], 65536);
            unsigned char *f = report + 1 + slot * 6;
            f[0] = 3; // Tip Switch + Touch Valid (confidence)
            f[1] = (unsigned char)(ids[i] & 0xff);
            f[2] = (unsigned char)x;
            f[3] = (unsigned char)(x >> 8);
            f[4] = (unsigned char)y;
            f[5] = (unsigned char)(y >> 8);
            nextIds[slot] = ids[i];
            nextX[slot] = (int)x;
            nextY[slot] = (int)y;
        }
        const unsigned downCount = slot;
        if (downCount >= 2) native_touch_hid_multi[0] = native_touch_hid_multi[0] + 1;
        if (downCount > native_touch_hid_multi[1]) native_touch_hid_multi[1] = downCount;

        for (unsigned j = 0; j < liftedCount && slot < TOUCH_FINGERS; ++j, ++slot) {
            unsigned char *f = report + 1 + slot * 6;
            f[0] = 2; // Tip Up + Touch Valid
            f[1] = (unsigned char)(liftedIds[j] & 0xff);
            f[2] = (unsigned char)liftedX[j];
            f[3] = (unsigned char)(liftedX[j] >> 8);
            f[4] = (unsigned char)liftedY[j];
            f[5] = (unsigned char)(liftedY[j] >> 8);
            native_touch_hid_multi[2] = native_touch_hid_multi[2] + 1;
        }
        for (unsigned j = 0; j < TOUCH_FINGERS; ++j) {
            prevIds[j] = j < downCount ? nextIds[j] : -1;
            if (j < downCount) {
                prevX[j] = nextX[j];
                prevY[j] = nextY[j];
            }
        }
        submitTouchReport(report, slot);
    }

    static void buttonsNotify(void *context, uint32_t mask) {
        NativeHidInput *self = static_cast<NativeHidInput *>(context);
        if (self && self->inputLoop) self->inputLoop->runAction(&NativeHidInput::buttonsAction, self, reinterpret_cast<void *>(uintptr_t(mask)));
    }

    static IOReturn buttonsAction(OSObject *owner, void *mask, void *, void *, void *) {
        NativeHidInput *self = OSDynamicCast(NativeHidInput, owner);
        if (!self) return kIOReturnSuccess;
        self->buttons = static_cast<uint8_t>(uintptr_t(mask) & 0xe);
        if (!self->hasEventDriverCached()) return kIOReturnSuccess;
        unsigned char report[] = {2, static_cast<unsigned char>((self->down ? 1 : 0) | self->buttons)};
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        IOReturn result = buffer ? self->handleReport(buffer, kIOHIDReportTypeInput, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(4, result);
        return kIOReturnSuccess;
    }

    bool hasEventDriverCached() {
        if (!eventDriverFound) {
            IORegistryEntry *driver = eventDriver(this, 4);
            eventDriverFound = driver != nullptr;
        }
        return eventDriverFound;
    }

    static bool hasEventDriver(IORegistryEntry *parent, unsigned depth) { return eventDriver(parent, depth) != nullptr; }

    static IORegistryEntry *eventDriver(IORegistryEntry *parent, unsigned depth) {
        if (!depth) return nullptr;
        OSIterator *it = parent->getChildIterator(gIOServicePlane);
        if (!it) return nullptr;
        IORegistryEntry *found = nullptr;
        while (OSObject *child = it->getNextObject()) {
            IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
            if (!entry) continue;
            found = entry->metaCast("IOHIDEventDriver") ? entry : eventDriver(entry, depth - 1);
            if (found) break;
        }
        it->release();
        return found;
    }

    static void tick(OSObject *owner, IOTimerEventSource *source) {
        NativeHidInput *self = OSDynamicCast(NativeHidInput, owner);
        if (!self) return;
        native_hid_live_checkpoint(self);
        if (self->touchOnly) {
            source->setTimeoutMS(1000);
            return;
        }
        if (!self->down && !hasEventDriver(self, 4)) {
            native_hid_input_observed(3, 0);
            source->setTimeoutMS(1000);
            return;
        }
        native_hid_input_observed(3, 1);
        if (!self->down && !native_hid_input_requested()) {
            source->setTimeoutMS(1000);
            return;
        }
        unsigned char report[] = {2, static_cast<unsigned char>((self->down ? 0 : 1) | self->buttons)};
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        IOReturn result = buffer ? self->handleReport(buffer, kIOHIDReportTypeInput, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down ? 5 : 4, result);
        if (!self->down) {
            self->down = true;
            source->setTimeoutMS(100);
        } else {
            self->down = false;
            source->setTimeoutMS(1000);
        }
    }

public:
    void setTouchOnly() { touchOnly = true; }
    IOWorkLoop *getWorkLoop() const override { return inputLoop; }
    OSString *newTransportString() const override { return OSString::withCString(touchOnly ? "SPI" : "Virtual"); }
    OSString *newProductString() const override { return OSString::withCString(touchOnly ? "Diting ST61Y Touch" : "Diting HID Input"); }

    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if (!out) return kIOReturnBadArgument;
        unsigned char combined[sizeof(descriptor) + sizeof(touchDescriptor)];
        size_t size;
        if (touchOnly) {
            size = sizeof(touchDescriptor);
            memcpy(combined, touchDescriptor, size);
        } else {
            size = sizeof(descriptor);
            memcpy(combined, descriptor, size);
            if (touchEnabled) {
                memcpy(combined + size, touchDescriptor, sizeof(touchDescriptor));
                size += sizeof(touchDescriptor);
            }
        }
        *out = IOBufferMemoryDescriptor::withBytes(combined, size, kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    bool start(IOService *provider) override {
        inputLoop = IOWorkLoop::workLoop();
        if (!inputLoop) return false;
        if (!setProperty("HIDDefaultBehavior", true)) return false;
        unsigned touch = 0;
        touchEnabled = (PE_parse_boot_argn("touch-hid", &touch, sizeof(touch)) && touch == 1) ||
                       (PE_parse_boot_argn("sheng-touch-hid", &touch, sizeof(touch)) && touch == 1);
        if (touchOnly || touchEnabled) {
            setProperty("Built-In", true);
            setProperty("DisplayIntegrated", true);
            setProperty("DigitizerCollectionDispatch", true);
        }
        if (!IOHIDDevice::start(provider)) return false;
        timer = IOTimerEventSource::timerEventSource(this, tick);
        if (!timer || inputLoop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);
            IOHIDDevice::stop(provider);
            return false;
        }
        if (touchOnly || touchEnabled) native_touch_set_sink(&NativeHidInput::touchNotify, this);
        if (!touchOnly) native_buttons_set_sink(&NativeHidInput::buttonsNotify, this);
        setProperty("OfflineInputProbe", true);
        native_hid_input_observed(2, 0);
        timer->setTimeoutMS(1000);
        return true;
    }

    void stop(IOService *provider) override {
        if (touchOnly || touchEnabled) native_touch_set_sink(nullptr, nullptr);
        if (!touchOnly) native_buttons_set_sink(nullptr, nullptr);
        if (timer) {
            timer->cancelTimeout();
            inputLoop->removeEventSource(timer);
            OSSafeReleaseNULL(timer);
        }
        IOHIDDevice::stop(provider);
    }

    void free() override {
        OSSafeReleaseNULL(inputLoop);
        IOHIDDevice::free();
    }
};

OSDefineMetaClassAndStructors(NativeHidInput, IOHIDDevice)

extern "C" IOService *native_hid_input_allocate(void) { return new NativeHidInput; }
extern "C" IOService *native_hid_touch_allocate(void) {
    NativeHidInput *device = new NativeHidInput;
    if (device) device->setTouchOnly();
    return device;
}
