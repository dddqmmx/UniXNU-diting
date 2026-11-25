/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Allocation-free minimal UCSI over PMIC GLINK (owner 32779) client for
 * Diting (SM8475). Protocol verified against PixelOS SM8450 ucsi_glink.c
 * and ucsi.h: 48-byte UCSI v1 buffer, opcodes 0x11 READ_BUF, 0x12 WRITE_BUF,
 * 0x13 USBC_NOTIFY_IND. Read-only role/status inspection (only emits
 * SET_NOTIFICATION_ENABLE, GET_CONNECTOR_STATUS, and ACK_CC_CI). */
#ifndef DITING_UCSI_H
#define DITING_UCSI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DITING_UCSI_OWNER 32779u
#define DITING_UCSI_TYPE_REQ_RESP 1u
#define DITING_UCSI_OP_READ_BUF 0x11u
#define DITING_UCSI_OP_WRITE_BUF 0x12u
#define DITING_UCSI_OP_NOTIFY_IND 0x13u

#define DITING_UCSI_BUF_SIZE 48u
#define DITING_UCSI_READ_REQ_BYTES 12u
#define DITING_UCSI_READ_RESP_BYTES 64u
#define DITING_UCSI_WRITE_REQ_BYTES 64u
#define DITING_UCSI_WRITE_RESP_BYTES 16u
#define DITING_UCSI_NOTIFY_BYTES 24u

/* UCSI buffer offsets */
#define DITING_UCSI_OFF_VERSION 0u
#define DITING_UCSI_OFF_CCI 4u
#define DITING_UCSI_OFF_CONTROL 8u
#define DITING_UCSI_OFF_MESSAGE_IN 16u

/* UCSI commands */
#define DITING_UCSI_CMD_ACK_CC_CI 0x04ull
#define DITING_UCSI_CMD_SET_NOTIFICATION_ENABLE 0x05ull
#define DITING_UCSI_CMD_GET_CONNECTOR_STATUS 0x12ull

#define DITING_UCSI_ACK_CONNECTOR_CHANGE (1ull << 16)
#define DITING_UCSI_ACK_COMMAND_COMPLETE (1ull << 17)
#define DITING_UCSI_ENABLE_NTFY_ALL 0xdbe70000ull
#define DITING_UCSI_CONNECTOR_NUMBER(n) ((uint64_t)(n) << 16)

/* CCI bits */
#define DITING_UCSI_CCI_CONNECTOR(c) (((c) >> 1) & 0x7fu)
#define DITING_UCSI_CCI_LENGTH(c) (((c) >> 8) & 0xffu)
#define DITING_UCSI_CCI_NOT_SUPPORTED (1u << 25)
#define DITING_UCSI_CCI_BUSY (1u << 28)
#define DITING_UCSI_CCI_ACK_COMPLETE (1u << 29)
#define DITING_UCSI_CCI_ERROR (1u << 30)
#define DITING_UCSI_CCI_COMMAND_COMPLETE (1u << 31)

/* Connector status flag values */
enum {
    DitingUcsiPwrOpmodeNone = 0,
    DitingUcsiPwrOpmodeDefault = 1,
    DitingUcsiPwrOpmodeBc = 2,
    DitingUcsiPwrOpmodePd = 3,
    DitingUcsiPwrOpmodeTypeC1_5 = 4,
    DitingUcsiPwrOpmodeTypeC3_0 = 5,
};

enum {
    DitingUcsiPartnerNone = 0,
    DitingUcsiPartnerDfp = 1,
    DitingUcsiPartnerUfp = 2,
    DitingUcsiPartnerCable = 3,
    DitingUcsiPartnerCableUfp = 4,
    DitingUcsiPartnerDebug = 5,
    DitingUcsiPartnerAudio = 6,
};

#define DITING_UCSI_PARTNER_FLAG_USB 1u
#define DITING_UCSI_PARTNER_FLAG_ALT_MODE 2u

typedef struct {
    int valid;
    uint16_t version;
    uint16_t change;
    uint16_t flags;
    uint8_t connected;      /* 0 or 1 */
    uint8_t pwr_opmode;     /* DitingUcsiPwrOpmode* */
    uint8_t pwr_dir;        /* 0 = consumer/sink, 1 = provider/source */
    uint8_t partner_flags;  /* bit0 = USB, bit1 = AltMode */
    uint8_t partner_type;   /* DitingUcsiPartner* */
    uint8_t bc_status;      /* 0..3 */
    uint32_t rdo;           /* PD Request Data Object */
    uint16_t rdo_op_ma;     /* decoded 10 mA units for PD Fixed/Var RDO */
    uint16_t rdo_max_ma;
} DitingUcsiStatus;

enum {
    UcsiIgnored = 0,
    UcsiProgress = 1,
    UcsiNotified = 2,
    UcsiStatusUpdated = 3,
    UcsiError = 4,
};

typedef struct {
    uint8_t state;
    uint8_t enabled;
    uint8_t cmd_notified;
    uint8_t refresh_wanted;
    uint8_t ack_connector_change;
    uint8_t pending_notify_change;
    uint16_t version;
    uint32_t last_cci;
    DitingUcsiStatus status;
    uint32_t updates;
    uint32_t notifications;
    uint32_t responses;
    uint32_t errors;
    uint32_t timeouts;
    uint32_t ignored;
} DitingUcsi;

void diting_ucsi_init(DitingUcsi *u);
/* Starts initial SET_NOTIFICATION_ENABLE + GET_CONNECTOR_STATUS sequence. */
void diting_ucsi_start(DitingUcsi *u);
/* Requests a GET_CONNECTOR_STATUS poll (queued if another command is active). */
void diting_ucsi_refresh(DitingUcsi *u);
/* Builds the next outgoing PMIC GLINK packet (12 or 64 bytes), or returns 0. */
uint32_t diting_ucsi_next(DitingUcsi *u, uint8_t out[DITING_UCSI_WRITE_REQ_BYTES]);
/* Handles an incoming PMIC GLINK packet for owner 32779. */
int diting_ucsi_receive(DitingUcsi *u, const uint8_t *data, uint32_t length);
/* Advances state machine when the current request times out or send fails. */
int diting_ucsi_timeout(DitingUcsi *u);
int diting_ucsi_send_failed(DitingUcsi *u);
/* Copies the latest committed connector status snapshot. */
void diting_ucsi_snapshot(const DitingUcsi *u, DitingUcsiStatus *out);
/* Returns 1 if UCSI explicitly confirms a USB-capable DFP host, 0 if unplugged
 * or non-host / non-USB partner, or -1 if UCSI status is not yet valid. */
int diting_ucsi_status_usb_data_online(const DitingUcsiStatus *st);
/* Returns 1 if UCSI explicitly confirms PD + DFP + USB data capability. */
int diting_ucsi_status_is_pd_usb_host(const DitingUcsiStatus *st);

#ifdef __cplusplus
}
#endif

#endif
