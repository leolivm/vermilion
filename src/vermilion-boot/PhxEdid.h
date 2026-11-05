// SPDX-License-Identifier: MIT
// PhxEdid.h: EDID validation and the "live or static" choice PhxAMDFB makes (M6 step 2). Kernel-free, so the same
// header builds in PhxBringup (PhxDdc.h::readEdid), in PhxAccel (phxfb.cpp) and in a host test of the development
// tree.
//
// Rules: an EDID is 128 or 256 bytes (block 0, plus block 1 when the read got it), starts with the fixed header
// 00 FF FF FF FF FF FF 00, and every block sums to 0 mod 256. Anything else, or a failed read, serves the fallback
// (kPhxEDID) unchanged.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxedid {

constexpr uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

enum : int {
    kLive = 0,              // the live bytes are served
    kWhyReadFailed = 1,     // the read returned an error (its rc is reported separately)
    kWhyLength = 2,         // not 128 or 256 bytes
    kWhyHeader = 3,         // block 0 header wrong
    kWhyChecksum0 = 4,      // block 0 checksum wrong
    kWhyChecksum1 = 5,      // block 1 checksum wrong
    kWhyNoProvider = 6,     // PhxBringup absent or older than b79 (no phxEdidRead platform function)
    kWhyBusy = 7,           // the DDC engine stayed busy (debug.phxedid in use) past the bounded wait
    kWhyDisabled = 8,       // boot-arg -phxedidstatic
};

inline bool blockSumOk(const uint8_t *b) {
    uint8_t s = 0;
    for (int i = 0; i < 128; i++) s = static_cast<uint8_t>(s + b[i]);
    return s == 0;
}

inline bool headerOk(const uint8_t *b) {
    for (int i = 0; i < 8; i++) if (b[i] != kHeader[i]) return false;
    return true;
}

// kLive when `e` is a well-formed EDID of `len` bytes, else the kWhy* reason.
inline int check(const uint8_t *e, size_t len) {
    if (!e || (len != 128 && len != 256)) return kWhyLength;
    if (!headerOk(e)) return kWhyHeader;
    if (!blockSumOk(e)) return kWhyChecksum0;
    if (len == 256 && !blockSumOk(e + 128)) return kWhyChecksum1;
    return kLive;
}

// The bytes to serve. readWhy = kLive when the read itself succeeded, else the kWhy* reason it did not (kept as is).
// Copies the live EDID into out when the read succeeded and the bytes pass check(), else the fallback (at most 256
// bytes). Returns the length copied; *why = kLive or the reason the fallback is served.
inline size_t choose(int readWhy, const uint8_t *live, size_t liveLen, const uint8_t *fallback, size_t fbLen,
                     uint8_t out[256], int *why) {
    int w = readWhy != kLive ? readWhy : check(live, liveLen);
    const uint8_t *src = w == kLive ? live : fallback;
    size_t n = w == kLive ? liveLen : (fbLen > 256 ? 256 : fbLen);
    for (size_t i = 0; i < n; i++) out[i] = src[i];
    if (why) *why = w;
    return n;
}

}  // namespace phxedid
