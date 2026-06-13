# Cardputer BLE Keyboard

Turn an [M5Stack Cardputer-Adv](https://docs.m5stack.com/en/core/Cardputer-Adv)
into a real Bluetooth Low Energy keyboard for macOS, Android, and any other
host that speaks HID-over-GATT. Pair it once and type into anything — no
companion app, no dongle.

The keyboard ships as **Arduino firmware** (`buddy/firmware/cardputer_ble_kb/`).
It enumerates as a standard BLE HID keyboard with battery reporting, full
modifier support (including ⌘ Command on macOS), and an Fn layer for the
arrow keys.

> **Built — and editable — by talking to Claude.** This whole project was
> developed in conversation with [Claude Code](https://claude.com/claude-code):
> the firmware, the key mappings, the BLE pairing fixes, all of it. You don't
> need to read C++ to change how it behaves. Open this repo in Claude Code and
> just say what you want — *"swap Option and Command", "add an Fn+L lock
> screen shortcut", "make the backlight dimmer"* — and Claude edits the
> firmware, rebuilds, and flashes it to the device for you.

---

## What works

- **Pairs and types** on macOS and Android over BLE (Just-Works pairing, no PIN).
- **Bonding persists** across reboots — power-cycle the Cardputer and it
  reconnects to the last host without re-pairing.
- **Real battery level** reported to the host (updated every ~3 minutes).
- **Modifiers**, mapped with macOS semantics:
  | Cardputer key | Sends |
  |---|---|
  | `ctrl`  | Left Control |
  | `shift` | Left Shift |
  | `opt`   | Left Alt (= Option ⌥) |
  | `alt`   | Left GUI (= Command ⌘) |
- **Fn layer** (Fn is not a HID key; the firmware remaps while it's held):
  | Chord | Sends |
  |---|---|
  | `Fn` + `;` `,` `.` `/` | ↑ ← ↓ → arrows |
  | `Fn` + `` ` `` | Escape |
  | `Fn` + `Backspace` | Forward Delete |

---

## Flashing it

You need [PlatformIO](https://platformio.org/) (`pip install platformio`).

```bash
cd buddy/firmware/cardputer_ble_kb
pio run                                    # build
pio run -t upload --upload-port <PORT>     # flash
```

`<PORT>` is the Cardputer's USB serial port (`/dev/cu.usbmodem*` on macOS,
`/dev/ttyACM*` on Linux, `COMx` on Windows).

**First flash from stock UIFlow firmware** needs the device in download mode.
On the **back** of the Cardputer-Adv: hold **BtnG0**, tap **BtnRST**, release
BtnRST first, then release BtnG0 — the screen goes dark. After that, uploads
auto-reset (no button dance) because the firmware ships USB-CDC-on-boot.

…or just open the repo in Claude Code and say *"flash the keyboard firmware"* —
it handles the build, the port, and the download-mode coaching.

### Pairing

1. On the host, open Bluetooth settings and connect to **`Cardputer KB`**.
2. No PIN dialog appears — that's correct for a Just-Works HID keyboard.
3. Type into any text field.

If the host had paired with an earlier build, **forget the device** in its
Bluetooth settings once and reconnect (the GATT layout changed; hosts cache it).

---

## Why two implementations live here

`buddy/device/apps/ble_keyboard.py` is a **MicroPython version** that runs on
the stock UIFlow 2.0 firmware. It is kept for the record because it does *not*
work as a daily-driver keyboard, and the reason is interesting:

UIFlow 2.0's MicroPython BLE stack can advertise an HID profile and start
pairing, but its **SMP (pairing) layer never completes encryption** — every
attempt ends in `ENCRYPTION_UPDATE enc=0` against macOS, iOS, and Android
alike. The Arduino path fails the same way on the **Bluedroid** stack with a
telltale `smp_derive_link_key_from_long_term_key failed / sm4=0x00`: that's
Bluedroid trying *Cross-Transport Key Derivation* (deriving a Classic-Bluetooth
link key from the BLE LTK) on a chip — the ESP32-S3 — that has **no Classic
radio**, made worse by the common library default of requiring MITM on a
keyboard with no input/output capability.

The fix, and the reason the shipping firmware works, is to use the **NimBLE**
host stack with **Just-Works** security (bonding + Secure Connections, no
MITM, IO capability "none"). NimBLE performs no cross-transport derivation, so
pairing completes cleanly. The full experiment log lives in the docstring of
`ble_keyboard.py` if you want the play-by-play.

---

## Roadmap

- **Multi-host switching** (`Fn`+`1` / `Fn`+`2` to jump between, say, a Mac and
  a phone) — the design is researched: one BLE identity per slot, separate
  bond storage per slot, switch-and-reconnect. Not implemented yet.

(Want one of these? Ask Claude Code in this repo to build it.)

---

## This repo also contains the M5Stack provisioning skill

Before the BLE keyboard, this repo was a one-command provisioner that flashes a
Cardputer-Adv with UIFlow and installs the "Claude Buddy" MicroPython app
bundle. That tooling is still here and still works — the `m5-onboard` Claude
Code skill under `.claude/skills/`, the app bundle under `buddy/device/`, and
its original documentation in **[`README.orig.md`](README.orig.md)**.

Running `m5-onboard go` reflashes UIFlow and the buddy bundle, which is also
exactly how you **revert** a device from this BLE-keyboard firmware back to a
normal Cardputer. The flash is fully reversible.

---

## License

Apache 2.0 — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Third-party
components are listed in [`LICENSE-THIRD-PARTY.md`](LICENSE-THIRD-PARTY.md). The
Arduino firmware additionally pulls in `M5Cardputer`, `NimBLE-Arduino`, and the
`ESP32-BLE-Keyboard` library at build time via PlatformIO; those carry their
own licenses.
