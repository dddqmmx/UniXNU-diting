/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Device-controller retirement, after the board has established MMIO power.
 * Synopsys fields and ordering: Linux v6.12 dwc3/gadget.c and core.h.
 * No inherited event buffer or TRB is dereferenced, and no DMA lease is changed.
 */
#ifndef DITING_DWC3_STOP_H
#define DITING_DWC3_STOP_H
#include <stdint.h>
#include "Dwc3Registers.h"

struct DitingDwc3StopResult {
    uint32_t id, gctl, dctl, dsts, endpoints, buffers, acknowledged, commandErrors;
    const char *error;
};

template<class Read, class Write, class Pause>
static DitingDwc3StopResult diting_dwc3_stop_device(Read read, Write write, Pause pause)
{
    DitingDwc3StopResult result = {};
    result.id = read(DWC3_GSNPSID);
    const uint32_t family = result.id & DWC3_GSNPSID_MASK;
    if (family != DWC3_GSNPSID_DWC_USB3 && family != DWC3_GSNPSID_DWC_USB31 &&
        family != DWC3_GSNPSID_DWC_USB31_3331) {
        result.error = "unrecognized controller ID"; return result;
    }
    result.gctl = read(DWC3_GCTL);
    if (DWC3_GCTL_PRTCAP(result.gctl) != DWC3_GCTL_PRTCAP_DEVICE) {
        result.error = "inherited controller is not in device role"; return result;
    }
    result.buffers = (read(DWC3_GHWPARAMS1) >> 15) & 0x3f;
    if (!result.buffers || result.buffers > 32) {
        result.error = "invalid hardware event-buffer count"; return result;
    }
    result.endpoints = read(DWC3_DALEPENA);
    result.dctl = read(DWC3_DCTL);
    result.dsts = read(DWC3_DSTS);
    if (!(result.dsts & DWC3_DSTS_DEVCTRLHLT)) {
        for (uint32_t ep = 0; ep < 32; ++ep) {
            if ((result.endpoints & (1u << ep)) && (read(DWC3_DEPCMD(ep)) & DWC3_DEPCMD_CMDACT)) {
                result.error = "inherited endpoint command still active"; return result;
            }
        }
    }
    for (uint32_t n = 0; n < result.buffers; ++n)
        write(DWC3_GEVNTSIZ(n), read(DWC3_GEVNTSIZ(n)) | (1u << 31));
    if (!(result.dsts & DWC3_DSTS_DEVCTRLHLT)) {
        // As in dwc3_stop_active_transfers, leave EP0/EP1 to controller halt.
        // Their inherited SETUP/DATA phase cannot be known without the old
        // owner's state; ENDTRANSFER in an EP0 SETUP phase is not valid.
        const uint32_t phy = read(DWC3_GUSB2PHYCFG(0));
        write(DWC3_GUSB2PHYCFG(0), phy & ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM));
        // ENDTRANSFER is addressed by the real resource returned by the last
        // START/UPDATETRANSFER, never by a guessed resource or donor endpoint.
        for (uint32_t ep = 2; ep < 32; ++ep) {
            if (!(result.endpoints & (1u << ep))) continue;
            uint32_t command = read(DWC3_DEPCMD(ep));
            const uint32_t kind = command & 0x3f, resource = (command >> 16) & 0x7f;
            if (!resource || (kind != DWC3_DEPCMD_STARTTRANSFER && kind != 7)) continue;
            write(DWC3_DEPCMDPAR0(ep), 0); write(DWC3_DEPCMDPAR1(ep), 0); write(DWC3_DEPCMDPAR2(ep), 0);
            write(DWC3_DEPCMD(ep), DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_CMDACT |
                (1u << 11) | DWC3_DEPCMD_PARAM(resource)); // HIPRI_FORCERM
            // Linux's non-IOC ENDTRANSFER path delays 1 ms. CMDACT polling
            // is not a completion guarantee for every DWC_usb31 revision.
            pause(1); command = read(DWC3_DEPCMD(ep));
            if (!(command & DWC3_DEPCMD_CMDACT) && DWC3_DEPCMD_STATUS(command)) ++result.commandErrors;
        }
    }
    write(DWC3_DCTL, result.dctl & ~(DWC3_DCTL_RUN_STOP | DWC3_DCTL_KEEP_CONNECT));
    for (unsigned ms = 0; ms < 250; ++ms) {
        // Pending events prevent DEVCTRLHLT. Acknowledge the hardware count;
        // the old owner's DMA memory is neither read nor reused here.
        for (uint32_t n = 0; n < result.buffers; ++n) {
            const uint32_t count = read(DWC3_GEVNTCOUNT(n)) & 0xffff;
            if (count & 3) {result.error = "unaligned inherited event count"; return result;}
            if (count) {write(DWC3_GEVNTCOUNT(n), count); result.acknowledged += count;}
        }
        result.dsts = read(DWC3_DSTS);
        if (result.dsts & DWC3_DSTS_DEVCTRLHLT) {
            result.dctl = read(DWC3_DCTL);
            if (result.dctl & DWC3_DCTL_RUN_STOP) {
                result.error = "RUN_STOP clear did not latch"; return result;
            }
            write(DWC3_DEVTEN, 0); write(DWC3_DALEPENA, 0);
            for (uint32_t n = 0; n < result.buffers; ++n) {
                write(DWC3_GEVNTSIZ(n), 1u << 31);
                write(DWC3_GEVNTADRLO(n), 0); write(DWC3_GEVNTADRHI(n), 0);
            }
            return result;
        }
        pause(1);
    }
    result.dctl = read(DWC3_DCTL);
    result.error = "device-controller halt timeout; old DMA retained";
    return result;
}
#endif
