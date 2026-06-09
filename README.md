# Teams Mute — Flipper Zero BLE mute remote

A one-button [Flipper Zero](https://flipperzero.one/) app (FAP) that pairs to a
PC as a **Bluetooth HID keyboard** and sends **Win+Alt+K** — the Windows 11
system-wide taskbar microphone toggle — so you can mute/unmute Microsoft Teams
(or anything using the mic) from across the room, regardless of the focused
window.

* **OK (tap)** → `Win+Alt+K` — toggle the Windows system mic mute
* **OK (hold)** → `Ctrl+Space` held — push-to-talk (Teams "hold to unmute")
* **Up (tap)** → `Ctrl+Shift+M` — Teams in-app mute shortcut (fallback)
* **Back** → exit (restores the Flipper's normal Bluetooth profile)

The screen shows the BLE connection status and a count of keystrokes sent. HID
is one-way, so there is no real mute-state readout — only a send counter.

## Target firmware

Built and validated against **official Flipper firmware**, ufbt `release`
channel — **1.4.3 / API 87.1**, target `f7`. See "Why the profile is vendored"
for why the firmware fork matters.

## Build

```sh
pip install --upgrade ufbt
ufbt update                 # official release SDK (default channel)
ufbt                        # build -> dist/teams_mute.fap
```

The app builds clean under the SDK's `-Wall -Wextra -Werror` and passes the FAP
API symbol check (`APPCHK`).

## Flash & run (do this on your machine — needs the physical Flipper)

```sh
ufbt launch                 # build, install to /ext/apps/Bluetooth, and run
```

Or copy `dist/teams_mute.fap` to the SD card under
`apps/Bluetooth/` with qFlipper and launch it from
`Apps → Bluetooth → Teams Mute`.

### Pair with Windows

1. Launch the app on the Flipper. It starts advertising; the screen shows
   `BT: Advertising`.
2. On the PC: **Settings → Bluetooth & devices → Add device → Bluetooth**, pick
   the **"Mute …"** device and confirm the pairing prompt.
3. When the screen shows `BT: Connected`, tap **OK** — Windows toggles the mic
   (you'll see the taskbar mic indicator flip).

> The app keeps its BLE bonding keys in its own file
> (`/ext/apps_data/teams_mute/teams_mute.keys`), so pairing this remote does
> **not** disturb the Flipper's normal Bluetooth bonds (qFlipper, mobile app).
> Exiting restores the default BLE profile.

## Why the profile is vendored

The clean approach would be to call the firmware's ready-made BLE HID keyboard
profile (`ble_profile_hid` + `ble_profile_hid_kb_press/release`). **That API is
deliberately *not* exposed to third-party FAPs** — it is marked disabled in the
firmware's public API symbol table on **official, Unleashed *and* Momentum**.
The stock "Bluetooth Remote" app only gets to use it because it is compiled
*inside* the firmware. A FAP that links those symbols builds, but the on-device
loader rejects it with a "Missing API" error.

What *is* exposed to FAPs is the **generic GATT API** (`ble_gatt_service_add`,
`ble_gatt_characteristic_init/update`) plus custom-profile registration via
`bt_profile_start()`. So this app **vendors the firmware's own, proven HID
profile + HID-over-GATT service** and builds them on those public primitives:

| File | Origin | Change |
|------|--------|--------|
| `teams_mute_hid_service.{c,h}` | firmware `lib/ble_profile/extra_services/hid_service.c` | include swaps; HID UUIDs defined locally (SDK omits the ST UUID headers); the HCI-event handler is a no-op (the raw ST event types aren't exposed — we only *send* notifications, so events defer to the firmware default handler) |
| `teams_mute_hid_profile.{c,h}` | firmware `lib/ble_profile/extra_profiles/hid_profile.c` | include swaps only |
| `teams_mute.c` | this app | the UI + input/BLE lifecycle |

Battery and Device-Information services *are* exposed to FAPs, so those are
still called from the firmware rather than vendored.

### Alternatives considered

* **USB HID** (`furi_hal_hid_*`) *is* available to FAPs on official firmware —
  trivial, but wired (Flipper plugged into the PC), which defeats the
  across-the-room goal.
* On a fork (Unleashed/Momentum/RogueMaster) the bundled **Bad BLE** app runs
  DuckyScript over BLE; a one-line payload `GUI ALT k` does the same job with no
  custom code — but requires reflashing to a fork.

## Status / caveats

* **Compiles, links, and passes `APPCHK`** against official 1.4.3. ✅
* The vendored BLE stack is the firmware's own proven code (same as the stock
  Bluetooth Remote), so behaviour should match a known-good HID device. **BLE
  pairing was not tested on hardware** as part of this build — validate the
  first pairing on your Flipper + PC.

## Tweaking

Keystrokes are plain HID keycodes in `teams_mute.c` (modifier bits in the high
byte, HID usage in the low byte — see `KEY_TEAMS_TOGGLE` etc.). The advertised
name prefix is `TEAMS_MUTE_NAME_PREFIX` (must be < 8 chars).
