/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Minimal allocation-free UCSI over PMIC GLINK (owner 32779) state machine. */
#include "DitingUcsi.h"

enum {
    UcsiIdle = 0,
    UcsiSendEnable,
    UcsiWaitEnableWrite,
    UcsiWaitEnableNotify,
    UcsiSendEnableRead,
    UcsiWaitEnableRead,
    UcsiSendEnableAck,
    UcsiWaitEnableAck,
    UcsiSendStatus,
    UcsiWaitStatusWrite,
    UcsiWaitStatusNotify,
    UcsiSendStatusRead,
    UcsiWaitStatusRead,
    UcsiSendStatusAck,
    UcsiWaitStatusAck,
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_hdr(uint8_t *out, uint32_t opcode)
{
    put32(out, DITING_UCSI_OWNER);
    put32(out + 4, DITING_UCSI_TYPE_REQ_RESP);
    put32(out + 8, opcode);
}

static uint32_t build_write_cmd(uint8_t out[DITING_UCSI_WRITE_REQ_BYTES], uint64_t control)
{
    for (unsigned i = 0; i < DITING_UCSI_WRITE_REQ_BYTES; ++i) out[i] = 0;
    write_hdr(out, DITING_UCSI_OP_WRITE_BUF);
    put64(out + 12u + DITING_UCSI_OFF_CONTROL, control);
    return DITING_UCSI_WRITE_REQ_BYTES;
}

static uint32_t build_read_req(uint8_t out[DITING_UCSI_WRITE_REQ_BYTES])
{
    write_hdr(out, DITING_UCSI_OP_READ_BUF);
    return DITING_UCSI_READ_REQ_BYTES;
}

static void finish_cycle(DitingUcsi *u)
{
    u->cmd_notified = 0;
    u->ack_connector_change = 0;
    if (u->refresh_wanted) {
        u->refresh_wanted = 0;
        u->state = UcsiSendStatus;
    } else {
        u->state = UcsiIdle;
    }
}

static void parse_connector_status(DitingUcsi *u, const uint8_t *buf)
{
    const uint8_t *msg = buf + DITING_UCSI_OFF_MESSAGE_IN;
    const uint16_t change = le16(msg + 0);
    const uint16_t flags = le16(msg + 2);
    const uint32_t rdo = le32(msg + 4);
    const uint8_t pwr_status = msg[8];

    DitingUcsiStatus st;
    for (unsigned i = 0; i < sizeof st; ++i) ((uint8_t *)&st)[i] = 0;
    st.valid = 1;
    st.version = le16(buf + DITING_UCSI_OFF_VERSION);
    st.change = change;
    st.flags = flags;
    st.connected = (uint8_t)((flags >> 3) & 1u);
    st.pwr_opmode = (uint8_t)(flags & 0x7u);
    st.pwr_dir = (uint8_t)((flags >> 4) & 1u);
    st.partner_flags = (uint8_t)((flags >> 5) & 0xffu);
    st.partner_type = (uint8_t)((flags >> 13) & 0x7u);
    st.bc_status = (uint8_t)(pwr_status & 0x7u);
    st.rdo = rdo;
    if (st.connected && st.pwr_opmode == DitingUcsiPwrOpmodePd && rdo != 0) {
        /* USB PD Fixed/Variable Supply RDO: bits[19:10] operating current, bits[9:0] max current (10 mA units). */
        st.rdo_op_ma = (uint16_t)(((rdo >> 10) & 0x3ffu) * 10u);
        st.rdo_max_ma = (uint16_t)((rdo & 0x3ffu) * 10u);
    }
    u->version = st.version;
    u->status = st;
    u->updates++;
    if (change || u->pending_notify_change) {
        u->ack_connector_change = 1;
        u->pending_notify_change = 0;
    }
}

void diting_ucsi_init(DitingUcsi *u)
{
    if (!u) return;
    for (unsigned i = 0; i < sizeof *u; ++i) ((uint8_t *)u)[i] = 0;
    u->state = UcsiIdle;
}

void diting_ucsi_start(DitingUcsi *u)
{
    if (!u) return;
    u->cmd_notified = 0;
    u->ack_connector_change = 0;
    u->pending_notify_change = 0;
    u->refresh_wanted = 1;
    u->state = UcsiSendEnable;
}

void diting_ucsi_refresh(DitingUcsi *u)
{
    if (!u) return;
    if (u->state == UcsiIdle) {
        u->cmd_notified = 0;
        u->ack_connector_change = 0;
        u->state = u->enabled ? UcsiSendStatus : UcsiSendEnable;
    } else {
        u->refresh_wanted = 1;
    }
}

uint32_t diting_ucsi_next(DitingUcsi *u, uint8_t out[DITING_UCSI_WRITE_REQ_BYTES])
{
    if (!u || !out) return 0;
    switch (u->state) {
    case UcsiSendEnable:
        u->cmd_notified = 0;
        u->state = UcsiWaitEnableWrite;
        return build_write_cmd(out, DITING_UCSI_CMD_SET_NOTIFICATION_ENABLE | DITING_UCSI_ENABLE_NTFY_ALL);
    case UcsiSendEnableRead:
        u->state = UcsiWaitEnableRead;
        return build_read_req(out);
    case UcsiSendEnableAck:
        u->state = UcsiWaitEnableAck;
        return build_write_cmd(out, DITING_UCSI_CMD_ACK_CC_CI | DITING_UCSI_ACK_COMMAND_COMPLETE);
    case UcsiSendStatus:
        u->cmd_notified = 0;
        u->ack_connector_change = 0;
        u->state = UcsiWaitStatusWrite;
        return build_write_cmd(out, DITING_UCSI_CMD_GET_CONNECTOR_STATUS | DITING_UCSI_CONNECTOR_NUMBER(1));
    case UcsiSendStatusRead:
        u->state = UcsiWaitStatusRead;
        return build_read_req(out);
    case UcsiSendStatusAck: {
        uint64_t cmd = DITING_UCSI_CMD_ACK_CC_CI | DITING_UCSI_ACK_COMMAND_COMPLETE;
        if (u->ack_connector_change) cmd |= DITING_UCSI_ACK_CONNECTOR_CHANGE;
        u->state = UcsiWaitStatusAck;
        return build_write_cmd(out, cmd);
    }
    default:
        return 0;
    }
}

int diting_ucsi_receive(DitingUcsi *u, const uint8_t *data, uint32_t length)
{
    if (!u || !data || length < 12u || le32(data) != DITING_UCSI_OWNER) {
        if (u) u->ignored++;
        return UcsiIgnored;
    }
    const uint32_t opcode = le32(data + 8);

    if (opcode == DITING_UCSI_OP_NOTIFY_IND) {
        if (length != DITING_UCSI_NOTIFY_BYTES) {
            u->ignored++;
            return UcsiIgnored;
        }
        const uint32_t cci = le32(data + 12);
        u->last_cci = cci;
        u->notifications++;
        if (DITING_UCSI_CCI_CONNECTOR(cci) != 0) {
            u->pending_notify_change = 1;
            u->refresh_wanted = 1;
            if (u->state == UcsiIdle) {
                u->refresh_wanted = 0;
                u->state = u->enabled ? UcsiSendStatus : UcsiSendEnable;
            }
        }
        if (cci & (DITING_UCSI_CCI_COMMAND_COMPLETE | DITING_UCSI_CCI_ACK_COMPLETE)) {
            if (u->state == UcsiWaitEnableWrite || u->state == UcsiWaitStatusWrite) {
                u->cmd_notified = 1;
            } else if (u->state == UcsiWaitEnableNotify) {
                u->state = UcsiSendEnableRead;
                return UcsiProgress;
            } else if (u->state == UcsiWaitStatusNotify) {
                u->state = UcsiSendStatusRead;
                return UcsiProgress;
            }
        }
        return UcsiNotified;
    }

    if (opcode == DITING_UCSI_OP_WRITE_BUF) {
        if (length != DITING_UCSI_WRITE_RESP_BYTES) {
            u->ignored++;
            return UcsiIgnored;
        }
        const uint32_t ret_code = le32(data + 12);
        u->responses++;
        if (ret_code != 0) {
            u->errors++;
            finish_cycle(u);
            return UcsiError;
        }
        if (u->state == UcsiWaitEnableWrite) {
            u->state = u->cmd_notified ? UcsiSendEnableRead : UcsiWaitEnableNotify;
            return UcsiProgress;
        }
        if (u->state == UcsiWaitEnableAck) {
            u->enabled = 1;
            u->refresh_wanted = 0;
            u->state = UcsiSendStatus;
            return UcsiProgress;
        }
        if (u->state == UcsiWaitStatusWrite) {
            u->state = u->cmd_notified ? UcsiSendStatusRead : UcsiWaitStatusNotify;
            return UcsiProgress;
        }
        if (u->state == UcsiWaitStatusAck) {
            finish_cycle(u);
            return UcsiProgress;
        }
        u->ignored++;
        return UcsiIgnored;
    }

    if (opcode == DITING_UCSI_OP_READ_BUF) {
        if (length != DITING_UCSI_READ_RESP_BYTES) {
            u->ignored++;
            return UcsiIgnored;
        }
        const uint8_t *buf = data + 12;
        const uint32_t ret_code = le32(data + 12 + DITING_UCSI_BUF_SIZE);
        u->responses++;
        if (ret_code != 0) {
            u->errors++;
            finish_cycle(u);
            return UcsiError;
        }
        const uint32_t cci = le32(buf + DITING_UCSI_OFF_CCI);
        u->version = le16(buf + DITING_UCSI_OFF_VERSION);
        u->last_cci = cci;
        if (u->state == UcsiWaitEnableRead) {
            u->enabled = 1;
            if (cci & DITING_UCSI_CCI_COMMAND_COMPLETE) {
                u->state = UcsiSendEnableAck;
            } else {
                u->refresh_wanted = 0;
                u->state = UcsiSendStatus;
            }
            return UcsiProgress;
        }
        if (u->state == UcsiWaitStatusRead) {
            int updated = 0;
            if ((cci & DITING_UCSI_CCI_COMMAND_COMPLETE) &&
                !(cci & (DITING_UCSI_CCI_ERROR | DITING_UCSI_CCI_NOT_SUPPORTED | DITING_UCSI_CCI_BUSY))) {
                parse_connector_status(u, buf);
                updated = 1;
            } else if (cci & (DITING_UCSI_CCI_ERROR | DITING_UCSI_CCI_NOT_SUPPORTED)) {
                u->errors++;
            }
            if (cci & DITING_UCSI_CCI_COMMAND_COMPLETE) {
                u->state = UcsiSendStatusAck;
            } else {
                finish_cycle(u);
            }
            return updated ? UcsiStatusUpdated : UcsiProgress;
        }
        u->ignored++;
        return UcsiIgnored;
    }

    u->ignored++;
    return UcsiIgnored;
}

int diting_ucsi_timeout(DitingUcsi *u)
{
    if (!u || u->state == UcsiIdle) return UcsiIgnored;
    /* Some PPMs do not emit NOTIFY_IND for SET_NOTIFICATION_ENABLE or quiet
     * polls; fall through to READ_BUF_REQ before declaring a hard timeout. */
    if (u->state == UcsiWaitEnableNotify) {
        u->state = UcsiSendEnableRead;
        return UcsiProgress;
    }
    if (u->state == UcsiWaitStatusNotify) {
        u->state = UcsiSendStatusRead;
        return UcsiProgress;
    }
    u->timeouts++;
    finish_cycle(u);
    return UcsiError;
}

int diting_ucsi_send_failed(DitingUcsi *u)
{
    if (!u || u->state == UcsiIdle) return UcsiIgnored;
    u->errors++;
    finish_cycle(u);
    return UcsiError;
}

void diting_ucsi_snapshot(const DitingUcsi *u, DitingUcsiStatus *out)
{
    if (!out) return;
    for (unsigned i = 0; i < sizeof *out; ++i) ((uint8_t *)out)[i] = 0;
    if (!u) return;
    *out = u->status;
}

int diting_ucsi_status_is_pd_usb_host(const DitingUcsiStatus *st)
{
    if (!st || !st->valid || !st->connected) return 0;
    return st->pwr_opmode == DitingUcsiPwrOpmodePd &&
           st->partner_type == DitingUcsiPartnerDfp &&
           (st->partner_flags & DITING_UCSI_PARTNER_FLAG_USB) != 0;
}

int diting_ucsi_status_usb_data_online(const DitingUcsiStatus *st)
{
    if (!st || !st->valid) return -1;
    if (!st->connected) return 0;
    if (st->partner_type != DitingUcsiPartnerNone && st->partner_type != DitingUcsiPartnerDfp) return 0;
    if (st->partner_type == DitingUcsiPartnerDfp && (st->partner_flags & DITING_UCSI_PARTNER_FLAG_USB)) return 1;
    if (st->pwr_opmode == DitingUcsiPwrOpmodePd) {
        /* Negotiated USB-PD partner without USB communications flag -> pure PD charger. */
        return 0;
    }
    /* Default/BC and non-PD Type-C 1.5A/3.0A ports do not report PD USB-comms
     * capability in UCSI; defer data-host vs charger classification to Battmgr APSD. */
    return -1;
}
