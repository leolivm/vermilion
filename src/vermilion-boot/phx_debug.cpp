// SPDX-License-Identifier: MIT
// phx_debug.cpp: user-client debug selectors (doDbg*: SMU/DAL, PSP, VRAM, registers, doorbell, sysMem, queues, claim).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// smu_v13_0_0_ppsmc.h; Linux sent PowerUpVcn arg 0 then 0x10000 on this card (a Linux register trace) (WP6 M1)
static constexpr uint64_t kPpsmcPowerUpVcn = 0x2Au, kPpsmcPowerDownVcn = 0x2Bu;
// b71: memory/fabric clock DPM limits (smu_v13_0_0_ppsmc.h SetSoftMinByFreq 0x19 .. GetDpmFreqByIndex 0x1F; argument =
// PPCLK << 16 | MHz or level index), limited to PPCLK_UCLK 2 and PPCLK_FCLK 3 (UCLK switches underflow the display)

IOReturn IOAccelPhxNativeClient::doDbgSmu(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 2, 0, 0) || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] & 0x10000u) {   // b75: DAL mailbox, DALSMC id in the low byte
        const uint32_t id = static_cast<uint32_t>(a->scalarInput[0] & 0xFFFFu), clk = static_cast<uint32_t>(a->scalarInput[1] >> 16);
        switch (id) {
        case 0x02: case 0x03: case 0x04:   // versions
        case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x15:   // deep sleep, displays, audio, FCLK switch, status
            break;
        // b76: 0x14 SetAlwaysWaitDmcubResp hung the PMFW with the VBIOS DMCUB (on the box); 0x0F, 0x12, 0x13 were never
        // sent by Linux on this card and may involve the DMCUB too: all refused by the default case.
        case 0x05: {   // SetDalDramAddrHigh: only the fixed DAL watermarks slot
            const uint64_t slot = owner->vramMcBase() + PHX_VRAM_DALWM_OFF;
            if (!owner->vramMcBase() || a->scalarInput[1] != (slot >> 32))
                return kIOReturnNotPermitted;
            break;
        }
        case 0x06: {   // SetDalDramAddrLow
            const uint64_t slot = owner->vramMcBase() + PHX_VRAM_DALWM_OFF;
            if (!owner->vramMcBase() || a->scalarInput[1] != (slot & 0xFFFFFFFFull))
                return kIOReturnNotPermitted;
            break;
        }
        case 0x08:     // TransferTableDram2Smu: TABLE_WATERMARKS (2) only
            if (a->scalarInput[1] != 2)
                return kIOReturnNotPermitted;
            break;
        case 0x09: case 0x0A: case 0x0B: case 0x0C:   // Set/HardMin/Max, GetDpmFreqByIndex, GetDcModeMax
            if (clk != 2 && clk != 3 && clk != 8 && clk != 9 && clk != 11 && clk != 12)
                return kIOReturnNotPermitted;
            break;
        default:
            return kIOReturnNotPermitted;
        }
        uint32_t resp = 0, after = 0;
        IOReturn r = owner->dalMessage(id, static_cast<uint32_t>(a->scalarInput[1]), &resp, &after);
        a->scalarOutput[0] = resp;
        a->scalarOutput[1] = after;
        return r;
    }
    switch (a->scalarInput[0]) {
    case PHX_PPSMC_TestMessage:
    case PHX_PPSMC_GetSmuVersion:
    case PHX_PPSMC_GetDriverIfVersion:
    case PHX_PPSMC_GetRunningSmuFeaturesLow:
    case PHX_PPSMC_GetRunningSmuFeaturesHigh:
    case PHX_PPSMC_AllowGfxOff:
    case PHX_PPSMC_DisallowGfxOff:
    case PHX_PPSMC_EnableAllSmuFeatures:
    case PHX_PPSMC_TransferTableSmu2Dram:
    case PHX_PPSMC_RunDcBtc:
        break;
    case 0x19u: case 0x1Au: case 0x1Bu: case 0x1Cu: case 0x1Du: case 0x1Eu: case 0x1Fu: {   // b71 clock limits
        const uint64_t clk = a->scalarInput[1] >> 16;
        if (clk != 2 && clk != 3)
            return kIOReturnNotPermitted;
        break;
    }
    case kPpsmcPowerUpVcn:     // smu_v13_0_set_vcn_enable: argument = instance << 16 (WP6 M1)
    case kPpsmcPowerDownVcn:
        if (a->scalarInput[1] != 0 && a->scalarInput[1] != 0x10000u)
            return kIOReturnNotPermitted;
        break;
    case PHX_PPSMC_SetDriverDramAddrHigh:
    case PHX_PPSMC_SetToolsDramAddrHigh: {
        uint64_t lo = owner->vramMcBase() + PHX_VRAM_SMU_OFF;
        if (a->scalarInput[1] != (lo >> 32))
            return kIOReturnNotPermitted;
        break;
    }
    case PHX_PPSMC_SetDriverDramAddrLow:
    case PHX_PPSMC_SetToolsDramAddrLow: {
        uint64_t lo = owner->vramMcBase() + PHX_VRAM_SMU_OFF, hi = lo + PHX_VRAM_SMU_SIZE;
        uint64_t want = (lo & 0xFFFFFFFF00000000ull) | a->scalarInput[1];
        if (!owner->vramMcBase() || want < lo || want >= hi)
            return kIOReturnNotPermitted;
        break;
    }
    default:
        return kIOReturnNotPermitted;
    }
    uint32_t resp = 0, after = 0;
    IOReturn r = owner->smuMessage(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                   &resp, &after);
    a->scalarOutput[0] = resp;
    a->scalarOutput[1] = after;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRing(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRing(static_cast<uint32_t>(a->scalarInput[0]), a->scalarInput[1], out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramRead(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t off = a->scalarInput[0], count = a->scalarInput[1];
    if (count < 1 || count > 256 || off > 0xFFFFFFFFull || a->structureOutputSize != count * 4)
        return kIOReturnBadArgument;
    return owner->vramRead(static_cast<uint32_t>(off), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doDbgPspSubmit(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspSubmit(static_cast<uint32_t>(a->scalarInput[0]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgFwStage(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarOutputCount != 0 || a->structureOutputSize != 0)
        return kIOReturnBadArgument;
    if (a->structureInputDescriptor || !a->structureInput || a->structureInputSize == 0 ||
        a->structureInputSize > 4096 || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->fwStage(static_cast<uint32_t>(a->scalarInput[0]), a->structureInput, a->structureInputSize);
}

IOReturn IOAccelPhxNativeClient::doDbgLoadIpFw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull ||
        a->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->loadIpFw(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                 static_cast<uint32_t>(a->scalarInput[2]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspCtl(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspCtl(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRaw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 7, 4, 0, 0))
        return kIOReturnBadArgument;
    uint32_t w[6];
    for (int i = 0; i < 6; i++)
        w[i] = static_cast<uint32_t>(a->scalarInput[i + 1]);
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRaw(static_cast<uint32_t>(a->scalarInput[0]), w, out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgWriteReg(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 1, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint32_t rb = 0;
    IOReturn r = owner->writeReg(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), &rb);
    a->scalarOutput[0] = rb;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramWrite(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 0, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->vramWrite(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]));
}

IOReturn IOAccelPhxNativeClient::doDbgDoorbell(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->doorbellWrite(a->scalarInput[0], a->scalarInput[1] | (a->scalarInput[2] << 32));
}

IOReturn IOAccelPhxNativeClient::doDbgSysMem(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 2, 0, 0))
        return kIOReturnBadArgument;
    return owner->sysMem(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], &a->scalarOutput[0], &a->scalarOutput[1]);
}

IOReturn IOAccelPhxNativeClient::doDbgAdoptQueue(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->adoptQueue(a->scalarInput);
}

IOReturn IOAccelPhxNativeClient::doDbgClaim(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 0, 0, 0) || a->scalarInput[0] != 1)
        return kIOReturnBadArgument;
    return owner->claimAccel();
}

