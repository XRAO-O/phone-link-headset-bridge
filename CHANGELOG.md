# Changelog

## v1.0.0

First public release.

- Bluetooth Hands-Free Audio Gateway on a dedicated USB dongle (BTstack + WinUSB), so a headset keeps its
  microphone while Windows' own Bluetooth is busy with Phone Link.
- mSBC wideband (16 kHz) speech with CVSD (8 kHz) fallback; automatic pairing and reconnection.
- Headset mic is played into a virtual cable; any Windows recording device (e.g. a Voicemeeter bus) is sent
  to the headset, with drift compensation between the Bluetooth and sound card clocks.
- Windows GUI (`headset_bridge_gui.exe`): live status, settings editor with audio device lists,
  "Find headset" Bluetooth search, log view, tray icon and Start with Windows.
- Console bridge (`headset_bridge.exe`) with `--list`, `--scan`, `--log` and `--config`.
- Both executables are self-contained; no extra DLLs needed.

**Before you start:** you need a second USB Bluetooth dongle switched to the WinUSB driver with Zadig, and
VB-Audio Virtual Cable. See the README for setup.

**License note:** the executables include BTstack and are for personal, non-commercial use only.
