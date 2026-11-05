// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxsurfmap.h: M7 S4b, the IOSurface ID -> head table. Kernel-free and
// lock-free by itself: PhxAccel holds one IOSimpleLock around learn() and exportBlob(); a host test
// drives the same header.
//
// Learned in NVDisplayPipe::performTransaction from the transaction itself: the pipe's framebuffer gives the head
// (NVAccel::pipeFramebuffer), plane 0's IOSurface gives the ID. Published read-only as the opaque sysctl
// debug.phxsurfmap, which the Metal plugin reads in WindowServer before it flips HUBP0 to a surface
// (src/metal-plugin/nvmtl_headmap.h parses the same layout). Design after Cobalt64's native_m6_pure.h SurfTable
// (MIT), reduced to two heads and time-based ambiguity.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxsurfmap {

constexpr unsigned kHeads = 2;
constexpr unsigned kMax = 16;                       // 2-3 display surfaces per head and per format (BGRA, l10r), with margin
constexpr uint64_t kAmbigNs = 1000000000ull;        // seen on both heads within 1 s = ambiguous (refused everywhere)
constexpr uint32_t kMagic = 0x50534d31u;            // 'PSM1'
constexpr uint32_t kFlagAmbiguous = 1u;
constexpr size_t kHdrBytes = 16, kEntBytes = 16;    // header: magic, generation, n, nowMs; entry: id, head, flags, ageMs
constexpr size_t kBlobMax = kHdrBytes + kMax * kEntBytes;

struct Ent { uint32_t id; uint32_t head; uint64_t seenNs[kHeads]; };   // id 0 = free; head = the latest pipe that named it
struct Table { Ent e[kMax]; uint32_t n; uint32_t gen; uint64_t learns, replaced, evicted, badHead, badId; };

enum : int { kNew = 0, kSame = 1, kMoved = 2, kEvicted = 3, kRefusedId = -1, kRefusedHead = -2 };

inline uint64_t lastSeen(const Ent &x) { uint64_t m = 0; for (unsigned h = 0; h < kHeads; h++) if (x.seenNs[h] > m) m = x.seenNs[h]; return m; }

// One transaction on head `head` named surface `id` at `nowNs`. kMoved: the ID was last named by the other head (a
// freed and reused ID, or a surface really shown on both: exportBlob decides which by the time window).
inline int learn(Table &t, uint32_t id, unsigned head, uint64_t nowNs) {
    if (!id) { t.badId++; return kRefusedId; }
    if (head >= kHeads) { t.badHead++; return kRefusedHead; }
    t.learns++;
    for (uint32_t i = 0; i < t.n; i++) {
        Ent &x = t.e[i];
        if (x.id != id) continue;
        const int r = x.head == head ? kSame : kMoved;
        x.seenNs[head] = nowNs; x.head = head;
        if (r == kMoved) { t.replaced++; t.gen++; }
        return r;
    }
    uint32_t slot = t.n;
    int r = kNew;
    if (t.n < kMax) t.n++;
    else {   // full: the entry named longest ago makes room
        slot = 0;
        for (uint32_t i = 1; i < kMax; i++) if (lastSeen(t.e[i]) < lastSeen(t.e[slot])) slot = i;
        t.evicted++; r = kEvicted;
    }
    Ent &x = t.e[slot];
    x.id = id; x.head = head;
    for (unsigned h = 0; h < kHeads; h++) x.seenNs[h] = 0;
    x.seenNs[head] = nowNs;
    t.gen++;
    return r;
}

// Forget everything (a WindowServer restart reallocates its surfaces; IOSurface IDs are reused lowest-free).
inline void reset(Table &t) { for (uint32_t i = 0; i < kMax; i++) t.e[i] = Ent{}; t.n = 0; t.gen++; }

inline bool ambiguous(const Ent &x) {
    if (!x.seenNs[0] || !x.seenNs[1]) return false;
    const uint64_t d = x.seenNs[0] > x.seenNs[1] ? x.seenNs[0] - x.seenNs[1] : x.seenNs[1] - x.seenNs[0];
    return d < kAmbigNs;
}

inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// The sysctl's bytes (little endian): u32 magic, u32 generation, u32 n, u32 nowMs (low 32 bits of nowNs / 1e6), then n x
// { u32 id, u32 head, u32 flags (bit 0 ambiguous), u32 ageMs = ms since the latest sighting, saturated }. Returns the
// byte count, 0 when cap is too small.
inline size_t exportBlob(const Table &t, uint64_t nowNs, uint8_t *out, size_t cap) {
    const size_t need = kHdrBytes + (size_t)t.n * kEntBytes;
    if (!out || cap < need) return 0;
    put32(out, kMagic); put32(out + 4, t.gen); put32(out + 8, t.n); put32(out + 12, (uint32_t)(nowNs / 1000000ull));
    for (uint32_t i = 0; i < t.n; i++) {
        const Ent &x = t.e[i];
        const uint64_t ls = lastSeen(x), age = nowNs > ls ? (nowNs - ls) / 1000000ull : 0;
        uint8_t *p = out + kHdrBytes + (size_t)i * kEntBytes;
        put32(p, x.id); put32(p + 4, x.head); put32(p + 8, ambiguous(x) ? kFlagAmbiguous : 0u);
        put32(p + 12, age > 0xffffffffull ? 0xffffffffu : (uint32_t)age);
    }
    return need;
}

}  // namespace phxsurfmap
