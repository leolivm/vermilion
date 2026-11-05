// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* nvmtl_headmap.h: M7 S4b, which head a WindowServer display surface belongs to.
   Pure C, no locking, no Vulkan: the caller reads the kernel's sysctl debug.phxsurfmap (PhxAccel,
   src/vermilion-accel/accel/phxsurfmap.h exportBlob) and asks for a verdict before HUBP0 is flipped to a surface.
   A host test drives this file together with the kernel half. */
#ifndef NVMTL_HEADMAP_H
#define NVMTL_HEADMAP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NVMTL_HM_MAGIC   0x50534d31u   /* 'PSM1', == phxsurfmap::kMagic */
#define NVMTL_HM_HDR     16u
#define NVMTL_HM_ENT     16u
#define NVMTL_HM_MAX     16u
#define NVMTL_HM_AMBIG   1u
#define NVMTL_HM_HEADS   2u

enum { NVMTL_HM_OK = 0, NVMTL_HM_UNKNOWN = 1, NVMTL_HM_OTHER = 2, NVMTL_HM_AMBIGUOUS = 3, NVMTL_HM_NOMAP = 4 };

typedef struct { uint32_t id, head, flags, age_ms; } nvmtl_hm_ent;
typedef struct { uint32_t gen, n; nvmtl_hm_ent e[NVMTL_HM_MAX]; } nvmtl_hm;

static inline uint32_t nvmtl_hm_rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* 0 parsed; -1 refused (bad magic, count or length, a corrupt entry): the caller then treats the map as missing. */
static inline int nvmtl_hm_parse(nvmtl_hm *m, const uint8_t *b, size_t len) {
    if (!m || !b || len < NVMTL_HM_HDR || nvmtl_hm_rd32(b) != NVMTL_HM_MAGIC) return -1;
    const uint32_t n = nvmtl_hm_rd32(b + 8);
    if (n > NVMTL_HM_MAX || len != (size_t)NVMTL_HM_HDR + (size_t)n * NVMTL_HM_ENT) return -1;
    nvmtl_hm t; memset(&t, 0, sizeof t);
    t.gen = nvmtl_hm_rd32(b + 4); t.n = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = b + NVMTL_HM_HDR + i * NVMTL_HM_ENT;
        t.e[i].id = nvmtl_hm_rd32(p); t.e[i].head = nvmtl_hm_rd32(p + 4); t.e[i].flags = nvmtl_hm_rd32(p + 8); t.e[i].age_ms = nvmtl_hm_rd32(p + 12);
        if (!t.e[i].id || t.e[i].head >= NVMTL_HM_HEADS) return -1;
    }
    *m = t;
    return 0;
}

/* Forget the entries last named before this WindowServer existed (age_ms > the process's age): the kernel table outlives
   a WindowServer restart, and the new process reuses the old one's IOSurface IDs, so such an entry names the old
   process's head (seen in M7 S4b: reused 4, 5, 6 read as head 1 for the new WS's first second). */
static inline void nvmtl_hm_drop_older(nvmtl_hm *m, uint64_t proc_age_ms) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->n && i < NVMTL_HM_MAX; i++)
        if ((uint64_t)m->e[i].age_ms <= proc_age_ms) m->e[k++] = m->e[i];
    m->n = k;
}

/* May HUBP0 (head 0) show surface sid? Only a surface the kernel saw on head 0's pipe, last, and not on both heads
   within its ambiguity window. map == NULL (no sysctl: an older PhxAccel, or a read failure) = NVMTL_HM_NOMAP. */
static inline int nvmtl_hm_verdict(const nvmtl_hm *m, uint32_t sid) {
    if (!m) return NVMTL_HM_NOMAP;
    for (uint32_t i = 0; i < m->n && i < NVMTL_HM_MAX; i++) {
        if (m->e[i].id != sid) continue;
        if (m->e[i].flags & NVMTL_HM_AMBIG) return NVMTL_HM_AMBIGUOUS;
        return m->e[i].head == 0 ? NVMTL_HM_OK : NVMTL_HM_OTHER;
    }
    return NVMTL_HM_UNKNOWN;
}

/* The policy. mode 0 = off (today: everything presents), 1 = observe (count, present anyway), 2 = enforce (only OK).
   NOMAP presents in every mode: with no map the kernel's phxhead::pluginSafe still keeps head 1 under 1920x1080. */
static inline int nvmtl_hm_may_present(int mode, int verdict) {
    if (mode != 2) return 1;
    return verdict == NVMTL_HM_OK || verdict == NVMTL_HM_NOMAP;
}

/* The fail-safe: in enforce mode, this many refused frames in a row with no presented one between them means the map
   no longer describes head 0 (head 0 would freeze on the last slot): the caller gives the plane back
   (nvmtl_vk_scanout_stop), which returns head 0 to the CPU copy for the life of the process. ~2 s at 60 Hz. */
#define NVMTL_HM_STALL_REFUSALS 120u
static inline int nvmtl_hm_stalled(uint32_t refused_in_a_row) { return refused_in_a_row >= NVMTL_HM_STALL_REFUSALS; }

/* The switch file /Library/GPUBundles/nvmtl/ws-headmap: absent = 0 (off), "observe" = 1, empty or "enforce" = 2. Any other
   content = 0, so a typo never turns enforcement on. Leading/trailing whitespace is ignored. */
static inline int nvmtl_hm_mode_parse(int exists, const char *s) {
    if (!exists) return 0;
    if (!s) return 2;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n == 0 || (n == 7 && !memcmp(s, "enforce", 7))) return 2;
    if (n == 7 && !memcmp(s, "observe", 7)) return 1;
    return 0;
}

/* Per-present bookkeeping: count the verdict, apply the policy and the stall fail-safe. Returns 1 present, 0 refuse,
   -1 refuse and give the plane back (the stall: head 0 goes to the CPU copy for the life of the process). */
typedef struct { uint64_t verdicts[5], calls, refused, stalls; uint32_t in_a_row; } nvmtl_hm_stats;
static inline int nvmtl_hm_decide(nvmtl_hm_stats *st, int mode, int verdict) {
    if (verdict < NVMTL_HM_OK || verdict > NVMTL_HM_NOMAP) verdict = NVMTL_HM_NOMAP;
    st->calls++; st->verdicts[verdict]++;
    if (nvmtl_hm_may_present(mode, verdict)) { st->in_a_row = 0; return 1; }
    st->refused++;
    if (nvmtl_hm_stalled(++st->in_a_row)) { st->stalls++; st->in_a_row = 0; return -1; }
    return 0;
}

/* Whether a CoreDisplay surface may join the CDCOPY display set (nvmtl_ws_note_display): only enforce keeps a surface the
   kernel puts on the other head, or on both, out of it. UNKNOWN joins (the present gate still refuses it in enforce). */
static inline int nvmtl_hm_may_join(int mode, int verdict) {
    return mode != 2 || (verdict != NVMTL_HM_OTHER && verdict != NVMTL_HM_AMBIGUOUS);
}

/* The CDCOPY display set (nvmtl_ws_note_display): the last NVMTL_WS_SET surfaces an unwaited CoreDisplay commit wrote.
   Returns 1 when sid is a member. add = 1 inserts a missing sid over the oldest member (FIFO). Before 10-30 the plugin
   wrote display[nd++ % 8] with nd clamped at 8, so once full every new surface overwrote slot 0 and slots 1-7 kept
   their first eight IDs for the life of the process (reused IDs then joined the set as stale members). */
#define NVMTL_WS_SET 8u
typedef struct { uint32_t id[NVMTL_WS_SET]; unsigned n, next; } nvmtl_ws_set;
static inline int nvmtl_ws_set_has(const nvmtl_ws_set *s, uint32_t sid) {
    for (unsigned i = 0; i < s->n && i < NVMTL_WS_SET; i++) if (s->id[i] == sid) return 1;
    return 0;
}
static inline void nvmtl_ws_set_add(nvmtl_ws_set *s, uint32_t sid) {
    if (nvmtl_ws_set_has(s, sid)) return;
    s->id[s->next % NVMTL_WS_SET] = sid;
    s->next = (s->next + 1) % NVMTL_WS_SET;
    if (s->n < NVMTL_WS_SET) s->n++;
}

#endif
