#ifndef BRIDGE_SCAN_H
#define BRIDGE_SCAN_H

// Runs a Bluetooth inquiry on the dongle, resolves device names and prints one line per device:
//   Found device AA:BB:CC:DD:EE:FF [audio] Device name
// The GUI parses these lines, so keep the format stable. Call once HCI is working.
void scan_start(void (*on_done)(void));

#endif
