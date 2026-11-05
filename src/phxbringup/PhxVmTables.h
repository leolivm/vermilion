// SPDX-License-Identifier: MIT
// PhxVmTables.h: the GPUVM page-table walk, map/unmap and empty-leaf reclaim of PhxBringup (b77: page tables ran out on exit from full-screen video).
//
// Kernel-free on purpose: a host test runs this exact code against a simulated table
// pool, GPU and TLB. The kext supplies an environment E with:
//   Shadow *sh;                 per-pool CPU shadow (nullptr: no accounting, never reclaims)
//   uint64_t poolPhys;          physical address of pool table 0 (the VRAM page-table pool)
//   uint8_t *poolCpu;           CPU pointer to pool table 0
//   uint64_t tablePhys(Ctx &, uint32_t i);   table i of a context, allocating its chunk; 0 when that fails
//   void flush(const volatile void *);       after a CPU write of a table entry
//   void event(Ev);                          counters
//   int mode();                              debug.phxpt_reclaim: 0 off, 1 reclaim + reuse, 2 = 1 + verify reused tables
//   uint32_t keep();                         debug.phxpt_keep: empty leaves a context keeps attached before detaching
//   uint64_t emitted(), retired();           GPU sequence numbers: newest emitted, newest retired
// The caller holds the lock that serializes every table change and every submission (gpuLock).
//
// Levels: root (va >> 39), PDB1 (>> 30), PDB0 (>> 21), leaf PTB (>> 12). Only leaves are reclaimed.
// The shadow keeps, per pool table, a bitmap of the PTEs made valid here and their count, so map/unmap never read PTEs
// back over BAR0 (uncached; b53 removed those reads). A leaf whose count drops to 0 stays attached (its 2 MiB is often
// mapped again: the VA allocator is first-fit) on its context's list of empty leaves, oldest first. When an unmap leaves
// more than keep() of them, the oldest are detached (PDB0 entry cleared) and parked with the newest emitted seqno.
// A parked leaf is handed out again only to a later map of the same context, once the GPU retired that seqno, and only
// if the caller's TLB invalidation after the detach was acknowledged (otherwise cancel() drops it until the context ends).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxvm {

constexpr uint32_t kChunkBytes = 64u << 10, kChunkPages = kChunkBytes / 4096, kChunks = 256, kTables = kChunks * kChunkPages;
constexpr uint64_t kAddrMask = 0x0000FFFFFFFFF000ull;
constexpr uint32_t kNil = 0xFFFF;
constexpr uint32_t kDetachPerCall = 64;

// kLeaf: attached, >= 1 valid PTE; kEmpty: attached, none, on the context's empty list; kParked: detached, on the FIFO;
// kLost: detached, its invalidation was not acknowledged, out of use until the context ends
enum : uint8_t { kNone = 0, kDir = 1, kLeaf = 2, kParked = 3, kLost = 4, kEmpty = 5 };
enum Ev { EvCap, EvPoolFull, EvFresh, EvReclaim, EvReuse, EvCancel, EvDirty, EvDrift, EvNoPde, EvCount };

// A context's tables: chunks of 16 tables allocated as it maps; table i is page i % 16 of chunk i / 16, table 0 the root.
struct Ctx {
    uint8_t *chunk[kChunks];
    uint64_t chunkPhys[kChunks];
    uint64_t phys;          // the root table
    uint32_t pages, used;
    bool busy;
    uint32_t hw;            // the hardware VMID pointed at these tables, 0 = none
    // pool table numbers; a head/tail is valid only while its count is non-zero
    uint32_t parked, empties;
    uint16_t parkHead, parkTail, emptyHead, emptyTail;
};

struct Shadow {
    uint32_t n;             // pool tables
    uint64_t *valid;        // n x 8 words: bit i = PTE i made valid by map()
    uint32_t *seq;          // parked: low 32 bits of the newest emitted seqno when it was detached
    uint32_t *region;       // leaf: va >> 21 of the 2 MiB it maps
    uint16_t *live;         // set bits in valid
    uint16_t *next;         // parked FIFO / empty list link
    uint16_t *prev;         // empty list back link
    uint8_t *kind;
    static size_t bytes(uint32_t count) { return static_cast<size_t>(count) * (64 + 4 + 4 + 2 + 2 + 2 + 1); }
    void carve(void *mem, uint32_t count) {
        n = count;
        uint8_t *p = static_cast<uint8_t *>(mem);
        valid = reinterpret_cast<uint64_t *>(p); p += static_cast<size_t>(count) * 64;
        seq = reinterpret_cast<uint32_t *>(p); p += static_cast<size_t>(count) * 4;
        region = reinterpret_cast<uint32_t *>(p); p += static_cast<size_t>(count) * 4;
        live = reinterpret_cast<uint16_t *>(p); p += static_cast<size_t>(count) * 2;
        next = reinterpret_cast<uint16_t *>(p); p += static_cast<size_t>(count) * 2;
        prev = reinterpret_cast<uint16_t *>(p); p += static_cast<size_t>(count) * 2;
        kind = p;
    }
    void clear(uint32_t g) {
        for (uint32_t k = 0; k < 8; k++) valid[g * 8 + k] = 0;
        live[g] = 0; kind[g] = kNone; seq[g] = 0; region[g] = 0; next[g] = 0; prev[g] = 0;
    }
};

// Per call of unmap(): where the parked FIFO stood before, so cancel() can drop what this call parked.
struct Park { uint32_t parkedBefore, tailBefore, n; };

struct Census {
    uint32_t tables, dirs, leaves, empty, sparse, mid, dense, parked, ready, lost;
    uint64_t livePtes;
};

// The CPU pointer of a table of this context, nullptr when it is none of its chunks.
inline uint64_t *table(const Ctx &c, uint64_t phys) {
    for (uint32_t i = 0; i < kChunks && c.chunk[i]; i++)
        if (phys - c.chunkPhys[i] < kChunkBytes)
            return reinterpret_cast<uint64_t *>(c.chunk[i] + (phys - c.chunkPhys[i]));
    return nullptr;
}

template <class E> inline uint32_t gid(const E &e, uint64_t phys) {
    if (!e.sh || phys < e.poolPhys)
        return kNil;
    const uint64_t g = (phys - e.poolPhys) >> 12;
    return g < e.sh->n ? static_cast<uint32_t>(g) : kNil;
}

inline void emptyLink(Shadow &s, Ctx &c, uint32_t g) {   // append (newest at the tail)
    s.kind[g] = kEmpty;
    s.next[g] = 0;
    s.prev[g] = c.emptyTail;
    if (c.empties) s.next[c.emptyTail] = static_cast<uint16_t>(g);
    else c.emptyHead = static_cast<uint16_t>(g);
    c.emptyTail = static_cast<uint16_t>(g);
    c.empties++;
}

inline void emptyUnlink(Shadow &s, Ctx &c, uint32_t g) {
    if (g == c.emptyHead) c.emptyHead = s.next[g]; else s.next[s.prev[g]] = s.next[g];
    if (g == c.emptyTail) c.emptyTail = s.prev[g]; else s.prev[s.next[g]] = s.prev[g];
    if (!--c.empties) c.emptyHead = c.emptyTail = 0;
    s.kind[g] = kLeaf;
}

template <class E> inline void markValid(E &e, Ctx &c, uint32_t g, uint32_t i) {
    if (g == kNil) return;
    Shadow &s = *e.sh;
    uint64_t &w = s.valid[g * 8 + (i >> 6)];
    const uint64_t b = 1ull << (i & 63);
    if (w & b) return;
    w |= b;
    if (!s.live[g]++ && s.kind[g] == kEmpty) emptyUnlink(s, c, g);
}

template <class E> inline void markInvalid(E &e, Ctx &c, uint32_t g, uint32_t i) {
    if (g == kNil) return;
    Shadow &s = *e.sh;
    uint64_t &w = s.valid[g * 8 + (i >> 6)];
    const uint64_t b = 1ull << (i & 63);
    if (!(w & b)) return;
    w &= ~b;
    if (!s.live[g]) { e.event(EvDrift); return; }
    if (!--s.live[g] && s.kind[g] == kLeaf) emptyLink(s, c, g);
}

// The oldest parked leaf of the context whose detach the GPU has retired, kNil when none (or reclaim is off).
template <class E> uint32_t takeParked(E &e, Ctx &c) {
    const int mode = e.mode();
    if (!e.sh || !c.parked || mode < 1)
        return kNil;
    Shadow &s = *e.sh;
    const uint32_t g = c.parkHead;
    if (static_cast<int32_t>(static_cast<uint32_t>(e.retired()) - s.seq[g]) < 0)
        return kNil;   // FIFO in seqno order: if the head is not retired, nothing behind it is
    c.parkHead = s.next[g];
    if (!--c.parked) c.parkHead = c.parkTail = 0;
    if (mode >= 2) {   // the shadow says every PTE is zero; check it over BAR0 (slow, for testing)
        volatile uint64_t *t = reinterpret_cast<volatile uint64_t *>(e.poolCpu + static_cast<uint64_t>(g) * 4096);
        bool dirty = false;
        for (uint32_t i = 0; i < 512; i++)
            if (t[i]) { t[i] = 0; e.flush(t + i); dirty = true; }
        if (dirty) e.event(EvDirty);
    }
    e.event(EvReuse);
    return g;
}

// The PTE of va, building the missing levels when alloc is set; *leafG = the leaf's pool table number (kNil: none).
// A new leaf is a retired parked one if there is one, else the context's next table.
template <class E> uint64_t *pte(E &e, Ctx &c, uint64_t va, bool alloc, uint32_t *leafG) {
    uint64_t tbl = c.phys;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = table(c, tbl);
        if (!t)
            return nullptr;
        uint64_t *ent = t + ((va >> shift) & 511);
        if (!(*ent & 1)) {
            if (!alloc)
                return nullptr;
            uint32_t g = shift == 21 ? takeParked(e, c) : kNil;
            if (g != kNil) {
                *ent = (e.poolPhys + static_cast<uint64_t>(g) * 4096) | 1;
                e.flush(ent);
            } else {
                if (c.used >= c.pages) {
                    e.event(EvCap);
                    return nullptr;
                }
                const uint64_t child = e.tablePhys(c, c.used);
                if (!child) {
                    e.event(EvPoolFull);
                    return nullptr;
                }
                c.used++;
                *ent = child | 1;   // navi31: local PDE (VALID); the tables are in VRAM
                g = gid(e, child);
                if (shift == 21) e.event(EvFresh);
            }
            // a new leaf has no valid PTE yet: kLeaf (not on the empty list); the map writes one right away
            if (g != kNil) {
                e.sh->kind[g] = shift == 21 ? kLeaf : kDir;
                if (shift == 21) e.sh->region[g] = static_cast<uint32_t>(va >> 21);
            }
        }
        tbl = *ent & kAddrMask;
    }
    uint64_t *t = table(c, tbl);
    if (leafG) *leafG = t ? gid(e, tbl) : kNil;
    return t ? t + ((va >> 12) & 511) : nullptr;
}

// Clears what a failed map wrote (one walk per page, as b48). Emptied leaves stay attached (empty list).
template <class E> void undo(E &e, Ctx &c, uint64_t va, uint64_t upto) {
    for (uint64_t q = 0; q < upto; q += 4096) {
        uint32_t g = kNil;
        if (uint64_t *u = pte(e, c, va + q, false, &g)) {
            *u = 0;
            e.flush(u);
            markInvalid(e, c, g, static_cast<uint32_t>((va + q) >> 12) & 511);
        }
    }
}

// Maps [va, va + size): value(p) is the PTE for byte p of the range, 0 = no physical page (the map fails).
// Returns size on success, else the offset where it failed (*noPage tells why) after undoing [va, va + offset).
// b53: the leaf is walked once per 2 MiB and its entries written in a row.
template <class E, class V> uint64_t map(E &e, Ctx &c, uint64_t va, uint64_t size, V value, bool *noPage) {
    uint64_t *leaf = nullptr;
    uint32_t g = kNil;
    for (uint64_t p = 0; p < size; p += 4096) {
        const uint64_t v = value(p);
        uint64_t *pt = !v ? nullptr : (leaf && ((va + p) & 0x1FFFFFull)) ? ++leaf : (leaf = pte(e, c, va + p, true, &g));
        if (!pt) {
            *noPage = !v;
            undo(e, c, va, p);
            return p;
        }
        *pt = v;
        e.flush(pt);
        markValid(e, c, g, static_cast<uint32_t>((va + p) >> 12) & 511);
    }
    return size;
}

// Detaches the empty leaf g (taken off the empty list): clears its PDB0 entry and parks it. The caller flushes the TLB.
template <class E> bool park(E &e, Ctx &c, uint32_t g) {
    Shadow &s = *e.sh;
    const uint64_t va = static_cast<uint64_t>(s.region[g]) << 21;
    uint64_t tbl = c.phys, *ent = nullptr;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = table(c, tbl);
        if (!t)
            return false;
        ent = t + ((va >> shift) & 511);
        if (!(*ent & 1))
            return false;
        tbl = *ent & kAddrMask;
    }
    if (tbl != e.poolPhys + static_cast<uint64_t>(g) * 4096)
        return false;
    *ent = 0;
    e.flush(ent);
    s.kind[g] = kParked;
    s.seq[g] = static_cast<uint32_t>(e.emitted());
    s.next[g] = 0;
    if (c.parked) s.next[c.parkTail] = static_cast<uint16_t>(g);
    else c.parkHead = static_cast<uint16_t>(g);
    c.parkTail = static_cast<uint16_t>(g);
    c.parked++;
    e.event(EvReclaim);
    return true;
}

// Unmaps [va, va + size) (b53 walk). With reclaim on, the context then keeps at most keep() empty leaves attached:
// the oldest emptied beyond that are detached and parked.
template <class E> Park unmap(E &e, Ctx &c, uint64_t va, uint64_t size) {
    Park mk{c.parked, c.parked ? c.parkTail : kNil, 0};
    uint64_t *leaf = nullptr;
    uint32_t g = kNil;
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pt = (leaf && ((va + p) & 0x1FFFFFull)) ? ++leaf : (leaf = pte(e, c, va + p, false, &g));
        if (!pt)
            continue;
        *pt = 0;
        e.flush(pt);
        markInvalid(e, c, g, static_cast<uint32_t>((va + p) >> 12) & 511);
    }
    if (e.sh && e.mode() >= 1) {
        const uint32_t keep = e.keep();
        // at most kDetachPerCall per unmap (each is 3 uncached BAR0 reads): turning reclaim on with a long empty list
        // spreads the catch-up over the next unmaps instead of one long hold of gpuLock
        for (uint32_t k = 0; c.empties > keep && k < kDetachPerCall; k++) {
            const uint32_t h = c.emptyHead;
            emptyUnlink(*e.sh, c, h);     // kLeaf with no valid PTE if park() refuses (stays attached)
            if (park(e, c, h)) mk.n++;
            else e.event(EvNoPde);
        }
    }
    return mk;
}

// The TLB invalidation after unmap() was not acknowledged: what that call parked is never handed out again.
template <class E> void cancel(E &e, Ctx &c, const Park &mk) {
    if (!mk.n || !e.sh)
        return;
    Shadow &s = *e.sh;
    uint32_t g = mk.parkedBefore ? s.next[mk.tailBefore] : c.parkHead;
    for (uint32_t k = 0; k < mk.n; k++) {
        const uint32_t nx = s.next[g];
        s.kind[g] = kLost;
        e.event(EvCancel);
        g = nx;
    }
    c.parked = mk.parkedBefore;
    if (c.parked) { c.parkTail = static_cast<uint16_t>(mk.tailBefore); s.next[mk.tailBefore] = 0; }
    else c.parkHead = c.parkTail = 0;
}

template <class E> inline uint32_t gidOf(const E &e, const Ctx &c, uint32_t i) {
    const uint32_t k = i / kChunkPages;
    return k < kChunks && c.chunk[k] ? gid(e, c.chunkPhys[k] + static_cast<uint64_t>(i % kChunkPages) * 4096) : kNil;
}

// The context's tables [0, used) were zeroed (context freed) or it is new: forget them. The root stays a directory.
template <class E> void reset(E &e, Ctx &c) {
    c.parked = c.empties = 0;
    c.parkHead = c.parkTail = c.emptyHead = c.emptyTail = 0;
    if (!e.sh)
        return;
    for (uint32_t i = 0; i < c.used; i++) {
        const uint32_t g = gidOf(e, c, i);
        if (g != kNil) e.sh->clear(g);
    }
    const uint32_t r = gid(e, c.phys);
    if (r != kNil) e.sh->kind[r] = kDir;
}

template <class E> Census census(const E &e, const Ctx &c, uint64_t retired) {
    Census n{};
    n.tables = c.used;
    if (!e.sh)
        return n;
    const Shadow &s = *e.sh;
    for (uint32_t i = 0; i < c.used; i++) {
        const uint32_t g = gidOf(e, c, i);
        if (g == kNil) continue;
        switch (s.kind[g]) {
        case kDir: n.dirs++; break;
        case kParked: n.parked++; break;
        case kLost: n.lost++; break;
        case kEmpty: n.leaves++; n.empty++; break;
        case kLeaf: {
            n.leaves++;
            const uint32_t l = s.live[g];
            n.livePtes += l;
            if (!l) n.empty++; else if (l <= 16) n.sparse++; else if (l < 256) n.mid++; else n.dense++;
            break; }
        default: break;
        }
    }
    uint32_t g = c.parkHead;
    for (uint32_t k = 0; k < c.parked && g < s.n; k++, g = s.next[g])
        if (static_cast<int32_t>(static_cast<uint32_t>(retired) - s.seq[g]) >= 0) n.ready++;
    return n;
}

}  // namespace phxvm
