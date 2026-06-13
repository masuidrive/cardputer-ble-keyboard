"""BLE HID keyboard (HOGP) for the Cardputer-Adv — turns the device
into a Bluetooth keyboard for a host computer (developed against
macOS, should work with any HOGP central).

### What this is, and the experiment it carries

A single-file HID-over-GATT keyboard: HID service 0x1812 with a
standard 8-byte keyboard report, Battery and Device Information
services, and an advertising payload that declares keyboard
appearance (0x03C1) so hosts render the right icon and apply HID
pairing flows.

The open question on UIFlow 2.0 is security. macOS requires
bonding+encryption for HID peripherals, and this build's MicroPython
pairing API is known-broken at the Python layer (`buddy_ble.py`
documents that even *making* bond/mitm/le_secure/io config calls
wedges adv_data acceptance). What nobody has tested is whether the
NimBLE C layer can complete a *central-initiated* Just Works pairing
with zero Python-side involvement. This app is instrumented to find
out: every BLE IRQ is printed to USB serial, so a pairing attempt
from macOS leaves a full trace of which SMP events (if any) fire.

``SEC_MODE`` at the top selects one experiment per boot:

    0  no pairing config at all — let the central drive SMP
    1  config(bond=True) before service registration
    2  bond=True + ENCRYPTED flag on the input report + gap_pair()
       on connect (default after modes 0/1 ran clean but silent)
    3  mode 2 plus le_secure/mitm/io config knobs

Field results: modes 0 and 1 both advertise fine (no wedge — the
buddy_ble warning evidently applies to a different call pattern),
macOS connects and walks the GATT table, but never starts SMP and
silently discards HID input. Diagnosis: nothing on our side *demands*
security — open characteristics give macOS's CoreBluetooth layer no
reason to pair, while its HID layer refuses unencrypted input. Mode 2
fixes both ends: encrypted-access flags on the report characteristics
(host hits Insufficient Encryption → its SMP engine engages) plus a
peripheral-initiated gap_pair security request. **Confirmed working
2026-06-11 on UIFlow 2.0 v2.4.6 / Cardputer-Adv against macOS:**
ENCRYPTION_UPDATE enc=1 bonded=1 key_size=16, keystrokes delivered.
The GET_SECRET/SET_SECRET key-store IRQs fire on this build too, so
bonds are persisted to /flash/blekb_bonds.json — reboots reconnect
without re-pairing. The "UIFlow 2.0 has no pairing" lore in
buddy_ble.py is wrong for this build; what it lacks is only the
*config knobs* working pre-advertise (appearance is rejected, the
rest now demonstrably work).

### Input model

MatrixKeyboard delivers press events only — no key-up, no modifier
state (Shift arrives pre-applied as shifted ASCII). So every key is
sent tap-style: one report with the usage+modifiers, immediately
followed by an all-zero release report. Good for typing; held keys
and chords are out of scope for this build (the Arduino fallback
documented in the repo plan gets real key-up events).

Exit gesture: **Fn+Q** (advertised as such; technically any Fn
chord). The driver collapses every Fn+key combination to int 0 —
measured on-device, and the API surface (get_key/get_string/
is_pressed/set_callback/tick, no keys-state access) offers nothing
finer — so Fn+Q cannot be told apart from Fn+anything. Plain ESC
forwards to the host like any other key, full speed.

### BLE bring-up rules inherited from buddy_ble.py (load-bearing)

- BLE() → 300 ms → active(True) → 250 ms → config(gap_name) →
  gatts_register_services → gap_advertise. Reordering risks
  OSError(16)/(−519) or a controller C-fault.
- No gatts_set_buffer anywhere — we never receive large writes, so
  the −519 trap is avoided entirely rather than worked around.
- Never active(False): panics the controller on this build.
- Re-advertise after disconnect via micropython.schedule with a
  150-750 ms staircase (immediate gap_advertise gets OSError(-30)).
"""

import binascii
import json
import struct
import time

import bluetooth
import M5
import machine
import micropython
from hardware import MatrixKeyboard
from micropython import const


# ---- experiment knob: one security mode per boot (see module docstring)
SEC_MODE = 2


_IRQ_CENTRAL_CONNECT = const(1)
_IRQ_CENTRAL_DISCONNECT = const(2)
_IRQ_GATTS_WRITE = const(3)
_IRQ_GATTS_READ_REQUEST = const(4)
_IRQ_MTU_EXCHANGED = const(21)
_IRQ_CONNECTION_UPDATE = const(27)
_IRQ_ENCRYPTION_UPDATE = const(28)
_IRQ_GET_SECRET = const(29)
_IRQ_SET_SECRET = const(30)
_IRQ_PASSKEY_ACTION = const(31)

_PASSKEY_ACTION_INPUT = const(2)
_PASSKEY_ACTION_DISP = const(3)
_PASSKEY_ACTION_NUMCMP = const(4)

_F_READ = const(0x0002)
_F_WRITE_NR = const(0x0004)
_F_WRITE = const(0x0008)
_F_NOTIFY = const(0x0010)
# Encrypted-access flags. The UIFlow build doesn't export these
# constants from the bluetooth module, but flags are plain ints fed
# to NimBLE — if the underlying stack honors them, hardcoding works.
_F_READ_ENC = const(0x0200)
_F_WRITE_ENC = const(0x2000)

# ---- palette (suite standard, inlined like snake.py)
_BLACK = 0x000000
_ORANGE = 0xCC785C
_CREAM = 0xF0EEE6
_DARK = 0x1F1F1F
_GRAY_MID = 0x777777
_GREEN = 0x33CC66

_LCD = M5.Lcd
_W = 240
_H = 135

_BONDS_PATH = "/flash/blekb_bonds.json"

# The exit chord. MatrixKeyboard reports every Fn+key combo as 0,
# so this is "Fn+Q" in the UI and "any Fn chord" in reality.
_EXIT_KEY = const(0)

# Diagnostic toggle: when False the secret-store IRQs answer exactly
# like the one build that ever paired successfully (GET → None,
# SET → False, nothing stored). Pairing failed on every build that
# answered SET_SECRET with True — suspicion is that True engages
# NimBLE's half-broken persistence path (micropython#12958) and
# wrecks key distribution. False = no bond survives a reboot.
_BOND_STORE = False

# ---- HID report map: standard keyboard, Report ID 1.
# 8-byte input (modifier bits, reserved, 6-key array) + 5-bit LED
# output with 3 bits padding. Notify payloads omit the Report ID —
# HOGP carries it in the Report Reference descriptor instead.
_REPORT_MAP = bytes([
    0x05, 0x01,        # Usage Page (Generic Desktop)
    0x09, 0x06,        # Usage (Keyboard)
    0xA1, 0x01,        # Collection (Application)
    0x85, 0x01,        #   Report ID (1)
    0x05, 0x07,        #   Usage Page (Key Codes)
    0x19, 0xE0,        #   Usage Minimum (LeftControl)
    0x29, 0xE7,        #   Usage Maximum (RightGUI)
    0x15, 0x00,        #   Logical Minimum (0)
    0x25, 0x01,        #   Logical Maximum (1)
    0x75, 0x01,        #   Report Size (1)
    0x95, 0x08,        #   Report Count (8)
    0x81, 0x02,        #   Input (Data, Variable, Absolute) — modifiers
    0x95, 0x01,        #   Report Count (1)
    0x75, 0x08,        #   Report Size (8)
    0x81, 0x01,        #   Input (Constant) — reserved byte
    0x95, 0x05,        #   Report Count (5)
    0x75, 0x01,        #   Report Size (1)
    0x05, 0x08,        #   Usage Page (LEDs)
    0x19, 0x01,        #   Usage Minimum (Num Lock)
    0x29, 0x05,        #   Usage Maximum (Kana)
    0x91, 0x02,        #   Output (Data, Variable, Absolute) — LEDs
    0x95, 0x01,        #   Report Count (1)
    0x75, 0x03,        #   Report Size (3)
    0x91, 0x01,        #   Output (Constant) — LED padding
    0x95, 0x06,        #   Report Count (6)
    0x75, 0x08,        #   Report Size (8)
    0x15, 0x00,        #   Logical Minimum (0)
    0x25, 0x65,        #   Logical Maximum (101)
    0x05, 0x07,        #   Usage Page (Key Codes)
    0x19, 0x00,        #   Usage Minimum (0)
    0x29, 0x65,        #   Usage Maximum (101)
    0x81, 0x00,        #   Input (Data, Array) — 6-key rollover
    0xC0,              # End Collection
])

# ---- GATT table. CCCDs (0x2902) are auto-generated by MicroPython
# for FLAG_NOTIFY characteristics — registering them manually creates
# a second dead CCCD and breaks host subscription. Report Reference
# descriptors (0x2908) we do register, read-only.
_U = bluetooth.UUID


def _build_services(encrypted):
    """GATT table, optionally with encrypted-access HID characteristics.

    With ``encrypted`` the HID report characteristics demand an
    encrypted link, which is the standard trigger for host-initiated
    pairing: the host's read/subscribe bounces with Insufficient
    Encryption and its SMP engine takes over. Report Map and the
    DIS/Battery services stay open — hosts read those during discovery
    before pairing, and locking them just breaks discovery.
    """
    enc_r = _F_READ_ENC if encrypted else 0
    enc_w = _F_WRITE_ENC if encrypted else 0
    hid = (_U(0x1812), (
        (_U(0x2A4A), _F_READ),                                   # HID Information
        (_U(0x2A4B), _F_READ),                                   # Report Map
        (_U(0x2A4D), _F_READ | _F_NOTIFY | enc_r,
            ((_U(0x2908), _F_READ),)),                           # Input Report
        (_U(0x2A4D), _F_READ | _F_WRITE | _F_WRITE_NR | enc_r | enc_w,
            ((_U(0x2908), _F_READ),)),                           # Output Report (LEDs)
        (_U(0x2A4E), _F_READ | _F_WRITE_NR),                     # Protocol Mode
        (_U(0x2A4C), _F_WRITE_NR),                               # HID Control Point
    ))
    bat = (_U(0x180F), ((_U(0x2A19), _F_READ | _F_NOTIFY),))
    dis = (_U(0x180A), (
        (_U(0x2A50), _F_READ),                                   # PnP ID
        (_U(0x2A29), _F_READ),                                   # Manufacturer Name
    ))
    return (hid, bat, dis)


def _mac_suffix(mac_bytes):
    return "".join("{:02X}".format(b) for b in mac_bytes[-3:])


# ---- ASCII → (usage, modifiers). US layout. Shift = 0x02.
def _build_keymap():
    m = {}
    for i in range(26):
        m[chr(0x61 + i)] = (0x04 + i, 0x00)   # a-z
        m[chr(0x41 + i)] = (0x04 + i, 0x02)   # A-Z
    for i in range(9):
        m[chr(0x31 + i)] = (0x1E + i, 0x00)   # 1-9
    m["0"] = (0x27, 0x00)
    for i, ch in enumerate("!@#$%^&*()"):     # shifted digit row
        m[ch] = (0x1E + i, 0x02)
    plain = {
        " ": 0x2C, "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30,
        "\\": 0x31, ";": 0x33, "'": 0x34, "`": 0x35, ",": 0x36,
        ".": 0x37, "/": 0x38,
    }
    shifted = {
        "_": 0x2D, "+": 0x2E, "{": 0x2F, "}": 0x30, "|": 0x31,
        ":": 0x33, '"': 0x34, "~": 0x35, "<": 0x36, ">": 0x37,
        "?": 0x38,
    }
    for ch, usage in plain.items():
        m[ch] = (usage, 0x00)
    for ch, usage in shifted.items():
        m[ch] = (usage, 0x02)
    return m


_KEYMAP = _build_keymap()

# Non-printable int codes from MatrixKeyboard. Enter is 0x0A on this
# firmware (main.py's launcher established that); 0x0D accepted in
# case a future build flips it back. 180-183 are the arrow ints
# upstream uiflow-micropython emits — unconfirmed on this build (the
# arrow cluster reports glyph ASCII here), mapped anyway since it
# costs four dict entries and self-activates if the driver changes.
_INT_KEYMAP = {
    0x0A: (0x28, 0x00),   # Enter
    0x0D: (0x28, 0x00),   # Enter (CR variant)
    0x08: (0x2A, 0x00),   # Backspace
    0x09: (0x2B, 0x00),   # Tab
    0x1B: (0x29, 0x00),   # Escape (exit-gesture aware in the loop)
    0x7F: (0x4C, 0x00),   # Delete forward
    180: (0x50, 0x00),    # Left arrow
    181: (0x52, 0x00),    # Up arrow
    182: (0x51, 0x00),    # Down arrow
    183: (0x4F, 0x00),    # Right arrow
}


_EVENT_NAMES = {
    1: "CENTRAL_CONNECT", 2: "CENTRAL_DISCONNECT", 3: "GATTS_WRITE",
    4: "GATTS_READ_REQUEST", 21: "MTU_EXCHANGED", 27: "CONNECTION_UPDATE",
    28: "ENCRYPTION_UPDATE", 29: "GET_SECRET", 30: "SET_SECRET",
    31: "PASSKEY_ACTION",
}


class HIDKeyboard:
    """HOGP keyboard peripheral with full IRQ tracing.

    Unlike buddy_ble there is no singleton dance: this app owns the
    whole boot (launcher relaunches us via machine.reset()), so the
    one-registration-per-boot constraint is satisfied by design.
    """

    def __init__(self):
        # UI-facing state, polled by the main loop (never painted
        # from IRQ context — IRQ sets, loop paints).
        self.state = "init"          # init/advertising/connected/encrypted
        self.dirty = True
        self.last_sent = ""
        self.passkey_prompt = None   # set if host asks for passkey entry
        self._passkey_conn = None
        self._passkey_action = None

        self._conn = None
        self._conn_ms = 0
        self._pair_requested = False
        self._encrypted = False
        self._shutting_down = False

        # Bond key store, fed by the GET/SET_SECRET IRQs (which do
        # fire on this build — verified). Loaded into RAM before BLE
        # bring-up so GET_SECRET can answer synchronously; writes are
        # flagged dirty here and flushed by the main loop, because
        # flash I/O inside IRQ/scheduler context is asking for
        # filesystem corruption.
        self._secrets = {}
        self._secrets_dirty = False
        self._load_secrets()

        print("blekb: SEC_MODE =", SEC_MODE)
        ble = bluetooth.BLE()
        # Same wall-time pauses as buddy_ble: active(True) right after
        # BLE() can C-fault the controller, config can race init.
        time.sleep_ms(300)
        try:
            pre_active = ble.active()
        except Exception:
            pre_active = False
        if not pre_active:
            ble.active(True)
        time.sleep_ms(250)
        self._ble = ble

        mac = ble.config("mac")[1]
        self.name = "CPadKB-{}".format(_mac_suffix(mac))
        ble.config(gap_name=self.name)

        # GAP appearance characteristic — unverified on this build.
        # The appearance AD in the advertising payload is what hosts
        # actually scan on; this is belt-and-braces and guarded.
        try:
            ble.config(appearance=0x03C1)
            print("blekb: config(appearance) ok")
        except Exception as e:
            print("blekb: config(appearance) rejected:", e)

        if SEC_MODE >= 1:
            self._try_sec_config(bond=True)
        if SEC_MODE >= 3:
            # io=2 (KeyboardOnly) → MITM pairing becomes Passkey
            # Entry: the host displays a 6-digit code, the user types
            # it on this keyboard (PASSKEY_ACTION INPUT → the digit-
            # collection UI in the main loop). Needed because macOS
            # accepts Just Works from a never-seen keyboard exactly
            # once; re-pairing a forgotten keyboard at the same
            # address demands MITM, and a NoInput peripheral fails
            # that with enc=0 before key distribution ever starts.
            self._try_sec_config(io=2)

        handles = None
        if SEC_MODE >= 2:
            # Encrypted-flag registration may be rejected by the
            # stripped build (flags are ints, so it may also just
            # work). Fall back to the open table rather than dying.
            try:
                handles = ble.gatts_register_services(_build_services(True))
                print("blekb: registered with ENCRYPTED hid flags")
            except (OSError, ValueError) as e:
                print("blekb: encrypted-flag registration rejected:", e)
        if handles is None:
            handles = ble.gatts_register_services(_build_services(False))
        ((self._h_info, self._h_map, self._h_in, self._h_in_ref,
          self._h_out, self._h_out_ref, self._h_proto, self._h_ctrl),
         (self._h_bat,),
         (self._h_pnp, self._h_mfg)) = handles
        print("blekb: services registered:", handles)

        self._write_static_values()

        self._ble.irq(self._irq)
        self._advertise()
        self.state = "advertising"
        self.dirty = True

        if SEC_MODE == 3:
            # The untested quadrant: pairing knob AFTER a successful
            # advertise, then re-arm. If the knob wedges the stack the
            # re-advertise cascade's shape label will show it.
            self._try_sec_config(bond=True)
            try:
                self._ble.gap_advertise(None)
            except OSError:
                pass
            time.sleep_ms(150)
            self._advertise()

    # ---- bond persistence (official ble_bonding_peripheral.py protocol)

    def _load_secrets(self):
        try:
            with open(_BONDS_PATH) as f:
                data = json.load(f)
            for k, v in data.items():
                sec_type, key_hex = k.split("|", 1)
                self._secrets[(int(sec_type), binascii.unhexlify(key_hex))] = \
                    binascii.unhexlify(v)
            print("blekb: loaded", len(self._secrets), "bond secrets")
        except OSError:
            pass  # no bond store yet — first boot
        except (ValueError, KeyError) as e:
            print("blekb: bond store corrupt, starting fresh:", e)
            self._secrets = {}

    def save_secrets(self):
        self._secrets_dirty = False
        data = {}
        for (sec_type, key), value in self._secrets.items():
            data["{}|{}".format(sec_type, binascii.hexlify(key).decode())] = \
                binascii.hexlify(value).decode()
        try:
            with open(_BONDS_PATH, "w") as f:
                json.dump(data, f)
            print("blekb: saved", len(data), "bond secrets")
        except OSError as e:
            print("blekb: bond save failed:", e)

    def _try_sec_config(self, **kwargs):
        # Known risk: these knobs wedge adv_data acceptance on this
        # build (buddy_ble's day-long lesson). Each is individually
        # guarded so we learn exactly which call dies.
        for key, val in kwargs.items():
            try:
                self._ble.config(**{key: val})
                print("blekb: config({}={}) ok".format(key, val))
            except Exception as e:
                print("blekb: config({}={}) rejected: {}".format(key, val, e))

    def _write_static_values(self):
        b = self._ble
        b.gatts_write(self._h_info, b"\x11\x01\x00\x02")  # HID 1.11, NormallyConnectable
        b.gatts_write(self._h_map, _REPORT_MAP)
        b.gatts_write(self._h_in, b"\x00" * 8)
        b.gatts_write(self._h_in_ref, b"\x01\x01")        # Report ID 1, Input
        b.gatts_write(self._h_out_ref, b"\x01\x02")       # Report ID 1, Output
        b.gatts_write(self._h_proto, b"\x01")             # Report Protocol
        b.gatts_write(self._h_bat, b"\x64")               # 100%
        # PnP ID: vendor source 0x02 (USB-IF), Espressif VID 0x303A,
        # arbitrary product 0x0001, version 1.0.
        b.gatts_write(self._h_pnp, struct.pack("<BHHH", 0x02, 0x303A, 0x0001, 0x0100))
        b.gatts_write(self._h_mfg, b"M5Stack")

    # ---- advertising (cascade pattern from buddy_ble._advertise)

    def _advertise(self):
        flags_ad = b"\x02\x01\x06"
        uuid_ad = b"\x03\x03\x12\x18"        # Complete 16-bit UUIDs: 0x1812
        appear_ad = b"\x03\x19\xC1\x03"      # Appearance: keyboard
        name_bytes = self.name.encode()
        name_ad = bytes([len(name_bytes) + 1, 0x09]) + name_bytes

        candidates = [
            ("adv=flags+uuid+appear resp=name",
                {"adv_data": flags_ad + uuid_ad + appear_ad, "resp_data": name_ad}),
            # This build's controller auto-adds Flags in at least some
            # states; duplicated Flags AD can cause rejection.
            ("adv=uuid+appear resp=name",
                {"adv_data": uuid_ad + appear_ad, "resp_data": name_ad}),
            ("adv=flags+uuid resp=name",
                {"adv_data": flags_ad + uuid_ad, "resp_data": name_ad}),
            ("adv=uuid resp=name", {"adv_data": uuid_ad, "resp_data": name_ad}),
            # Diagnostic-only: visible to permissive scanners, not to
            # OS Bluetooth settings. Reaching here = stack is wedged.
            ("empty", {}),
        ]
        last_err = None
        for label, kwargs in candidates:
            try:
                self._ble.gap_advertise(None)
            except OSError:
                pass
            try:
                self._ble.gap_advertise(250_000, **kwargs)
                print("blekb: advertising as", self.name, "shape:", label)
                return
            except OSError as e:
                print("blekb: adv shape", label, "err:", e)
                last_err = e
        raise last_err if last_err is not None else OSError("advertise failed")

    def maybe_request_pair(self):
        """Security request from the main loop; timing depends on
        whether we hold bond keys.

        Fresh pairing (empty store): wait 3 s. Firing earlier races
        the host's own SMP start (collision → enc=0 on every try
        once macOS has our GATT cached). Bonded reconnect (store
        non-empty): fire fast. macOS reconnects, never re-encrypts
        on its own, and drops the link after ~1-2 s of silence — a
        real HID keyboard sends a Security Request right after
        reconnection to trigger the LTK restore, so we do too.
        """
        if (self._conn is None or self._encrypted or self._pair_requested
                or self._shutting_down or SEC_MODE < 2):
            return
        wait_ms = 800 if self._secrets else 3000
        if time.ticks_diff(time.ticks_ms(), self._conn_ms) < wait_ms:
            return
        self._pair_requested = True
        try:
            self._ble.gap_pair(self._conn)
            print("blekb: gap_pair sent (fallback security request)")
        except (OSError, AttributeError, TypeError) as e:
            print("blekb: gap_pair unavailable/failed:", e)

    def _rearm_adv(self, _):
        # Staircase retry from scheduler context (buddy_ble pattern):
        # immediate re-advertise after disconnect gets OSError(-30).
        for attempt in range(5):
            try:
                self._ble.gap_advertise(None)
            except OSError:
                pass
            time.sleep_ms(150 * (attempt + 1))
            try:
                self._advertise()
                self.state = "advertising"
                self.dirty = True
                return
            except OSError as e:
                print("blekb: re-advertise attempt", attempt + 1, "err:", e)
        print("blekb: giving up on re-advertise; power-cycle to recover")

    # ---- IRQ: trace everything, react to little

    def _irq(self, event, data):
        if self._shutting_down:
            return
        name = _EVENT_NAMES.get(event, "?")
        if event == _IRQ_CENTRAL_CONNECT:
            conn, addr_type, addr = data
            print("blekb: IRQ", event, name, "conn=", conn,
                  "addr_type=", addr_type, "addr=", bytes(addr).hex())
            self._conn = conn
            self._conn_ms = time.ticks_ms()
            self._pair_requested = False
            self._encrypted = False
            self.state = "connected"
            self.dirty = True
            # NOTE: no immediate gap_pair here. The encrypted input-
            # report flag is the primary pairing trigger (host bounces
            # off Insufficient Encryption and starts SMP itself —
            # standard HOGP flow). An eager security request from our
            # side raced the host's own SMP start once macOS had the
            # GATT table cached (discovery near-instant on revisits)
            # and the collision failed every pairing with enc=0.
            # maybe_request_pair() in the main loop sends gap_pair
            # only as a 3-second fallback for hosts that never start.

        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn, _at, _addr = data
            print("blekb: IRQ", event, name, "conn=", conn)
            self._conn = None
            self._encrypted = False
            self._pair_requested = False
            self.passkey_prompt = None
            self.state = "disconnected"
            self.dirty = True
            try:
                micropython.schedule(self._rearm_adv, 0)
            except RuntimeError:
                try:
                    self._advertise()
                except OSError as e:
                    print("blekb: inline re-advertise failed:", e)

        elif event == _IRQ_ENCRYPTION_UPDATE:
            # The Phase-1 success signal. If this fires with enc=1 the
            # C-level SMP completed despite the broken Python API.
            conn, enc, auth, bonded, key_size = data
            print("blekb: IRQ", event, name, "enc=", enc, "auth=", auth,
                  "bonded=", bonded, "key_size=", key_size)
            self._encrypted = bool(enc)
            if enc:
                self.state = "encrypted"
                self.dirty = True

        elif event == _IRQ_GET_SECRET:
            sec_type, index, key = data
            print("blekb: GET_SECRET type=", sec_type, "idx=", index,
                  "key=", None if key is None else bytes(key).hex())
            if not _BOND_STORE:
                return None
            if key is None:
                # Index-based iteration over all secrets of this type.
                i = 0
                for (t, _k), value in self._secrets.items():
                    if t == sec_type:
                        if i == index:
                            return value
                        i += 1
                return None
            return self._secrets.get((sec_type, bytes(key)), None)

        elif event == _IRQ_SET_SECRET:
            sec_type, key, value = data
            print("blekb: SET_SECRET type=", sec_type,
                  "del" if value is None else "store")
            if not _BOND_STORE:
                return False
            k = (sec_type, bytes(key))
            if value is None:
                if k in self._secrets:
                    del self._secrets[k]
                    self._secrets_dirty = True
                    return True
                return False
            self._secrets[k] = bytes(value)
            self._secrets_dirty = True
            return True

        elif event == _IRQ_PASSKEY_ACTION:
            conn, action, passkey = data
            print("blekb: IRQ", event, name, "action=", action,
                  "passkey=", passkey)
            if action == _PASSKEY_ACTION_NUMCMP:
                # Numeric comparison with no display on our side:
                # accept. (MITM is off in every SEC_MODE we run.)
                try:
                    self._ble.gap_passkey(conn, action, 1)
                except OSError as e:
                    print("blekb: gap_passkey accept failed:", e)
            elif action == _PASSKEY_ACTION_INPUT:
                # Classic keyboard pairing: host displays a code, user
                # types it here. Flag the main loop to collect digits.
                self._passkey_conn = conn
                self._passkey_action = action
                self.passkey_prompt = ""
                self.dirty = True
            elif action == _PASSKEY_ACTION_DISP:
                # DisplayOnly flow — we have a screen, so show one.
                pk = (time.ticks_us() ^ time.ticks_ms()) % 1000000
                print("blekb: displaying passkey", pk)
                self.passkey_prompt = "SHOW:{:06d}".format(pk)
                self.dirty = True
                try:
                    self._ble.gap_passkey(conn, action, pk)
                except OSError as e:
                    print("blekb: gap_passkey display failed:", e)

        elif event == _IRQ_GATTS_WRITE:
            conn, handle = data
            try:
                val = bytes(self._ble.gatts_read(handle))
            except OSError:
                val = b"?"
            which = ("OUTPUT(LED)" if handle == self._h_out
                     else "PROTO_MODE" if handle == self._h_proto
                     else "CTRL_POINT" if handle == self._h_ctrl
                     else "h{}".format(handle))
            # A write to the LED output report is proof the host's HID
            # layer is alive and parsed our report map.
            print("blekb: IRQ", event, name, which, "val=", val.hex())

        elif event == _IRQ_MTU_EXCHANGED:
            conn, mtu = data
            print("blekb: IRQ", event, name, "mtu=", mtu)

        elif event == _IRQ_CONNECTION_UPDATE:
            print("blekb: IRQ", event, name, tuple(data))

        else:
            print("blekb: IRQ", event, name)

    # ---- passkey entry (only if _IRQ_PASSKEY_ACTION INPUT fired)

    def feed_passkey_digit(self, ch):
        if self.passkey_prompt is None or self.passkey_prompt.startswith("SHOW:"):
            return
        if ch.isdigit() and len(self.passkey_prompt) < 6:
            self.passkey_prompt += ch
            self.dirty = True

    def submit_passkey(self):
        if (self.passkey_prompt is None or self.passkey_prompt.startswith("SHOW:")
                or self._passkey_conn is None):
            return
        try:
            pk = int(self.passkey_prompt)
        except ValueError:
            pk = 0
        print("blekb: submitting passkey", pk)
        try:
            self._ble.gap_passkey(self._passkey_conn, self._passkey_action, pk)
        except OSError as e:
            print("blekb: gap_passkey submit failed:", e)
        self.passkey_prompt = None
        self.dirty = True

    # ---- HID send

    @property
    def connected(self):
        return self._conn is not None

    def send_key(self, usage, mods, label):
        if self._conn is None:
            return False
        try:
            self._ble.gatts_notify(self._conn, self._h_in,
                                   bytes([mods, 0, usage, 0, 0, 0, 0, 0]))
            self._ble.gatts_notify(self._conn, self._h_in, b"\x00" * 8)
        except OSError as e:
            # Drop the keystroke, never crash the loop — the host may
            # not have subscribed yet, or the link may be mid-teardown.
            print("blekb: notify failed:", e)
            return False
        self.last_sent = label
        self.dirty = True
        return True

    def deinit(self):
        # Three-layer teardown copied from buddy_ble.deinit: flag,
        # detach IRQ, then stop adv / drop the link. gap_disconnect is
        # async — without the flag the late DISCONNECT event would
        # re-arm advertising on an app that's exiting.
        self._shutting_down = True
        try:
            self._ble.irq(None)
        except (OSError, TypeError):
            pass
        if self._secrets_dirty:
            try:
                self.save_secrets()
            except Exception as e:
                print("blekb: exit bond save failed:", e)
        if self._conn is not None:
            try:
                self._ble.gatts_notify(self._conn, self._h_in, b"\x00" * 8)
            except OSError:
                pass
        try:
            self._ble.gap_advertise(None)
        except OSError:
            pass
        if self._conn is not None:
            try:
                self._ble.gap_disconnect(self._conn)
            except OSError:
                pass


# ---- LCD chrome (suite standard: 20 px DARK header, ORANGE hairline)

def _set_font():
    try:
        _LCD.setFont(_LCD.FONTS.DejaVu9)
    except Exception as e:
        print("blekb: setFont fallback:", e)


def _draw_chrome():
    _LCD.fillScreen(_BLACK)
    _LCD.fillRect(0, 0, _W, 20, _DARK)
    _LCD.fillRect(0, 20, _W, 1, _ORANGE)
    _LCD.setTextSize(1)
    _LCD.setTextColor(_ORANGE, _DARK)
    _LCD.drawString("BLE Keyboard", 6, 5)
    _LCD.setTextColor(_GRAY_MID, _BLACK)
    hint = "Fn+Q = exit"
    _LCD.drawString(hint, (_W - _LCD.textWidth(hint)) // 2, _H - 14)


def _draw_state(kbd):
    _LCD.fillRect(0, 21, _W, _H - 21 - 16, _BLACK)
    if kbd.passkey_prompt is not None:
        if kbd.passkey_prompt.startswith("SHOW:"):
            line1 = "Pairing code:"
            line2 = kbd.passkey_prompt[5:]
        else:
            line1 = "Type pairing code + Enter:"
            line2 = kbd.passkey_prompt + "_"
        _LCD.setTextColor(_CREAM, _BLACK)
        _LCD.drawString(line1, (_W - _LCD.textWidth(line1)) // 2, 40)
        _LCD.setTextSize(2)
        _LCD.drawString(line2, (_W - _LCD.textWidth(line2)) // 2, 62)
        _LCD.setTextSize(1)
        return
    state = kbd.state
    if state == "encrypted":
        color, text = _GREEN, "ENCRYPTED LINK"
    elif state == "connected":
        color, text = _GREEN, "CONNECTED"
    elif state == "advertising":
        color, text = _CREAM, "ADVERTISING"
    elif state == "disconnected":
        color, text = _ORANGE, "DISCONNECTED"
    else:
        color, text = _GRAY_MID, state.upper()
    _LCD.setTextColor(color, _BLACK)
    _LCD.drawString(text, (_W - _LCD.textWidth(text)) // 2, 40)
    _LCD.setTextColor(_GRAY_MID, _BLACK)
    _LCD.drawString(kbd.name, (_W - _LCD.textWidth(kbd.name)) // 2, 58)
    if kbd.last_sent:
        _LCD.setTextColor(_CREAM, _BLACK)
        sent = "sent: {}".format(kbd.last_sent)
        _LCD.drawString(sent, (_W - _LCD.textWidth(sent)) // 2, 84)


def _key_to_report(k):
    """Map a MatrixKeyboard key to (usage, mods, label) or None."""
    if k is None:
        return None
    if isinstance(k, int):
        hit = _INT_KEYMAP.get(k)
        if hit:
            label = {0x0A: "Enter", 0x0D: "Enter", 0x08: "BS", 0x09: "Tab",
                     0x1B: "ESC", 0x7F: "Del", 180: "Left", 181: "Up",
                     182: "Down", 183: "Right"}.get(k, "?")
            return (hit[0], hit[1], label)
        if 0x20 <= k <= 0x7E:
            k = chr(k)
        else:
            # The discovery channel: unmapped codes logged here tell
            # us what Fn-combos actually emit on this firmware.
            print("blekb: unmapped key int:", k)
            return None
    if not isinstance(k, str) or len(k) != 1:
        print("blekb: unmapped key:", repr(k))
        return None
    hit = _KEYMAP.get(k)
    if hit is None:
        print("blekb: unmapped char:", repr(k))
        return None
    return (hit[0], hit[1], k if k != " " else "Space")


def run():
    _set_font()
    _draw_chrome()
    kb = MatrixKeyboard()
    kbd = HIDKeyboard()
    # Debounce the keypress that launched us from the menu.
    time.sleep_ms(400)
    try:
        while True:
            kb.tick()
            k = kb.get_key()

            if k is not None and kbd.passkey_prompt is not None \
                    and not kbd.passkey_prompt.startswith("SHOW:"):
                # Passkey-entry mode: digits feed the prompt, Enter
                # submits, everything else is ignored until done.
                if isinstance(k, int) and k in (0x0A, 0x0D):
                    kbd.submit_passkey()
                elif isinstance(k, int) and 0x30 <= k <= 0x39:
                    kbd.feed_passkey_digit(chr(k))
                k = None

            if k is not None:
                if isinstance(k, int) and k == _EXIT_KEY:
                    return
                rep = _key_to_report(k)
                if rep is not None:
                    kbd.send_key(rep[0], rep[1], rep[2])

            if kbd.dirty:
                kbd.dirty = False
                _draw_state(kbd)

            if kbd._secrets_dirty:
                kbd.save_secrets()

            kbd.maybe_request_pair()

            time.sleep_ms(25)
    finally:
        try:
            kbd.deinit()
        except Exception as e:
            print("blekb: deinit warning:", e)
        try:
            _LCD.fillScreen(_BLACK)
        except Exception as e:
            print("blekb: clear warning:", e)
        time.sleep_ms(200)
        machine.reset()


run()
