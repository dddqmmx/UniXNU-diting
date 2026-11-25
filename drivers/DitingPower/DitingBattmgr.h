/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Diting PMIC GLINK on "PMIC_RTR_ADSP_APPS". A property request is a
 * 12-byte {owner, type, opcode} header followed by {battery 0, property, value};
 * its response carries {property, value, result}. This allocation-free codec
 * reads measurements, subscribes to notifications, and supports strictly
 * whitelisted, single-in-flight SET writes when explicitly enabled by policy.
 * The Sheng API names and diagnostic field indices are retained for the board
 * aggregate. */
#ifndef DITING_BATTMGR_H
#define DITING_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SHENG_BATTMGR_OWNER 32778u  /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define SHENG_BATTMGR_TYPE_REQUEST 1u
#define SHENG_BATTMGR_TYPE_NOTIFY 2u
#define SHENG_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define SHENG_BATTMGR_OP_NOTIFICATION 0x07u
#define SHENG_BATTMGR_OP_BATTERY_GET 0x30u
#define SHENG_BATTMGR_OP_BATTERY_SET 0x31u
#define SHENG_BATTMGR_OP_USB_GET 0x32u
#define SHENG_BATTMGR_OP_USB_SET 0x33u
#define SHENG_BATTMGR_OP_BOARD_GET 0x50u  /* the board's own properties, same request and answer shape: fuel gauges, charger state */
#define SHENG_BATTMGR_REQUEST_BYTES 24u
#define SHENG_BATTMGR_ENABLE_BYTES 28u
#define SHENG_BATTMGR_NOTIFY_BYTES 16u

/* Strictly whitelisted safe property write limits (verified against diting
 * SM8475 PixelOS qti_battery_charger.c and xiaomi-sm8475-common.dtsi). */
#define SHENG_BATTMGR_PROP_BATT_THERMAL_LEVEL 10u
#define SHENG_BATTMGR_PROP_USB_ICL 5u
#define SHENG_BATTMGR_THERMAL_MAX_LEVEL 3u  /* qcom,thermal-mitigation = <3000000 1500000 1000000 500000> */
#define SHENG_BATTMGR_ICL_MIN_UA 100000u
#define SHENG_BATTMGR_ICL_MAX_UA 3000000u

/* Stable diagnostic indices, not the request order. Unsupported board fields
 * are never requested. There are at most 32 fields: masks have one bit each. */
enum {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge, BmUsbOnline, BmUsbType, BmUsbVoltage,
    BmUsbCurrent, BmTimeToEmpty, BmTimeToFull, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmFull, BmDesign,
    BmCycles,
    /* the charger's state: port type as charger detection saw it, input current limit and the most the port offers (uA), charge type,
     * thermal level, the charge current the charger runs, the board's view of the adapter (type, watts, PD authenticated), battery
     * authentication of both cells, input suspended */
    BmUsbAdapter, BmUsbLimit, BmUsbMax, BmChargeType, BmThermalLevel, BmChargeCurrent, BmRealType, BmPowerMax, BmPdVerified,
    BmAuthentic, BmSlaveAuthentic, BmInputSuspend, BmFields
};

typedef struct {
    int32_t value[BmFields];
    uint32_t valid, fresh;        /* working values and this cycle's replies */
    int32_t completed_value[BmFields];
    uint32_t completed_valid, completed, snapshot_cycle; /* immutable publication */
    uint32_t deferred;            /* optional failed reads: retry on a slow refresh */
    uint8_t cursor, in_flight, enabled, slow_due;
    uint8_t write_pending, write_in_flight, write_opcode, write_property;
    uint32_t write_value;
    uint32_t writes_sent, writes_ok, writes_err;
    int32_t last_write_rc;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored;
    uint32_t refresh_wanted;      /* a notification asked for a new cycle while one ran */
    int last_timeout;             /* the field the last timeout skipped, -1 none */
} ShengBattmgr;

/* Xiaomi battery properties: capacity 0.01%, temperature 0.01 C, voltage uV,
 * current uA (discharge positive), charge counter mAh, full/design uAh,
 * time-to-full minutes and time-to-empty seconds. IOPMPowerSource current is
 * charge positive. No readings from the old dual-gauge board extension are
 * used to infer this phone's capacity. */
typedef struct {
    int present, external, charging, fully_charged, critical;
    int present_known;
    int source_known, status_known; /* current completed cycle, not retained values */
    int percent;                  /* 0..100 */
    int charge_known;             /* percent comes from a reading, not a default */
    int voltage_mv, current_ma;   /* current: positive while the battery takes charge */
    int temperature, temperature_valid;  /* valid: a reading in the range a battery can have */
    int raw_mah, full_mah, design_mah, cycles;
    int time_to_empty_min, time_to_full_min;  /* -1 unknown */
    uint32_t adapter_family;      /* kIOPSFamilyCode value, 0 when no adapter */
    int adapter_mv, adapter_ma, adapter_watts;
    uint32_t sample_cycle;        /* core and optional publication share this ID */
} ShengBatterySnapshot;

enum {BmIgnored = 0, BmAnswer = 1, BmCycleDone = 2, BmNotified = 3, BmEnabled = 4, BmSnapshotReady = 5, BmWriteDone = 6};

void sheng_battmgr_init(ShengBattmgr *b);
/* The notification enable request (sent once the channel is up). */
uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that hardly change. */
void sheng_battmgr_refresh(ShengBattmgr *b, int slow);
/* Queues one whitelisted SET request (thermal level 0..3 or USB ICL uA).
 * Returns 1 if accepted into the single-slot queue, 0 if rejected or busy. */
int sheng_battmgr_queue_write(ShengBattmgr *b, uint8_t opcode, uint8_t property, uint32_t value);
/* Pure hysteresis thermal level selector (temperature in 0.01 C -> level 0..3). */
uint32_t sheng_battmgr_thermal_level(int temp_centic, uint32_t current_level);
/* The next read or queued write; 0 when a request is in flight or nothing is queued. */
uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES]);
/* Advance a timed-out request or a request the transport refused to queue.
 * Both return a Bm* result; a refusal is not an unanswered firmware request. */
int sheng_battmgr_timeout(ShengBattmgr *b);
int sheng_battmgr_send_failed(ShengBattmgr *b);
/* A message from the channel; returns one of the Bm* results. */
int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length);
/* The most recently committed snapshot, unaffected by a following cycle. */
void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
