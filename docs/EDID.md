# Fallback EDIDs

When the live EDID read of a monitor fails, VermilionAccel serves a built-in EDID so CoreDisplay still gets display
attributes: `kPhxEDID` in `src/vermilion-accel/accel/phxfb.cpp` (an Acer KG241Q S, HDMI) and `kAocEdid` in
`src/vermilion-accel/accel/phxhead.h` (an AOC 24G2W1G4, DisplayPort).

They are **generic examples of those two models, not anyone's monitor**: in this repository the serial number fields
were cleared (the ID serial number, bytes 12-15, set to 0; the 0xFF "display serial number" descriptor emptied to the
spec's empty string, 0x0A followed by 0x20 padding) and the base block checksum (byte 127) recomputed. Everything else
(manufacturer, product code, timings, ranges, the CTA extension) is unchanged. A monitor's own EDID always wins when
it can be read.
