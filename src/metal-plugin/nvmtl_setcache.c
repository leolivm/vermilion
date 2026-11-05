// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by nvmtl_vk.c at this exact place (one translation unit with the plugin;
   definitions keep their order): descriptor-set helpers of nvmtl_snapshot_set_c: the content cache (NVMTL_SETCACHE,
   refuted) and the pipeline-read fast-path gate (NVMTL_FASTGATE, shipped in nvrm610.conf). */
// Drag lag (NVMTL_SETCACHE=1): sets made earlier in this command buffer, keyed by what they hold. Core Animation alternates
// between a few samplers, textures and pipelines; a set with the same contents for every binding the pipeline reads is
// bound again instead of written anew. Sets and the bind table live until the command buffer retires, together.
static int nvmtl_setcache_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SETCACHE"); on = e && *e == '1' && !nvmtl_dynbuf();
        nvlog("vk: descriptor set content cache %s (NVMTL_SETCACHE=%s)", on ? "ON" : "off", e ? e : "unset"); }
    return on;
}
// WP5 (NVMTL_FASTGATE=1): the buffer-only fast path also takes a draw whose changed bindings this pipeline does not read
// (or the last set does not cover as storage); they leave the set's coverage. With bindless textures every texture
// change used to force a new set. Off when DYNBUF or SETCACHE is on.
static int nvmtl_fastgate_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_FASTGATE"); on = e && e[0] == '1' && !nvmtl_dynbuf() && !nvmtl_setcache_on();
                  nvlog("descriptor fast-path gate by pipeline reads %s (NVMTL_FASTGATE)", on ? "ON" : "off"); }
    return on;
}
// A cached set that holds the current contents of every binding in pm; *match gets the bindings of that set that still
// hold the current contents (its coverage from now on).
static const struct nvmtl_setkey *nvmtl_setcache_find(const nvmtl_bindtable *t, uint32_t which, const uint64_t *pm, const uint64_t *tb,
                                                      uint64_t match[4])
{
    for (uint32_t e = 0; e < t->nkey[which] && e < NVMTL_SETCACHE_N; e++) {
        const struct nvmtl_setkey *k = &t->key[which][e];
        int ok = 1;
        for (int w = 0; w < 4 && ok; w++) ok = !(pm[w] & ~k->m[w]) && k->tb[w] == tb[w];
        if (!ok) continue;
        uint64_t mm[4] = { 0, 0, 0, 0 };
        for (uint32_t j = 0; j < k->n; j++) {
            const nvmtl_slot *sl = &t->s[which][k->slot[j]];
            if (sl->kind == k->kind[j] && sl->h == k->h[j] && sl->off == k->off[j]) mm[k->slot[j] >> 6] |= 1ull << (k->slot[j] & 63);
        }
        if (k->m[NVMTL_ADDRESS_BINDING >> 6] >> (NVMTL_ADDRESS_BINDING & 63) & 1) mm[NVMTL_ADDRESS_BINDING >> 6] |= 1ull << (NVMTL_ADDRESS_BINDING & 63);
        for (int w = 0; w < 4 && ok; w++) ok = !(pm[w] & ~mm[w]);
        if (!ok) continue;
        memcpy(match, mm, sizeof mm);
        return k;
    }
    return NULL;
}
static void nvmtl_setcache_note(nvmtl_bindtable *t, uint32_t which, VkDescriptorSet set, unsigned idx, const uint64_t *m,
                                const uint64_t *tb, uint32_t dyn, void *tab)
{
    uint32_t n = 0;
    for (int k = 0; k < 4; k++) n += (uint32_t)__builtin_popcountll(m[k]);
    if (n > NVMTL_SETKEY_SLOTS) return;
    struct nvmtl_setkey *k = &t->key[which][t->nkey[which] % NVMTL_SETCACHE_N];
    t->nkey[which]++;
    k->set = set; k->idx = idx; k->dyn = dyn; k->tab = tab; k->n = 0;
    memcpy(k->m, m, sizeof k->m); memcpy(k->tb, tb, sizeof k->tb);
    for (int w = 0; w < 4; w++)
        for (uint64_t b = m[w]; b; b &= b - 1) {
            const uint32_t i = (uint32_t)w * 64 + (uint32_t)__builtin_ctzll(b);
            if (i == NVMTL_ADDRESS_BINDING) continue;
            const nvmtl_slot *sl = &t->s[which][i];
            k->slot[k->n] = (uint16_t)i; k->kind[k->n] = sl->kind; k->h[k->n] = sl->h; k->off[k->n] = sl->off; k->n++;
        }
}
