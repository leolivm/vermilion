// SPDX-License-Identifier: MIT
// recover_test.cpp: host test of the automatic recovery (src/vermilion-boot/PhxRecover.h, kext b127, debug.phxrecover).
// Runs the exact header the kext compiles: the STATUS decode, the kill value per VMID, the stall clock, the ownership
// of a stalled queue head, the decision, the kill loop and the status clear against a register recorder, and a small
// GPU model of the KILL-DRAINED-PASS-1123 / TCP-KILL-1123 runs (the kill unsticks only with the fault default page on).
// Built and run by recover_test.sh (also against mutated copies of the header).
#include "PhxRecover.h"

#include <cstdio>
#include <vector>

using namespace phxrec;

static int gPass, gFail;
static void check(const char *name, bool ok) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? gPass : gFail)++;
}

struct Access { bool write; uint32_t reg, val; };

// GC registers as far as recovery touches them. A write of CNTL with bit 0 set clears STATUS and ADDR (the Linux
// interrupt handler's behaviour); SQ_CMD reads 0xbebebeef as on the box. `killWorks` models the GPU: the stalled
// wave goes when SQ_CMD kills its VMID with MODE broadcast and CHECK_VMID, after `killsNeeded` such writes.
struct Gpu {
    uint32_t cntl = 0x1FFEu, status = 0x0094115Cu, addrLo = 0x00100008u, addrHi = 0;
    uint32_t guilty = 9, killsNeeded = 1, goodKills = 0;
    bool killWorks = true, stuckBit0 = false;
    uint64_t retired = 833455, emitted = 833479, delayed = 0;
    std::vector<Access> log;
    uint32_t rd(uint32_t r) {
        uint32_t v = r == kRegCntl ? cntl : r == kRegStatus ? status : r == kRegAddrLo ? addrLo : r == kRegAddrHi ? addrHi
                   : r == kRegSqCmd ? 0xBEBEBEEFu : 0xDEADBEEFu;
        log.push_back({false, r, v});
        return v;
    }
    void wr(uint32_t r, uint32_t v) {
        log.push_back({true, r, v});
        if (r == kRegCntl) {
            if (v & 1u) { status = 0; addrLo = 0; addrHi = 0; }
            cntl = stuckBit0 ? (v | 1u) : v;
        } else if (r == kRegSqCmd) {
            const bool kill = (v & 0xFu) == 3u, bcast = ((v >> 4) & 7u) == 1u, chk = ((v >> 7) & 1u) != 0;
            if (killWorks && kill && bcast && chk && (v >> 28) == guilty && ++goodKills >= killsNeeded)
                retired = emitted;
        }
    }
    void delayUs(uint32_t us) { delayed += us; }
    uint64_t stalledAt = 833455;
    bool moved() { return retired > stalledAt; }
    std::vector<Access> writes() const {
        std::vector<Access> w;
        for (const Access &a : log) if (a.write) w.push_back(a);
        return w;
    }
    bool onlyRegs(std::initializer_list<uint32_t> ok) const {
        for (const Access &a : log) {
            bool hit = false;
            for (uint32_t r : ok) hit = hit || a.reg == r;
            if (!hit) return false;
        }
        return true;
    }
    bool noMmhub() const {   // nothing on MMHUB (0x1A000-0x1AFFF), never its invalidation semaphores 0x1A751-0x1A762
        for (const Access &a : log) if ((a.reg >= 0x1A000 && a.reg <= 0x1AFFF) || (a.reg >= 0x1A751 && a.reg <= 0x1A762)) return false;
        return true;
    }
};

// gpuSubmit's queue history: n entries from seq s0, each with its VMID
struct Q {
    uint64_t fseq[64] = {};
    uint8_t fvmid[64] = {};
    QView v{};
    Q(bool ready, bool vcn, uint64_t done, uint32_t head, std::initializer_list<std::pair<uint64_t, uint8_t>> e) {
        uint32_t i = head;
        for (auto &p : e) { fseq[i & 63] = p.first; fvmid[i & 63] = p.second; i++; }
        v = QView{ready, vcn, done, head, i, fseq, fvmid};
    }
};

static Input good() {
    Input in{};
    in.recoverOn = true;
    in.faultDefOn = true;
    in.stalledNs = 2500000000ull;
    in.thresholdNs = 2000000000ull;
    in.acted = false;
    in.status = 0x0094115Cu;     // KILL-DRAINED-PASS-1123: VMID 9 CID 8 (TCP) RW 1 mapping 1
    in.ownedMask = 1u << 9;
    return in;
}

int main() {
    {   // D: the STATUS decode (gc_11_0_0_sh_mask GCVM_L2_PROTECTION_FAULT_STATUS)
        const Fault f = decode(0x0094115Cu);
        check("D1 0x0094115c = VMID 9, CID 8 (TCP), RW 1, MAPPING_ERROR 1, PERMISSION 0x5, WALKER 6, MORE 0",
              f.vmid == 9 && f.cid == 8 && f.rw && f.mapping && f.perm == 5 && f.walker == 6 && !f.more && latched(f));
        check("D2 STATUS 0 = nothing latched", !latched(decode(0)));
        check("D3 PERMISSION_FAULTS alone latches (0x00800050: VMID 8 perm 0x5)", latched(decode(0x00800050u)) && decode(0x00800050u).vmid == 8);
        check("D4 MAPPING_ERROR alone latches (0x00a00100: VMID 10)", latched(decode(0x00A00100u)) && decode(0x00A00100u).vmid == 10);
        check("D5 WALKER_ERROR / MORE_FAULTS / CID / RW alone do not latch", !latched(decode(0x009FFE0Fu)) && !latched(decode(0x00F4000Fu)));
        check("D6 #2886's permission fault 0x00701031 = VMID 7 (a kernel VMID)", decode(0x00701031u).vmid == 7 && latched(decode(0x00701031u)));
    }
    {   // K: the kill value
        bool ok = true;
        for (uint32_t v = 8; v <= 15; v++) ok = ok && killValue(v) == (0x93u | (v << 28));
        check("K1 VMID 8-15: 0x93 | vmid << 28 (CMD 3 KILL, MODE 1 BROADCAST, CHECK_VMID)", ok);
        check("K2 VMID 9 = 0x90000093, the value written by hand in KILL-DRAINED-PASS-1123", killValue(9) == 0x90000093u);
        bool none = true;
        for (uint32_t v = 0; v < 8; v++) none = none && killValue(v) == 0;
        check("K3 VMID 0-7 and out of range: 0 (never killed)", none && killValue(16) == 0 && killValue(0xFFFFFFFFu) == 0);
        check("K4 the kill value always has CHECK_VMID and MODE broadcast", ((killValue(12) >> 7) & 1u) && ((killValue(12) >> 4) & 7u) == 1u && (killValue(12) & 0xFu) == 3u);
    }
    {   // S: the stall clock
        Stall s{};
        check("S1 idle (retired == emitted): not stalled", track(s, 100, 100, 1000) == 0 && !s.armed);
        check("S2 the first pending poll starts the clock, returns 0", track(s, 100, 101, 5000000000ull) == 0 && s.armed && s.sinceNs == 5000000000ull);
        check("S3 retired unchanged 2.5 s later: 2.5 s stalled", track(s, 100, 105, 7500000000ull) == 2500000000ull);
        s.acted = true;
        check("S4 retired moved: the clock restarts and acted clears", track(s, 101, 105, 8000000000ull) == 0 && s.sinceNs == 8000000000ull && !s.acted);
        check("S5 idle resets", track(s, 105, 105, 9000000000ull) == 0 && !s.armed);
        // a long idle, then work that hangs at once: the clock starts at the first pending poll, not at the last idle one
        Stall t{};
        track(t, 200, 200, 1000000000ull);
        check("S6 idle for 60 s then a new stall: 0 at the first pending poll (no instant kill)", track(t, 200, 201, 61000000000ull) == 0);
        check("S7 ... and 2 s after it: 2 s", track(t, 200, 202, 63000000000ull) == 2000000000ull);
        Stall u{};
        track(u, 300, 301, 1000);
        u.acted = true;
        track(u, 300, 302, 3000000000ull);
        check("S8 acted stays set while the same stall lasts", u.acted);
    }
    {   // O: who ran the head of a stalled queue
        Q comp(true, false, 833455, 10, {{833456, 9}, {833470, 8}});   // fd3's dispatch (VMID 9) at the compute head
        Q gfx(true, false, 833479, 40, {{833460, 8}, {833479, 8}});    // WindowServer's gfx queue: all done
        Q vcn(true, true, 0, 0, {{833400, 12}});                      // VCN entries never count (MMHUB faults)
        const QView qs[3] = {comp.v, gfx.v, vcn.v};
        check("O1 compute head = VMID 9; the finished gfx queue and VCN add nothing", stalledHeads(qs, 3) == (1u << 9));
        Q lag(true, false, 833458, 0, {{833457, 11}, {833458, 11}, {833459, 9}});   // head not yet advanced past done entries
        check("O2 entries already done are skipped to the first unfinished one", stalledHeads(&lag.v, 1) == (1u << 9));
        Q off(false, false, 0, 0, {{5, 9}});
        check("O3 a queue that is not ready adds nothing", stalledHeads(&off.v, 1) == 0);
        Q both1(true, false, 10, 0, {{11, 9}}), both2(true, false, 20, 0, {{21, 13}});
        const QView qb[2] = {both1.v, both2.v};
        check("O4 two stalled queues: both head VMIDs", stalledHeads(qb, 2) == ((1u << 9) | (1u << 13)));
        Q empty(true, false, 50, 7, {});
        check("O5 an empty queue adds nothing", stalledHeads(&empty.v, 1) == 0);
    }
    {   // P: the decision
        uint32_t v = 99;
        check("P1 stalled >= threshold, faultdef on, STATUS VMID 9 owned: recover VMID 9", decide(good(), &v) == kRecover && v == 9);
        Input a = good(); a.recoverOn = false;
        check("P2 debug.phxrecover 0: off", decide(a, &v) == kOff && v == 0);
        a = good(); a.stalledNs = 1999999999ull;
        check("P3 1 ns under the threshold: wait", decide(a, &v) == kWait && v == 0);
        a = good(); a.stalledNs = 0; a.thresholdNs = 0;
        check("P4 not stalled at all (threshold 0): wait", decide(a, &v) == kWait);
        a = good(); a.acted = true;
        check("P5 already acted on this stall: no second recovery", decide(a, &v) == kActed && v == 0);
        a = good(); a.faultDefOn = false;
        check("P6 debug.phxfaultdef 0: refused (the kill alone does nothing, TCP-KILL-1123)", decide(a, &v) == kFaultDefOff && v == 0);
        a = good(); a.status = 0;
        check("P7 nothing latched: refused", decide(a, &v) == kNoFault && v == 0);
        a = good(); a.status = 0x0074115Cu; a.ownedMask = 0xFFFFu;
        check("P8 STATUS names VMID 7: refused even if every VMID is owned", decide(a, &v) == kNotUser && v == 0);
        a = good(); a.ownedMask = 1u << 8;
        check("P9 STATUS names 9, only 8 (WindowServer) is at a stalled head: refused, 8 never killed", decide(a, &v) == kNotOwned && v == 0);
        a = good(); a.status = 0x0084115Cu; a.ownedMask = (1u << 8) | (1u << 9);
        check("P10 STATUS names WindowServer's VMID 8 and it is owned: recover 8 (the kext logs it loudly)", decide(a, &v) == kRecover && v == 8);
        // exhaustive: whatever is owned, a recovery only ever names the VMID STATUS names
        bool only = true;
        for (uint32_t sv = 0; sv < 16; sv++)
            for (uint32_t m = 0; m < 0x10000; m += 0x101) {
                Input b = good(); b.status = 0x0000115Cu | (sv << 20); b.ownedMask = m;
                uint32_t out = 0;
                const int r = decide(b, &out);
                if (r == kRecover && (out != sv || sv < 8 || !(m & (1u << sv)))) only = false;
                if (r != kRecover && out != 0) only = false;
            }
        check("P11 exhaustive: kRecover only for the STATUS VMID, 8-15, owned; any other verdict names no VMID", only);
    }
    {   // X: the kill loop
        Gpu g; uint32_t w = 0;
        const int r = kill(g, 9, &w);
        const std::vector<Access> ws = g.writes();
        check("X1 unsticks at the first kill: one SQ_CMD write 0x90000093, ok", r == kKillOk && w == 1 && ws.size() == 1 && ws[0].reg == kRegSqCmd && ws[0].val == 0x90000093u);
        check("X2 1 ms after the write before the check", g.delayed == 1000);
        check("X3 only SQ_CMD is touched, nothing read, never MMHUB", g.onlyRegs({kRegSqCmd}) && g.noMmhub());
        Gpu g3; g3.killsNeeded = 3; uint32_t w3 = 0;
        check("X4 needs 3 kills: 3 writes 1 ms apart, then stops", kill(g3, 9, &w3) == kKillOk && w3 == 3 && g3.delayed == 3000 && g3.writes().size() == 3);
        Gpu gs; gs.killWorks = false; uint32_t w10 = 0;
        const int rs = kill(gs, 9, &w10);
        bool same = true;
        for (const Access &a : gs.writes()) same = same && a.reg == kRegSqCmd && a.val == 0x90000093u;
        check("X5 never unsticks: 10 writes over 10 ms, kKillStuck, the same value every time", rs == kKillStuck && w10 == 10 && gs.delayed == 10000 && same && gs.writes().size() == 10);
        check("X6 a stuck kill touches nothing else (no reset, no CNTL, no MMHUB)", gs.onlyRegs({kRegSqCmd}) && gs.noMmhub());
        Gpu gb; uint32_t wb = 7;
        check("X7 VMID 3: refused before any write", kill(gb, 3, &wb) == kKillBadVmid && wb == 0 && gb.log.empty());
        bool each = true;
        for (uint32_t v = 8; v <= 15; v++) {
            Gpu gv; gv.guilty = v; uint32_t wv = 0;
            each = each && kill(gv, v, &wv) == kKillOk && gv.writes().size() == 1 && gv.writes()[0].val == (0x93u | (v << 28));
        }
        check("X8 every user VMID gets its own kill value", each);
        Gpu gw; gw.guilty = 10; uint32_t ww = 0;
        check("X9 killing VMID 9 while VMID 10's wave hangs does not unstick it", kill(gw, 9, &ww) == kKillStuck && ww == 10);
    }
    {   // C: the status clear
        Gpu g;
        const uint32_t after = clearStatus(g);
        const std::vector<Access> &l = g.log;
        check("C1 sequence: read STATUS (dummy), read CNTL, write CNTL|1, write CNTL, read STATUS",
              l.size() == 5 && !l[0].write && l[0].reg == kRegStatus && !l[1].write && l[1].reg == kRegCntl &&
              l[2].write && l[2].reg == kRegCntl && l[2].val == 0x1FFFu && l[3].write && l[3].reg == kRegCntl && l[3].val == 0x1FFEu &&
              !l[4].write && l[4].reg == kRegStatus);
        check("C2 STATUS and ADDR cleared, CNTL back to debug.phxfaultdef's 0x1FFE", after == 0 && g.status == 0 && g.addrLo == 0 && g.cntl == 0x1FFEu);
        check("C3 only CNTL written, never MMHUB", g.onlyRegs({kRegCntl, kRegStatus}) && g.noMmhub());
        Gpu g2; g2.cntl = 0x2;
        clearStatus(g2);
        check("C4 from mesboot's CNTL 0x2: writes 0x3 then 0x2", g2.writes().size() == 2 && g2.writes()[0].val == 0x3u && g2.writes()[1].val == 0x2u && g2.cntl == 0x2u);
        Gpu g3; g3.cntl = 0x1FFFu;
        clearStatus(g3);
        check("C5 a CLEAR bit left at 1 is written back as 0", g3.writes().back().val == 0x1FFEu && g3.cntl == 0x1FFEu);
        Gpu g4; g4.cntl = 0x3FFFFFFCu;   // Linux's value on this card: every other bit is preserved
        clearStatus(g4);
        check("C6 every other CNTL bit is preserved (Linux 0x3ffffffc)", g4.writes()[0].val == 0x3FFFFFFDu && g4.writes()[1].val == 0x3FFFFFFCu);
    }
    {   // E: the poll as the kext runs it (track -> decide -> kill -> clear), on the two box runs of 11-23
        auto poll = [](Gpu &g, Stall &s, bool faultDefOn, uint64_t now, int *verdict, int *killR, uint32_t *writes) {
            *verdict = -1; *killR = -1; *writes = 0;
            const uint64_t st = track(s, g.retired, g.emitted, now);
            if (!st || st < 2000000000ull || s.acted) return;
            Q comp(true, false, g.retired, 0, {{g.retired + 1, static_cast<uint8_t>(g.guilty)}});
            Input in{true, faultDefOn, st, 2000000000ull, false, g.rd(kRegStatus), stalledHeads(&comp.v, 1) & 0xFF00u};
            uint32_t v = 0;
            *verdict = decide(in, &v);
            s.acted = true;
            if (*verdict != kRecover) return;
            g.stalledAt = g.retired;
            *killR = kill(g, v, writes);
            if (*killR == kKillOk) clearStatus(g);
        };
        Gpu on; Stall s{}; int vd, kr; uint32_t w;
        poll(on, s, true, 1000000000ull, &vd, &kr, &w);
        const bool early = vd == -1;
        poll(on, s, true, 2999999999ull, &vd, &kr, &w);
        const bool stillEarly = vd == -1;
        poll(on, s, true, 3000000000ull, &vd, &kr, &w);
        check("E1 KILL-DRAINED model: nothing before 2 s, then one kill of VMID 9, retired == emitted, STATUS cleared",
              early && stillEarly && vd == kRecover && kr == kKillOk && w == 1 && on.retired == on.emitted && on.status == 0 && on.cntl == 0x1FFEu);
        poll(on, s, true, 9000000000ull, &vd, &kr, &w);
        check("E2 the GPU idle afterwards: no further action", vd == -1);
        Gpu off; off.killWorks = false; off.cntl = 0x2; Stall s2{};
        poll(off, s2, false, 0, &vd, &kr, &w);
        poll(off, s2, false, 5000000000ull, &vd, &kr, &w);
        check("E3 TCP-KILL model (faultdef off): refused, no SQ_CMD write at all, STATUS kept", vd == kFaultDefOff && off.writes().empty() && off.status == 0x0094115Cu);
        poll(off, s2, false, 10000000000ull, &vd, &kr, &w);
        check("E4 ... and not retried while the same stall lasts", vd == -1 && off.writes().empty());
        Gpu fail; fail.killWorks = false; Stall s3{};
        poll(fail, s3, true, 0, &vd, &kr, &w);
        poll(fail, s3, true, 2500000000ull, &vd, &kr, &w);
        check("E5 a kill that does not unstick: 10 writes, failure, STATUS kept as evidence, CNTL untouched",
              vd == kRecover && kr == kKillStuck && w == 10 && fail.status == 0x0094115Cu && fail.cntl == 0x1FFEu);
        poll(fail, s3, true, 6000000000ull, &vd, &kr, &w);
        check("E6 ... and no second try in the same stall (nothing else is done)", vd == -1 && fail.writes().size() == 10);
        check("E7 none of the runs touched MMHUB", on.noMmhub() && off.noMmhub() && fail.noMmhub());
    }
    printf("RESULT: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
