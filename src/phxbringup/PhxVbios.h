// SPDX-License-Identifier: MIT
// PhxVbios.h: the board's display connectors from the VBIOS (AtomBIOS display object table v1.4/v1.5), kernel-free so
// a host test runs it on a dump of this card's VBIOS. Follows Linux dc/bios/bios_parser2.c and
// atomfirmware.h (as RedNavi's tools/parse_vbios_objects.py does). Every read is bounds-checked: the image comes
// from the card and a bad one must give 0 connectors, never a fault.
//
// The rules that turn a path into our sources (checked on this board):
//   HPD index = HPD record pin id - 1; DDC/AUX index = I2C record line id & 0xF (0x90 -> DDC line 0 = DDC1, 0x92 -> AUX2);
//   transmitter (= DIG back end on DCN 3.2) = base(encoder id) + enum - 1, base UNIPHY 0x1E -> 0 (A/B), UNIPHY1 0x20 -> 2
//   (C/D), UNIPHY2 0x21 -> 4 (E/F), UNIPHY3 0x22 -> 6.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxvbios {

enum : uint8_t { kOther = 0, kDP = 1, kHDMI = 2 };
struct Connector {
    uint8_t path;          // display path index in the table
    uint8_t type;          // kDP / kHDMI / kOther
    uint8_t hpd;           // HPD index (0-based, HPDn_DC_HPD_* registers); 0xFF = none
    uint8_t line;          // DP: AUX engine; HDMI: DC_I2C DDC line (0 = DDC1); 0xFF = none
    int8_t transmitter;    // 0 = A ... 5 = F (DIG back end), -1 = unknown
    uint16_t connectorId;  // ATOM object ids, for logs
    uint16_t encoderId;
};

inline bool rd16(const uint8_t *b, size_t len, size_t o, uint16_t *v) {
    if (o + 2 > len) return false;
    *v = static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
    return true;
}

inline int8_t transmitterOf(uint16_t encoderId) {
    const unsigned id = encoderId & 0xFF, en = (encoderId >> 8) & 0xF;
    int base = id == 0x1E ? 0 : id == 0x20 ? 2 : id == 0x21 ? 4 : id == 0x22 ? 6 : -1;
    if (base < 0 || en < 1 || en > 2) return -1;
    return static_cast<int8_t>(base + static_cast<int>(en) - 1);
}

// Parses up to `max` connectors into `out`; returns the count (0 = no usable table).
inline unsigned parse(const uint8_t *b, size_t len, Connector *out, unsigned max) {
    uint16_t rom = 0, master = 0, dobj = 0;
    if (!b || !out || !rd16(b, len, 0x48, &rom) || rom + 8 > len) return 0;
    if (b[rom + 4] != 'A' || b[rom + 5] != 'T' || b[rom + 6] != 'O' || b[rom + 7] != 'M') return 0;
    if (!rd16(b, len, rom + 32, &master) || !master) return 0;
    if (!rd16(b, len, master + 4 + 2 * 22, &dobj) || !dobj || dobj + 8 > len) return 0;   // data table 22 = displayobjectinfo
    const uint8_t crev = b[dobj + 3];
    if (b[dobj + 2] != 1 || (crev != 4 && crev != 5)) return 0;
    const unsigned npath = b[dobj + 6];
    unsigned n = 0;
    for (unsigned i = 0; i < npath && n < max; i++) {
        const size_t p = dobj + 8 + 16u * i;
        uint16_t oid = 0, rec = 0, enc = 0;
        if (!rd16(b, len, p, &oid) || !rd16(b, len, p + 2, &rec) || !rd16(b, len, p + 4, &enc)) break;
        Connector c{static_cast<uint8_t>(i), kOther, 0xFF, 0xFF, transmitterOf(enc), oid, enc};
        const unsigned ctype = (oid >> 12) & 7, cid = oid & 0xFF;
        if (ctype == 3) c.type = cid == 19 ? kDP : cid == 12 ? kHDMI : kOther;   // CONNECTOR DISPLAY_PORT / HDMI_TYPE_A
        if (rec) {
            size_t r = static_cast<size_t>(dobj) + rec;
            for (unsigned guard = 0; guard < 32 && r + 2 <= len; guard++) {
                const uint8_t rt = b[r], rs = b[r + 1];
                if (rt == 0xFF || rs < 2 || r + rs > len) break;
                if (rt == 1 && rs >= 4) c.line = b[r + 2] & 0xF;                          // I2C record: line id
                if (rt == 2 && rs >= 4 && b[r + 2] >= 1) c.hpd = static_cast<uint8_t>(b[r + 2] - 1);   // HPD record: pin id
                r += rs;
            }
        }
        out[n++] = c;
    }
    return n;
}

// The debug.phxedid_src value for a connector: DP -> AUX engine (0x20 + n), HDMI -> DDC line (0x10 + n); -1 = none.
inline int edidSource(const Connector &c) {
    if (c.line == 0xFF) return -1;
    return c.type == kDP ? 0x20 + c.line : c.type == kHDMI ? 0x10 + c.line : -1;
}
// The connector whose HPD index is `hpd`, or -1.
inline int byHpd(const Connector *cs, unsigned n, unsigned hpd) {
    for (unsigned i = 0; i < n; i++) if (cs[i].hpd == hpd) return static_cast<int>(i);
    return -1;
}

}  // namespace phxvbios
