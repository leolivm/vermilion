// SPDX-License-Identifier: MIT
// phx_vkbd.cpp: debug.phxkey: the virtual HID keyboard.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// ---- debug.phxkey: a virtual keyboard ---------------------------------------------------------------------------
// Typing through CGEventPost from ssh did not reach the login window's password field, and osascript has no session
// there (2026-10-25). A HID device of our own is a keyboard like any other to the HID event system, so its keys reach
// the login window and every app with no Accessibility grant: `sudo sysctl -w debug.phxkey='text{RET}'` types text
// (US layout) and {NAME} keys - RET ESC TAB BS SPACE UP DOWN LEFT RIGHT PGUP PGDN HOME END F1..F12 - with optional
// cmd+ ctrl+ alt+ shift+ prefixes inside the braces ({cmd+q}). Made on first use; writing the sysctl needs root.
class PhxVirtualKeyboard : public IOHIDDevice {
    OSDeclareDefaultStructors(PhxVirtualKeyboard)
public:
    IOReturn newReportDescriptor(IOMemoryDescriptor **d) const override {
        static const uint8_t k[] = { 0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                     // generic desktop, keyboard
                                     0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
                                     0x75, 0x01, 0x95, 0x08, 0x81, 0x02,                     // 8 modifier bits
                                     0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                     // reserved byte
                                     0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
                                     0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,   // 6 key slots
                                     0xC0 };
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withBytes(k, sizeof k, kIODirectionNone);
        if (!b) return kIOReturnNoMemory;
        *d = b;
        return kIOReturnSuccess;
    }
    OSString *newTransportString() const override { return OSString::withCString("Virtual"); }
    OSString *newManufacturerString() const override { return OSString::withCString("PhxBringup"); }
    OSString *newProductString() const override { return OSString::withCString("Phx Virtual Keyboard"); }
    OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(0x16c0, 32); }
    OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(0x27db, 32); }
    OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(1, 32); }
    OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(6, 32); }
    OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x50480000, 32); }
    void press(uint8_t mods, uint8_t key) {
        const uint8_t down[8] = { mods, 0, key, 0, 0, 0, 0, 0 }, up[8] = { 0 };
        const uint8_t *const reports[2] = { down, up };
        for (const uint8_t *r : reports) {
            if (IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withBytes(r, 8, kIODirectionNone)) {
                IOHIDDevice::handleReport(b, kIOHIDReportTypeInput, 0);   // by symbol, not through a vtable slot the SDK header may place differently
                b->release();
            }
            IOSleep(15);
        }
    }
};
OSDefineMetaClassAndStructors(PhxVirtualKeyboard, IOHIDDevice)

static PhxVirtualKeyboard *gPhxKbd;
static IOLock *gPhxKbdLock;
// US-layout usage for one printable ASCII character; shift set when it needs it. 0 = not typable.
static uint8_t phxKeyUsage(char c, bool *shift) {
    static const char kShifted[] = "!@#$%^&*()", kPlain[] = "1234567890";
    *shift = false;
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') { *shift = true; return static_cast<uint8_t>(0x04 + (c - 'A')); }
    for (int i = 0; i < 10; i++) {
        if (c == kPlain[i]) return static_cast<uint8_t>(0x1E + i);
        if (c == kShifted[i]) { *shift = true; return static_cast<uint8_t>(0x1E + i); }
    }
    static const struct { char plain, shifted; uint8_t usage; } kPunct[] = {
        { ' ', 0, 0x2C }, { '-', '_', 0x2D }, { '=', '+', 0x2E }, { '[', '{', 0x2F }, { ']', '}', 0x30 }, { '\\', '|', 0x31 },
        { ';', ':', 0x33 }, { '\'', '"', 0x34 }, { '`', '~', 0x35 }, { ',', '<', 0x36 }, { '.', '>', 0x37 }, { '/', '?', 0x38 } };
    for (const auto &p : kPunct) {
        if (c == p.plain) return p.usage;
        if (p.shifted && c == p.shifted) { *shift = true; return p.usage; }
    }
    return 0;
}
static uint8_t phxKeyNamed(const char *n, size_t len) {
    static const struct { const char *name; uint8_t usage; } kNames[] = {
        { "RET", 0x28 }, { "ESC", 0x29 }, { "BS", 0x2A }, { "TAB", 0x2B }, { "SPACE", 0x2C }, { "RIGHT", 0x4F }, { "LEFT", 0x50 },
        { "DOWN", 0x51 }, { "UP", 0x52 }, { "PGUP", 0x4B }, { "PGDN", 0x4E }, { "HOME", 0x4A }, { "END", 0x4D } };
    for (const auto &k : kNames)
        if (strlen(k.name) == len && !strncasecmp(n, k.name, len)) return k.usage;
    if ((len == 2 || len == 3) && (n[0] == 'F' || n[0] == 'f')) {
        const int f = (len == 2) ? n[1] - '0' : (n[1] - '0') * 10 + (n[2] - '0');
        if (f >= 1 && f <= 12) return static_cast<uint8_t>(0x3A + f - 1);
    }
    if (len == 1) { bool sh; const char c = (n[0] >= 'A' && n[0] <= 'Z') ? static_cast<char>(n[0] - 'A' + 'a') : n[0]; return phxKeyUsage(c, &sh); }
    return 0;
}
static int phxkey_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    char buf[256] = { 0 };
    int err = sysctl_handle_string(oidp, buf, sizeof buf, req);
    if (err || !req->newptr) return err;
    if (!gPhx) return ENXIO;
    if (!gPhxKbdLock) gPhxKbdLock = IOLockAlloc();
    if (!gPhxKbdLock) return ENOMEM;
    IOLockLock(gPhxKbdLock);
    if (!gPhxKbd) {
        PhxVirtualKeyboard *k = OSTypeAlloc(PhxVirtualKeyboard);
        if (k && k->init() && k->attach(gPhx)) {
            if (k->start(gPhx)) { gPhxKbd = k; IOSleep(500); }   // let the HID event system pick the new keyboard up
            else k->detach(gPhx);
        }
        if (!gPhxKbd && k) k->release();
    }
    PhxVirtualKeyboard *kb = gPhxKbd;
    if (!kb) { IOLockUnlock(gPhxKbdLock); return ENODEV; }
    for (const char *p = buf; *p; p++) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) { err = EINVAL; break; }
            const char *n = p + 1;
            uint8_t mods = 0;
            for (;;) {
                const char *plus = static_cast<const char *>(memchr(n, '+', static_cast<size_t>(e - n)));
                if (!plus || plus == e - 1) break;
                const size_t ml = static_cast<size_t>(plus - n);
                if (ml == 3 && !strncasecmp(n, "cmd", 3)) mods |= 0x08;
                else if (ml == 4 && !strncasecmp(n, "ctrl", 4)) mods |= 0x01;
                else if (ml == 3 && !strncasecmp(n, "alt", 3)) mods |= 0x04;
                else if (ml == 5 && !strncasecmp(n, "shift", 5)) mods |= 0x02;
                else break;
                n = plus + 1;
            }
            const uint8_t u = phxKeyNamed(n, static_cast<size_t>(e - n));
            if (!u) { err = EINVAL; break; }
            kb->press(mods, u);
            p = e;
            continue;
        }
        bool shift;
        const uint8_t u = phxKeyUsage(*p, &shift);
        if (!u) { err = EINVAL; break; }
        kb->press(shift ? 0x02 : 0, u);
    }
    IOLockUnlock(gPhxKbdLock);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxkey, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxkey_sysctl, "A",
            "type on PhxBringup's virtual keyboard: text and {RET} {ESC} {PGDN} {F11} {cmd+q} ... (root)");

