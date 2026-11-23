// SPDX-License-Identifier: MIT
// faultdef_test.cpp: host test of the GFX hub fault default sequence (src/vermilion-boot/PhxFaultDef.h, kext b126,
// debug.phxfaultdef). Runs the exact header the kext compiles against a register recorder: every read and write in
// order, with knobs for a DEFAULT_ADDR that does not hold its value, a CNTL that ignores writes and an engine 17 ack
// that never comes. Built and run by faultdef_test.sh (also against mutated copies of the header).
#include "PhxFaultDef.h"

#include <cstdio>
#include <map>
#include <vector>

using namespace phxfd;

static int gPass, gFail;
static void check(const char *name, bool ok) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? gPass : gFail)++;
}

struct Access { bool write; uint32_t reg, val; };

struct Hub {
    std::map<uint32_t, uint32_t> regs;
    std::vector<Access> log;
    uint32_t defLoKeep = 0xFFFFFFFFu;   // bits of DEFAULT_ADDR_LO32 that hold a written value
    bool cntlIgnoresWrites = false;
    bool ackNever = false;
    uint64_t delayed = 0;
    Hub() {
        regs[kRegCntl] = kCntlBoot;
        regs[kRegDefLo] = kDefLoBoot;
        regs[kRegDefHi] = kDefHiBoot;
        regs[kRegInvAck] = 0;
    }
    uint32_t rd(uint32_t r) {
        uint32_t v = r == kRegInvAck ? (ackNever ? 0u : 1u) : regs[r];
        log.push_back({false, r, v});
        return v;
    }
    void wr(uint32_t r, uint32_t v) {
        log.push_back({true, r, v});
        if (r == kRegCntl && cntlIgnoresWrites) return;
        if (r == kRegDefLo) v &= defLoKeep;
        regs[r] = v;
    }
    void delayUs(uint32_t us) { delayed += us; }
    std::vector<Access> writes() const {
        std::vector<Access> w;
        for (const Access &a : log) if (a.write) w.push_back(a);
        return w;
    }
    int firstWrite(uint32_t r) const {
        int i = 0;
        for (const Access &a : log) { if (a.write && a.reg == r) return i; i++; }
        return -1;
    }
    bool onlyAllowed() const {   // CNTL, DEFAULT_ADDR, engine 17 req/ack; nothing on MMHUB (0x1A000-0x1AFFF)
        for (const Access &a : log) {
            if (a.reg >= 0x1A000 && a.reg <= 0x1AFFF) return false;
            if (a.reg != kRegCntl && a.reg != kRegDefLo && a.reg != kRegDefHi && a.reg != kRegInvReq && a.reg != kRegInvAck)
                return false;
        }
        return true;
    }
    bool noSemaphore() const {
        for (const Access &a : log) if (a.reg >= 0x1A751 && a.reg <= 0x1A762) return false;
        return true;
    }
};

int main() {
    {   // E1: enable from mesboot's state, a page below 4 GiB
        Hub h; uint32_t us = 0; bool acked = false;
        const uint64_t pa = 0x0000000123456000ull;
        const int r = enable(h, pa, &us, &acked);
        const std::vector<Access> w = h.writes();
        check("E1 enable returns ok, flush acked", r == kOk && acked);
        check("E1 CNTL 0x2 -> 0x1FFE (eleven ENABLE_DEFAULT bits, no interrupt, no CRASH)", h.regs[kRegCntl] == 0x1FFEu && kCntlOn == 0x1FFEu);
        check("E1 DEFAULT_ADDR = pa >> 12 / pa >> 44", h.regs[kRegDefLo] == 0x00123456u && h.regs[kRegDefHi] == 0);
        check("E1 write order: DEFAULT_ADDR_LO, DEFAULT_ADDR_HI, CNTL, engine 17 request",
              w.size() == 4 && w[0].reg == kRegDefLo && w[1].reg == kRegDefHi && w[2].reg == kRegCntl && w[2].val == 0x1FFEu &&
              w[3].reg == kRegInvReq && w[3].val == 0x00F80001u);
        check("E1 both addresses read back before CNTL is written", [&] {
            int lo = -1, hi = -1, i = 0;
            for (const Access &a : h.log) { if (!a.write && a.reg == kRegDefLo) lo = i; if (!a.write && a.reg == kRegDefHi) hi = i; i++; }
            return lo >= 0 && hi >= 0 && lo < h.firstWrite(kRegCntl) && hi < h.firstWrite(kRegCntl);
        }());
        check("E1 only CNTL / DEFAULT_ADDR / engine 17 touched, never MMHUB", h.onlyAllowed() && h.noSemaphore());
    }
    {   // E2: a page above 2^44 (DEFAULT_ADDR_HI32 = address bits 44-47)
        Hub h; uint32_t us = 0; bool acked = false;
        const int r = enable(h, 0x0000712345678000ull, &us, &acked);
        check("E2 high page: LO = bits 12-43, HI = bits 44-47", r == kOk && h.regs[kRegDefLo] == 0x12345678u && h.regs[kRegDefHi] == 0x7u);
    }
    {   // R1-R4: refusals write nothing
        struct { const char *name; uint64_t pa; uint32_t cntl; int want; } cases[] = {
            {"R1 page allocation failed (pa 0): refused, no register written", 0, kCntlBoot, kNoPage},
            {"R2 unaligned page: refused, no register written", 0x123456010ull, kCntlBoot, kUnaligned},
            {"R3 page above 48 bits: refused, no register written", 0x0001000000000000ull, kCntlBoot, kTooHigh},
            {"R4 CNTL not mesboot's 0x2 (FAULT_DEFAULT 0x3ffffffc): refused, no register written", 0x123456000ull, 0x3ffffffcu, kNotBoot},
        };
        for (const auto &c : cases) {
            Hub h; h.regs[kRegCntl] = c.cntl; uint32_t us = 0; bool acked = false;
            const int r = enable(h, c.pa, &us, &acked);
            check(c.name, r == c.want && h.writes().empty() && h.regs[kRegCntl] == c.cntl && h.regs[kRegDefLo] == kDefLoBoot);
        }
    }
    {   // R5: DEFAULT_ADDR does not hold the value: restored, CNTL never written
        Hub h; h.defLoKeep = 0x0000FFFFu; uint32_t us = 0; bool acked = false;
        const int r = enable(h, 0x0000000123456000ull, &us, &acked);
        check("R5 DEFAULT_ADDR read-back mismatch: refused, CNTL never written, address back to 0xA3FF",
              r == kAddrMismatch && h.firstWrite(kRegCntl) < 0 && h.regs[kRegDefLo] == kDefLoBoot && h.regs[kRegDefHi] == 0 &&
              h.regs[kRegCntl] == kCntlBoot);
    }
    {   // R6: CNTL ignores the write: back to 0x2 and mesboot's address
        Hub h; h.cntlIgnoresWrites = true; uint32_t us = 0; bool acked = false;
        const int r = enable(h, 0x0000000123456000ull, &us, &acked);
        check("R6 CNTL read-back mismatch: refused, CNTL 0x2 and DEFAULT_ADDR 0xA3FF written back",
              r == kCntlMismatch && h.regs[kRegCntl] == kCntlBoot && h.regs[kRegDefLo] == kDefLoBoot && h.regs[kRegDefHi] == 0);
    }
    {   // A1: the engine 17 ack never comes: bounded, reported
        Hub h; h.ackNever = true; uint32_t us = 0; bool acked = true;
        const int r = enable(h, 0x0000000123456000ull, &us, &acked);
        check("A1 no ack: enable still ok (Linux does not fail either), acked false, wait bounded to ~100 ms",
              r == kOk && !acked && us >= kAckCapUs && h.delayed <= kAckCapUs + 10);
    }
    {   // D1: teardown in reverse order
        Hub h; uint32_t us = 0; bool acked = false;
        enable(h, 0x0000000123456000ull, &us, &acked);
        const size_t n0 = h.log.size();
        const int r = disable(h, &us, &acked);
        std::vector<Access> w;
        for (size_t i = n0; i < h.log.size(); i++) if (h.log[i].write) w.push_back(h.log[i]);
        check("D1 disable returns ok", r == kOk && acked);
        check("D1 write order: CNTL 0x2 first, then DEFAULT_ADDR 0xA3FF / 0, then engine 17",
              w.size() == 4 && w[0].reg == kRegCntl && w[0].val == kCntlBoot && w[1].reg == kRegDefLo && w[1].val == 0xA3FFu &&
              w[2].reg == kRegDefHi && w[2].val == 0 && w[3].reg == kRegInvReq && w[3].val == 0x00F80001u);
        check("D1 state = mesboot's (CNTL 0x2, DEFAULT_ADDR 0xA3FF)", h.regs[kRegCntl] == 0x2u && h.regs[kRegDefLo] == 0xA3FFu && h.regs[kRegDefHi] == 0);
        check("D1 never MMHUB", h.onlyAllowed() && h.noSemaphore());
    }
    {   // D2: the bits do not clear: the caller must keep the page
        Hub h; uint32_t us = 0; bool acked = false;
        enable(h, 0x0000000123456000ull, &us, &acked);
        h.cntlIgnoresWrites = true;
        const int r = disable(h, &us, &acked);
        check("D2 CNTL still has the bits after disable: not ok (the page must not be freed)", r == kCntlMismatch);
    }
    {   // S1: the page scan
        static uint32_t page[1024];
        for (uint32_t &d : page) d = 0xFDFDFDFDu;
        uint32_t off = 0, val = 0;
        uint32_t n = scanPage(page, 0xFDFDFDFDu, &off, &val);
        check("S1 a page all = fill: 0 differ, no first", n == 0 && off == 0xFFFFFFFFu && val == 0);
        page[0x100 / 4] = 0xC0FFEE01u;
        page[0x200 / 4] = 0x11111111u;
        n = scanPage(page, 0xFDFDFDFDu, &off, &val);
        check("S1 two dwords differ: count 2, the first at +0x100 = 0xC0FFEE01", n == 2 && off == 0x100 && val == 0xC0FFEE01u);
        for (uint32_t &d : page) d = 0;
        page[1023] = 5;
        n = scanPage(page, 0, &off, &val);
        check("S1 zero fill: the first non-zero dword and its offset", n == 1 && off == 0xFFC && val == 5);
    }
    printf("RESULT: %d passed, %d failed\n", gPass, gFail);
    return gFail != 0;
}
