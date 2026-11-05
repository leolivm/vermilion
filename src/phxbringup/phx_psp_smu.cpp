// SPDX-License-Identifier: MIT AND BSD-3-Clause
// phx_psp_smu.cpp: PSP ring and GPCOM commands (firmware load, TMR), SMU and DAL -> PMFW mailboxes.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK): response flag set and status 0.
bool PhxBringup::pspWaitTos(uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs * 10; i++) {
        if ((rreg(PHX_PSP_C2PMSG_64) & PHX_MBOX_TOS_MASK) == PHX_MBOX_TOS_READY)
            return true;
        IODelay(100);
    }
    return false;
}

// Debug selector 101: create / destroy the PSP 13.0.4 KM ring the way Linux psp_v13_0_ring_create / _stop and
// Cobalt64 psp_v14_0.cpp do. The ring lives in VRAM at carveOffset; the CPU writes it through the BAR0
// framebuffer aperture, which is MMIO above system RAM — so the PSP sees the bytes and the macOS heap is never
// touched. The MC address handed to the PSP is fbMcBase + carveOffset.
IOReturn PhxBringup::pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]) {
    if (!mmio || !fbAper || !fbMcBase)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    IOLockLock(smuLock);
    if (op == 1) {
        if (ringMc) {
            r = kIOReturnExclusiveAccess;   // a ring is already live; destroy it first
        } else if ((carveOffset & 0xFFF) || carveOffset < PHX_VRAM_RING_MIN ||
                   carveOffset + PHX_PSP_FENCE_REL + 0x1000 > PHX_VRAM_TMR_OFF ||
                   PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE > fbAperLen) {
            r = kIOReturnBadArgument;        // ring+cmd+fence must sit below the TMR; fw_buf must fit the aperture
        } else {
            volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + carveOffset);
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                nonzero |= ring[i];
            out[2] = nonzero == 0;           // report whether the VRAM region was already clear (informational)
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                ring[i] = 0;                 // zero the ring buffer (Linux memsets ring_mem; safe via BAR0)
            if (!pspWaitTos(2000)) {
                r = kIOReturnNotReady;       // SOS not ready to accept the ring
            } else {
                ringMc = fbMcBase + carveOffset;
                wreg(PHX_PSP_C2PMSG_69, static_cast<uint32_t>(ringMc));
                wreg(PHX_PSP_C2PMSG_70, static_cast<uint32_t>(ringMc >> 32));
                wreg(PHX_PSP_C2PMSG_71, PHX_PSP_RING_SIZE);
                wreg(PHX_PSP_C2PMSG_64, PHX_PSP_RING_TYPE_KM << 16);
                IOSleep(20);
                if (!pspWaitTos(2000)) {
                    r = kIOReturnTimeout;    // raw C2PMSG_64 (with PSP status in bits 15:0) returned in out[0]
                    ringMc = 0;              // not established: allow destroy + retry
                } else {
                    // Ring is live. Lay the GPCOM command + fence buffers just above it and zero them,
                    // then reset the doorbell so wptr agrees with our slot 0 (Linux psp_v13_0_ring_create).
                    ringOff = carveOffset;
                    cmdOff = carveOffset + PHX_PSP_CMD_REL;
                    fenceOff = carveOffset + PHX_PSP_FENCE_REL;
                    cmdMc = fbMcBase + cmdOff;
                    fenceMc = fbMcBase + fenceOff;
                    fenceCounter = 0;
                    tmrOff = PHX_VRAM_TMR_OFF;
                    tmrMc = fbMcBase + tmrOff;
                    tmrSize = 0;
                    tmrSetUp = false;
                    fwBufOff = PHX_VRAM_FWBUF_OFF;
                    fwBufMc = fbMcBase + fwBufOff;
                    fwBufSize = PHX_VRAM_FWBUF_SIZE;
                    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
                    for (uint32_t i = 0; i < 0x1000 / 4; i++) {
                        cmd[i] = 0;
                        fen[i] = 0;
                    }
                    wreg(PHX_PSP_C2PMSG_67, 0);
                }
            }
        }
    } else if (op == 2) {
        wreg(PHX_PSP_C2PMSG_64, PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS);
        IOSleep(20);
        if (!pspWaitTos(2000))
            r = kIOReturnTimeout;
        clearRing();
    } else if (op != 3) {
        r = kIOReturnBadArgument;
    }
    out[0] = rreg(PHX_PSP_C2PMSG_64);
    out[1] = ringMc;
    out[3] = rreg(PHX_PSP_C2PMSG_67);
    IOLockUnlock(smuLock);
    return r;
}

// Core GPCOM submit (caller holds smuLock and a ring exists): write a prepared command — cmd_id plus up to
// nWords union dwords at +28 — into the VRAM command buffer, ring the doorbell, wait for the PSP's fence and
// read the response. Ports Linux psp_cmd_submit_buf / Cobalt64 psp_ring_cmd_submit.
IOReturn PhxBringup::pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]) {
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(base + cmdOff);
    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(base + fenceOff);

    // Command buffer: zero it, set cmd_id (buf_size/buf_version stay 0, as upstream does), then the union.
    for (uint32_t i = 0; i < PHX_PSP_CMD_BUF_SIZE / 4; i++)
        cmd[i] = 0;
    cmd[PHX_PSP_CMD_HDR_CMD_ID_OFFSET / 4] = cmdId;
    for (uint32_t i = 0; i < nWords && i < 14; i++)
        cmd[PHX_PSP_GFX_CMD_UNION_OFFSET / 4 + i] = unionWords[i];

    uint32_t fence = ++fenceCounter;
    fen[0] = 0;

    // Ring frame at the slot the current wptr points to.
    uint32_t wptr = rreg(PHX_PSP_C2PMSG_67);
    const uint32_t ringSizeDw = PHX_PSP_RING_SIZE / 4;         // 1024
    const uint32_t frameSizeDw = PHX_PSP_GFX_FRAME_SIZE / 4;   // 16
    uint32_t slot = (wptr % ringSizeDw == 0) ? 0 : wptr / frameSizeDw;
    if (slot >= PHX_PSP_RING_SIZE / PHX_PSP_GFX_FRAME_SIZE)
        return kIOReturnInternalError;
    PhxGfxRbFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(cmdMc);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(cmdMc >> 32);
    frame.fence_addr_lo = static_cast<uint32_t>(fenceMc);
    frame.fence_addr_hi = static_cast<uint32_t>(fenceMc >> 32);
    frame.fence_value = fence;
    volatile uint32_t *slotp =
        reinterpret_cast<volatile uint32_t *>(base + ringOff + static_cast<uint64_t>(slot) * PHX_PSP_GFX_FRAME_SIZE);
    const uint32_t *src = reinterpret_cast<const uint32_t *>(&frame);
    for (uint32_t i = 0; i < PHX_PSP_GFX_FRAME_SIZE / 4; i++)
        slotp[i] = src[i];

    // Drain CPU write buffers, flush the host data path, then ring the doorbell.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wptr = (wptr + frameSizeDw) % ringSizeDw;
    wreg(PHX_PSP_C2PMSG_67, wptr);

    // Wait for the PSP to write the fence value into VRAM (BAR0 read bypasses the CPU cache).
    uint32_t observed = 0, us = 0;
    const uint32_t budgetUs = 5000000;
    while (us < budgetUs) {
        observed = fen[0];
        if (observed == fence)
            break;
        IOSleep(1);
        us += 1000;
    }
    uint32_t status = cmd[PHX_PSP_GFX_RESP_OFFSET / 4];
    // Always succeed at the transport level so IOKit delivers the outputs; the PSP result is in out[].
    // out[1] = elapsed us (≈budget on a fence timeout), so userspace distinguishes timeout from a status code.
    out[0] = (observed == fence) ? status : 0xFFFFFFFFu;  // 0xFFFFFFFF = fence never arrived
    out[1] = us;
    out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 8) / 4];   // resp.fw_addr_lo / session_id
    out[3] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 12) / 4];  // resp.fw_addr_hi
    return kIOReturnSuccess;
}

// Selector 103: no-payload query command (FB_FW_RESERV_ADDR/EXT only) — validates the ring without touching GPU power.
IOReturn PhxBringup::pspSubmit(uint32_t cmdId, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR && cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR)
        return kIOReturnNotPermitted;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, nullptr, 0, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Selector 104: stage a chunk of firmware bytes into the VRAM fw_buf through BAR0 (no submit).
IOReturn PhxBringup::fwStage(uint32_t dstOffset, const void *bytes, uint32_t len) {
    if (!fbAper || !fwBufSize)
        return kIOReturnNotReady;
    if ((dstOffset & 3) || static_cast<uint64_t>(dstOffset) + len > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress() + fwBufOff + dstOffset);
    const uint8_t *s = static_cast<const uint8_t *>(bytes);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    if (i < len) {
        uint32_t v = 0;
        memcpy(&v, s + i, len - i);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    IOLockUnlock(smuLock);
    return kIOReturnSuccess;
}

// Selector 105: submit GFX_CMD_ID_LOAD_IP_FW for the firmware already staged in fw_buf at fwBufOffset.
IOReturn PhxBringup::loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if ((fwBufOffset & 3) || size == 0 || static_cast<uint64_t>(fwBufOffset) + size > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else {
        uint64_t mc = fwBufMc + fwBufOffset;
        uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, fwType};
        r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, out);
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 106: op 1 = SETUP_TMR (driver-managed TMR, Phoenix psp_13_0_4 is not in psp_skip_tmr), op 2 = AUTOLOAD_RLC.
IOReturn PhxBringup::pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else if (op == 1) {
        uint32_t size = arg ? arg : 0x400000u;
        if (size > PHX_VRAM_TMR_CAP)
            size = PHX_VRAM_TMR_CAP;
        tmrSize = size;
        // psp_gfx_cmd_setup_tmr (Linux psp_prep_tmr_cmd_buf): buf_phy_addr = TMR MC address, buf_size,
        // bitfield(virt_phy_addr=1 => 0x2), system_phy_addr = the REAL system-physical address of the TMR
        // (amdgpu_gmc_vram_pa = mc - vram_start + vram_base_offset = fbPhys + tmrOff on this UMA part).
        uint64_t tmrPa = fbPhys + tmrOff;
        uint32_t u[6] = {static_cast<uint32_t>(tmrMc), static_cast<uint32_t>(tmrMc >> 32), size, 0x2u,
                         static_cast<uint32_t>(tmrPa), static_cast<uint32_t>(tmrPa >> 32)};
        r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
        if (r == kIOReturnSuccess)
            tmrSetUp = true;
    } else if (op == 2) {
        r = pspRunCmd(PHX_PSP_GFX_CMD_AUTOLOAD_RLC, nullptr, 0, out);
    } else {
        r = kIOReturnBadArgument;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 107: submit an arbitrary GPCOM command with six caller-supplied union dwords. For bring-up
// experiments (SETUP_TMR / LOAD_TOC parameter sweeps) without rebuilding the kext.
IOReturn PhxBringup::pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, words, 6, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Polls the SMU response register for up to 2 s (Linux: usec_timeout * 20); 0 means no answer.
uint32_t PhxBringup::smuPoll() const {
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t r = rreg(PHX_SMU_RESP_REG);
        if (r)
            return r;
        IODelay(10);
    }
    return 0;
}

// b75: one DAL -> PMFW mailbox transaction, the sequence of Linux dcn32_smu_send_msg_with_param(): RESP 0, ARG, MSG,
// then poll RESP (1 = OK); its own registers, so it never interleaves with the driver mailbox; serialized by smuLock.
static constexpr uint32_t kDalMsgReg = 0x1628A, kDalArgReg = 0x16273, kDalRespReg = 0x16274;
IOReturn PhxBringup::dalMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    if (gDalDead) {   // b76: never talk to a PMFW that already stopped answering (it does not recover without a power-off)
        IOLockUnlock(smuLock);
        return kIOReturnNotResponding;
    }
    // b76: Linux dcn32_smu_send_msg_with_param waits for RESP != 0 first; a zero RESP = a message still in flight.
    // The first DAL message of the boot skips it: nothing may have used the DAL mailbox since the VBIOS.
    static bool dalUsed = false;
    if (dalUsed && rreg(kDalRespReg) == 0) {
        OSAddAtomic64(1, &gDalBusy);
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    dalUsed = true;
    wreg(kDalRespReg, 0);
    wreg(kDalArgReg, arg);
    wreg(kDalMsgReg, msg);
    OSAddAtomic64(1, &gDalSent);
    gDalLastMsg = msg; gDalLastArg = arg;
    uint32_t r = 0;
    for (uint32_t i = 0; i < 200000 && !r; i++) {
        r = rreg(kDalRespReg);
        if (!r)
            IODelay(10);
    }
    *resp = r;
    gDalLastResp = r;
    *argAfter = rreg(kDalArgReg);
    if (!r) {
        OSAddAtomic64(1, &gDalTimeouts);
        gDalDead = 1;
    } else {
        OSAddAtomic64(1, r == 1 ? &gDalOk : &gDalNotOk);
    }
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

// One SMU 13.0.4 mailbox transaction, the sequence of Linux smu_msg_v1_send_msg().
IOReturn PhxBringup::smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    if (gDalDead) {   // b76: the PMFW stopped answering a DAL message; it serves both mailboxes
        IOLockUnlock(smuLock);
        return kIOReturnNotResponding;
    }
    // The first message of the boot skips the pre-poll (Linux SMU_FW_INIT): nothing may have been sent yet.
    if (smuUsed && smuPoll() == 0) {
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    smuUsed = true;
    wreg(PHX_SMU_RESP_REG, 0);
    wreg(PHX_SMU_ARG_REG, arg);
    wreg(PHX_SMU_MSG_REG, msg);
    uint32_t r = smuPoll();
    *resp = r;
    *argAfter = rreg(PHX_SMU_ARG_REG);
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

