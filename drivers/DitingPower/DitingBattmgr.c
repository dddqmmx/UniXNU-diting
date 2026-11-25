/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Protocol facts: LineageOS/android_kernel_xiaomi_sm8450, eff4b40407a1b6f1b1ab84554cbb540bcb495175,
 * drivers/power/supply/qti_battery_charger.{h,c}. No vendor driver code or
 * charging policy is imported. See README.md for the field units. */
#include "DitingBattmgr.h"

static const struct {uint8_t opcode, property, slow;} kFields[BmFields] = {
    [BmStatus] = {SHENG_BATTMGR_OP_BATTERY_GET, 0, 0},
    [BmPresent] = {SHENG_BATTMGR_OP_BATTERY_GET, 2, 0},
    [BmCapacity] = {SHENG_BATTMGR_OP_BATTERY_GET, 4, 0},
    [BmVoltage] = {SHENG_BATTMGR_OP_BATTERY_GET, 7, 0},
    [BmCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 9, 0},
    [BmTemperature] = {SHENG_BATTMGR_OP_BATTERY_GET, 13, 0},
    [BmCharge] = {SHENG_BATTMGR_OP_BATTERY_GET, 15, 0},
    [BmUsbOnline] = {SHENG_BATTMGR_OP_USB_GET, 0, 0},
    [BmUsbType] = {SHENG_BATTMGR_OP_USB_GET, 6, 0},
    [BmUsbVoltage] = {SHENG_BATTMGR_OP_USB_GET, 1, 0},
    [BmUsbCurrent] = {SHENG_BATTMGR_OP_USB_GET, 3, 0},
    [BmTimeToEmpty] = {SHENG_BATTMGR_OP_BATTERY_GET, 21, 0},
    [BmTimeToFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 20, 0},
    [BmFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 18, 1},
    [BmDesign] = {SHENG_BATTMGR_OP_BATTERY_GET, 17, 1},
    [BmCycles] = {SHENG_BATTMGR_OP_BATTERY_GET, 16, 1},
    [BmUsbAdapter] = {SHENG_BATTMGR_OP_USB_GET, 7, 0},
    [BmUsbLimit] = {SHENG_BATTMGR_OP_USB_GET, 5, 0},
    [BmUsbMax] = {SHENG_BATTMGR_OP_USB_GET, 4, 0},
    [BmChargeType] = {SHENG_BATTMGR_OP_BATTERY_GET, 3, 0},
    [BmThermalLevel] = {SHENG_BATTMGR_OP_BATTERY_GET, 10, 0},
    [BmChargeCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 12, 0},
    [BmRealType] = {SHENG_BATTMGR_OP_USB_GET, 11, 0},
};

/* Commit the eight core measurements before optional requests. Missing
 * adapter details or lifetime estimates must not hide a real percentage. */
static const uint8_t kReadOrder[] = {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmUsbOnline, BmUsbVoltage,
    BmCharge, BmTimeToEmpty, BmTimeToFull, BmFull, BmDesign, BmCycles, BmUsbAdapter, BmUsbType,
    BmUsbCurrent, BmUsbLimit, BmUsbMax, BmChargeType, BmThermalLevel, BmChargeCurrent, BmRealType,
};
enum {kCoreReads = 8, kReadCount = sizeof kReadOrder / sizeof kReadOrder[0], kNoCycle = 0xff};
_Static_assert(BmFields <= 32, "Battery diagnostic masks are 32 bits");

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void header(uint8_t *out, uint32_t type, uint32_t opcode)
{
    put32(out, SHENG_BATTMGR_OWNER); put32(out + 4, type); put32(out + 8, opcode);
}

void sheng_battmgr_init(ShengBattmgr *b)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->cursor = kNoCycle;
    b->last_timeout = -1;
}

uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES])
{
    header(out, SHENG_BATTMGR_TYPE_NOTIFY, SHENG_BATTMGR_OP_REQUEST_NOTIFICATION);
    /* Match Xiaomi's zero-initialized BC_SET_NOTIFY_REQ payload. */
    for (unsigned i = 12; i < SHENG_BATTMGR_ENABLE_BYTES; ++i) out[i] = 0;
    return SHENG_BATTMGR_ENABLE_BYTES;
}

static void skip_to_wanted(ShengBattmgr *b)
{
    while (b->cursor < kReadCount) {
        const unsigned field = kReadOrder[b->cursor];
        if (b->cursor < kCoreReads || b->slow_due ||
            (!kFields[field].slow && !(b->deferred & (1u << field)))) break;
        b->cursor++;
    }
}

void sheng_battmgr_refresh(ShengBattmgr *b, int slow)
{
    if (b->cursor != kNoCycle) {
        const uint32_t wanted = slow ? 2u : 1u;
        if (wanted > b->refresh_wanted) b->refresh_wanted = wanted;
        return;
    }
    /* A missing optional FULL field must not force a slow pass every 5 s. */
    b->slow_due = slow || !b->cycles_done;
    b->cursor = 0;
    b->fresh = 0;
    b->in_flight = 0;
}

uint32_t sheng_battmgr_thermal_level(int temp_centic, uint32_t current_level)
{
    if (current_level > SHENG_BATTMGR_THERMAL_MAX_LEVEL) current_level = SHENG_BATTMGR_THERMAL_MAX_LEVEL;
    if (temp_centic >= 4500) return 3u;
    if (current_level >= 3u) {
        if (temp_centic >= 4400) return 3u;
        if (temp_centic >= 4100) return 2u;
        if (temp_centic >= 3700) return 1u;
        return 0u;
    }
    if (temp_centic >= 4200) return 2u;
    if (current_level == 2u) {
        if (temp_centic >= 4100) return 2u;
        if (temp_centic >= 3700) return 1u;
        return 0u;
    }
    if (temp_centic >= 3800) return 1u;
    if (current_level == 1u && temp_centic >= 3700) return 1u;
    return 0u;
}

int sheng_battmgr_queue_write(ShengBattmgr *b, uint8_t opcode, uint8_t property, uint32_t value)
{
    if (!b || b->write_pending || b->write_in_flight) return 0;
    const int allow_thermal = opcode == SHENG_BATTMGR_OP_BATTERY_SET &&
        property == SHENG_BATTMGR_PROP_BATT_THERMAL_LEVEL && value <= SHENG_BATTMGR_THERMAL_MAX_LEVEL;
    const int allow_icl = opcode == SHENG_BATTMGR_OP_USB_SET &&
        property == SHENG_BATTMGR_PROP_USB_ICL &&
        value >= SHENG_BATTMGR_ICL_MIN_UA && value <= SHENG_BATTMGR_ICL_MAX_UA;
    if (!allow_thermal && !allow_icl) return 0;
    b->write_opcode = opcode;
    b->write_property = property;
    b->write_value = value;
    b->write_pending = 1;
    return 1;
}

uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight || b->write_in_flight) return 0;
    if (b->write_pending) {
        header(out, SHENG_BATTMGR_TYPE_REQUEST, b->write_opcode);
        put32(out + 12, 0);
        put32(out + 16, b->write_property);
        put32(out + 20, b->write_value);
        b->write_pending = 0;
        b->write_in_flight = 1;
        b->writes_sent++;
        return SHENG_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor == kNoCycle || b->cursor >= kReadCount) return 0;
    const unsigned field = kReadOrder[b->cursor];
    header(out, SHENG_BATTMGR_TYPE_REQUEST, kFields[field].opcode);
    put32(out + 12, 0); put32(out + 16, kFields[field].property); put32(out + 20, 0);
    b->in_flight = 1;
    return SHENG_BATTMGR_REQUEST_BYTES;
}

static void commit(ShengBattmgr *b)
{
    for (unsigned i = 0; i < BmFields; ++i) b->completed_value[i] = b->value[i];
    b->completed_valid = b->valid;
    b->completed = b->fresh;
    b->snapshot_cycle = b->cycles_done + 1;
}

static int advance(ShengBattmgr *b)
{
    const unsigned before = b->cursor;
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
    if (b->cursor >= kReadCount) {
        commit(b);
        b->cycles_done++;
        b->cursor = kNoCycle;
        if (b->refresh_wanted) {
            const int slow = b->refresh_wanted == 2;
            b->refresh_wanted = 0;
            sheng_battmgr_refresh(b, slow);
        }
        return BmCycleDone;
    }
    if (before < kCoreReads && b->cursor >= kCoreReads) {
        commit(b);
        return BmSnapshotReady;
    }
    return BmAnswer;
}

static int failed_request(ShengBattmgr *b, int timeout)
{
    if (b->write_in_flight) {
        b->write_in_flight = 0;
        b->writes_err++;
        if (timeout) {
            b->timeouts++;
            b->last_write_rc = -110;
        } else {
            b->errors++;
            b->last_write_rc = -5;
        }
        return BmWriteDone;
    }
    if (b->cursor == kNoCycle || b->cursor >= kReadCount || !b->in_flight) return BmIgnored;
    const unsigned field = kReadOrder[b->cursor];
    if (timeout) {
        b->timeouts++;
        b->last_timeout = (int)field;
        if (b->cursor >= kCoreReads) b->deferred |= 1u << field;
    } else b->errors++;
    b->valid &= ~(1u << field);
    b->fresh &= ~(1u << field);
    return advance(b);
}

int sheng_battmgr_timeout(ShengBattmgr *b) {return failed_request(b, 1);}
int sheng_battmgr_send_failed(ShengBattmgr *b) {return failed_request(b, 0);}

static int valid_value(unsigned field, int32_t value)
{
    switch (field) {
    case BmStatus: return value >= 0 && value <= 4;
    case BmPresent: case BmUsbOnline: return value == 0 || value == 1;
    case BmCapacity: return value >= 0 && value <= 10000;
    case BmVoltage: return value >= 0 && value <= 20000000;
    case BmUsbVoltage: return value >= 0 && value <= 30000000;
    case BmCurrent: case BmUsbCurrent: return value >= -30000000 && value <= 30000000;
    case BmTemperature: return value >= -4000 && value <= 9000;
    case BmCharge: return value >= 0 && value <= 50000; /* mAh */
    case BmFull: case BmDesign: return value > 0 && value <= 50000000; /* uAh */
    case BmCycles: return value >= 0 && value <= 100000;
    case BmTimeToEmpty: return value >= 0 && value < 100 * 3600; /* seconds */
    case BmTimeToFull: return value >= 0 && value <= 65535 / 60; /* minutes */
    case BmUsbAdapter: return (value >= 0 && value <= 9) || (value >= 0x80 && value <= 0x84);
    case BmUsbType: case BmRealType: return value >= 0 && value <= 0xff;
    case BmUsbLimit: case BmUsbMax: case BmChargeCurrent: return value >= 0 && value <= 30000000;
    case BmChargeType: return value >= 0 && value <= 8;
    case BmThermalLevel: return value >= 0 && value <= 30000000; /* level 0..16 or FCC limit in uA */
    default: return 0;
    }
}

int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length)
{
    if (!data || length < 12 || le32(data) != SHENG_BATTMGR_OWNER) {b->ignored++; return BmIgnored;}
    const uint32_t type = le32(data + 4), opcode = le32(data + 8);
    if (opcode == SHENG_BATTMGR_OP_NOTIFICATION) {
        if (type != SHENG_BATTMGR_TYPE_NOTIFY || length != SHENG_BATTMGR_NOTIFY_BYTES) {
            b->ignored++; return BmIgnored;
        }
        const uint32_t notification = le32(data + 12);
        if (notification != SHENG_BATTMGR_OP_BATTERY_GET && notification != SHENG_BATTMGR_OP_USB_GET &&
            notification != SHENG_BATTMGR_OP_BOARD_GET && notification != 0x80u) {
            b->ignored++; return BmIgnored;
        }
        b->notifications++;
        sheng_battmgr_refresh(b, 0);
        return BmNotified;
    }
    if (opcode == SHENG_BATTMGR_OP_REQUEST_NOTIFICATION &&
        (type == SHENG_BATTMGR_TYPE_REQUEST || type == SHENG_BATTMGR_TYPE_NOTIFY) &&
        (length == 12 || length == SHENG_BATTMGR_ENABLE_BYTES)) {
        b->enabled = 1;
        return BmEnabled;
    }
    if (b->write_in_flight && type == SHENG_BATTMGR_TYPE_REQUEST && length == SHENG_BATTMGR_REQUEST_BYTES &&
        opcode == b->write_opcode && le32(data + 12) == b->write_property) {
        const int32_t rc = (int32_t)le32(data + 20);
        b->write_in_flight = 0;
        b->responses++;
        b->last_write_rc = rc;
        if (rc == 0) {
            b->writes_ok++;
            sheng_battmgr_refresh(b, 0);
        } else {
            b->writes_err++;
            b->errors++;
        }
        return BmWriteDone;
    }
    /* Only exact scalar replies may complete a property read. Never accept
     * a model string, an unrelated owner, or a late reply for another field. */
    if (type != SHENG_BATTMGR_TYPE_REQUEST || length != SHENG_BATTMGR_REQUEST_BYTES ||
        b->cursor == kNoCycle || b->cursor >= kReadCount || !b->in_flight) {
        b->ignored++; return BmIgnored;
    }
    const unsigned field = kReadOrder[b->cursor];
    if (opcode != kFields[field].opcode || le32(data + 12) != kFields[field].property) {
        b->ignored++; return BmIgnored;
    }
    const int32_t value = (int32_t)le32(data + 16);
    b->responses++;
    if (le32(data + 20) || !valid_value(field, value)) {
        b->errors++;
        b->valid &= ~(1u << field);
        b->fresh &= ~(1u << field);
        if (b->cursor >= kCoreReads) b->deferred |= 1u << field;
    } else {
        b->value[field] = value;
        b->valid |= 1u << field;
        b->fresh |= 1u << field;
        b->deferred &= ~(1u << field);
    }
    return advance(b);
}

/* POWER_SUPPLY_USB_TYPE plus Xiaomi's HVDCP extensions to IOPS families. */
static uint32_t family(int type)
{
    switch (type) {
    case 1: return 0xe0004000u;
    case 2: case 9: case 0x80: case 0x81: case 0x82: case 0x84: return 0xe0004004u;
    case 3: return 0xe0004005u;
    case 5: return 0xe0004009u;
    case 6: case 7: case 8: return 0xe000400au;
    default: return 0xe0004007u;
    }
}

void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s)
{
#define HAS(f) ((b->completed_valid >> (f)) & 1u)
#define FRESH(f) (((b->completed >> (f)) & 1u) && HAS(f))
#define V(f) (b->completed_value[(f)])
    for (unsigned i = 0; i < sizeof *s; ++i) ((uint8_t *)s)[i] = 0;
    const int status = FRESH(BmStatus) ? V(BmStatus) : 0;
    s->sample_cycle = b->snapshot_cycle;
    s->present_known = FRESH(BmPresent);
    s->charge_known = FRESH(BmCapacity);
    s->present = s->present_known ? V(BmPresent) != 0 : s->charge_known;
    /* ONLINE can be zero when a full pack stops drawing. Only a fresh VBUS
     * measurement or affirmative ONLINE establishes physical power. */
    s->source_known = FRESH(BmUsbVoltage) || (FRESH(BmUsbOnline) && V(BmUsbOnline) != 0);
    s->status_known = FRESH(BmStatus) && status != 0;
    s->external = (FRESH(BmUsbOnline) && V(BmUsbOnline) != 0) ||
        (FRESH(BmUsbVoltage) && V(BmUsbVoltage) >= 4000000);
    s->percent = s->charge_known ? (V(BmCapacity) + 50) / 100 : 0;
    s->charging = s->present && s->external && status == 1;
    s->fully_charged = s->present && s->external && status == 4;
    s->voltage_mv = HAS(BmVoltage) ? V(BmVoltage) / 1000 : 0;
    s->current_ma = HAS(BmCurrent) ? (int)(-(int64_t)V(BmCurrent) / 1000) : 0;
    {
        const int weak_drain = s->external && HAS(BmCurrent) && s->current_ma < 0 &&
            (s->percent <= 1 || (HAS(BmVoltage) && s->voltage_mv > 0 && s->voltage_mv < 3450));
        s->critical = s->present && s->charge_known && s->source_known &&
            ((!s->external && s->percent <= 2) || weak_drain);
    }
    s->temperature_valid = HAS(BmTemperature);
    s->temperature = s->temperature_valid ? V(BmTemperature) : 0;
    s->raw_mah = HAS(BmCharge) ? V(BmCharge) : 0;
    s->full_mah = HAS(BmFull) ? V(BmFull) / 1000 : 0;
    s->design_mah = HAS(BmDesign) ? V(BmDesign) / 1000 : 0;
    s->cycles = HAS(BmCycles) ? V(BmCycles) : 0;
    s->time_to_empty_min = HAS(BmTimeToEmpty) ? V(BmTimeToEmpty) / 60 : -1;
    s->time_to_full_min = HAS(BmTimeToFull) ? V(BmTimeToFull) : -1;
    if (s->external) {
        int adapter_type = HAS(BmUsbAdapter) && V(BmUsbAdapter) ? V(BmUsbAdapter) : 0;
        if (!adapter_type && HAS(BmUsbType) && V(BmUsbType)) adapter_type = V(BmUsbType);
        if (!adapter_type && HAS(BmRealType) && V(BmRealType)) adapter_type = V(BmRealType);
        s->adapter_family = family(adapter_type);
        s->adapter_mv = HAS(BmUsbVoltage) ? V(BmUsbVoltage) / 1000 : 0;
        s->adapter_ma = HAS(BmUsbMax) ? V(BmUsbMax) / 1000 : 0;
        if (HAS(BmUsbLimit) && (!s->adapter_ma || V(BmUsbLimit) / 1000 < s->adapter_ma))
            s->adapter_ma = V(BmUsbLimit) / 1000;
        s->adapter_watts = (int)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
    }
#undef HAS
#undef FRESH
#undef V
}
