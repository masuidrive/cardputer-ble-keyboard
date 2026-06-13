// BLE HID keyboard for the M5Stack Cardputer-Adv.
//
// The M5Cardputer library's Keyboard_Class delivers true key-down /
// key-up state (the TCA8418 driver tracks the full matrix), so we
// build the 8-byte HID report directly from keysState() on every
// change: held keys stay held, the host does its own key repeat,
// and modifier chords (Cmd+C etc.) work like on a real keyboard —
// none of which the MicroPython MatrixKeyboard API could express.
//
// Modifier mapping (left side of the bottom row, Mac semantics):
//   ctrl  -> Left Ctrl   (0x01)
//   shift -> Left Shift  (0x02)
//   opt   -> Left Alt    (0x04)  = Option on macOS
//   alt   -> Left GUI    (0x08)  = Command on macOS
//
// Fn layer (Fn is not a HID modifier; we remap usages while held):
//   Fn + ; , . /  -> real arrow keys (Up Left Down Right)
//   Fn + `        -> Escape
//   Fn + Backspace-> Forward Delete
//   Fn + 1 / 2 / 3-> switch host slot (multi-device, see below)
//
// Multi-host: three independent host "slots." Each slot advertises
// under its own BLE address (derived from the factory MAC with the
// low nibble set to the slot number) and its own name, so each host
// bonds to what it sees as a separate keyboard — no single-address
// multi-bond corruption. Fn+N persists the target slot and reboots;
// the device comes back up as slot N's identity and the host bonded
// there reconnects. Bonds for all slots coexist in NimBLE's NVS
// store (MAX_BONDS is raised in platformio.ini). This per-slot-
// identity + reboot design is the one proven to work on
// NimBLE-Arduino for ESP32-S3; live address switching is flaky.

// BleKeyboard.h must come FIRST: M5Cardputer's Keyboard_def.h
// #defines KEY_BACKSPACE/KEY_TAB/... as bare numbers, which would
// macro-expand inside BleKeyboard.h's `const uint8_t KEY_*`
// declarations and break the build. We never use those KEY_*
// constants ourselves (raw HID usages only), so the reverse order
// is safe.
#include <BleKeyboard.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_mac.h"

static const uint8_t NUM_SLOTS = 3;

// Constructed in setup() once the active slot is known, so the GAP
// name can carry the slot number.
BleKeyboard *kbd = nullptr;
Preferences prefs;
uint8_t currentSlot = 0;

static const uint16_t COL_BLACK = 0x0000;
static const uint16_t COL_ORANGE = 0xCBAB;  // 0xCC785C in RGB565
static const uint16_t COL_CREAM = 0xF777;   // 0xF0EEE6 in RGB565
static const uint16_t COL_DARK = 0x18E3;    // 0x1F1F1F in RGB565
static const uint16_t COL_GREEN = 0x3E6C;
static const uint16_t COL_GRAY = 0x7BEF;

static uint8_t fnRemap(uint8_t usage) {
    switch (usage) {
        case 0x33: return 0x52;  // ;  -> Up arrow
        case 0x36: return 0x50;  // ,  -> Left arrow
        case 0x37: return 0x51;  // .  -> Down arrow
        case 0x38: return 0x4F;  // /  -> Right arrow
        case 0x35: return 0x29;  // `  -> Escape
        case 0x2A: return 0x4C;  // BS -> Forward Delete
        default:   return usage;
    }
}

static void addKey(KeyReport &report, int &n, uint8_t usage) {
    if (usage == 0 || n >= 6) return;
    for (int i = 0; i < n; i++) {
        if (report.keys[i] == usage) return;
    }
    report.keys[n++] = usage;
}

// Give this slot its own BLE identity. Read the factory STA MAC and
// vary the low nibble of the last byte per slot, keeping the OUI and
// universal/multicast bits intact (a wholly synthetic locally-
// administered base can confuse the controller's address-derivation).
// Must run BEFORE NimBLE init (i.e. before kbd->begin()).
static void applySlotAddress(uint8_t slot) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
    mac[5] = (mac[5] & 0xF0) | (slot & 0x0F);
    esp_base_mac_addr_set(mac);
}

static void slotName(uint8_t slot, char *out, size_t n) {
    snprintf(out, n, "Cardputer KB %d", slot + 1);
}

static void drawStatus(bool connected) {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BLACK);

    // Header bar.
    d.fillRect(0, 0, d.width(), 22, COL_DARK);
    d.fillRect(0, 22, d.width(), 1, COL_ORANGE);
    d.setTextSize(1);
    d.setTextColor(COL_ORANGE, COL_DARK);
    d.drawString("BLE Keyboard", 6, 7);

    // Active host slot, big, top-right of the header line area.
    char host[12];
    snprintf(host, sizeof(host), "Host %d", currentSlot + 1);
    d.setTextSize(1);
    d.setTextColor(COL_CREAM, COL_DARK);
    d.drawString(host, d.width() - d.textWidth(host) - 6, 7);

    // Big connection status, centered.
    const char *msg = connected ? "CONNECTED" : "ADVERTISING";
    d.setTextSize(2);
    d.setTextColor(connected ? COL_GREEN : COL_CREAM, COL_BLACK);
    d.drawString(msg, (d.width() - d.textWidth(msg)) / 2, 44);

    // Slot/device name underneath, small.
    char name[20];
    slotName(currentSlot, name, sizeof(name));
    d.setTextSize(1);
    d.setTextColor(COL_GRAY, COL_BLACK);
    d.drawString(name, (d.width() - d.textWidth(name)) / 2, 74);

    // Hint strip.
    d.setTextColor(COL_GRAY, COL_BLACK);
    const char *hint = "Fn+1/2/3 switch host";
    d.drawString(hint, (d.width() - d.textWidth(hint)) / 2, d.height() - 14);
}

// Persist the target slot and reboot into it. Reboot (vs live address
// change) is the reliable path on NimBLE-Arduino; the new identity is
// applied cleanly at the next boot's applySlotAddress().
static void switchToSlot(uint8_t slot) {
    if (slot >= NUM_SLOTS || slot == currentSlot) return;

    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BLACK);
    d.setTextSize(2);
    d.setTextColor(COL_ORANGE, COL_BLACK);
    char line[16];
    snprintf(line, sizeof(line), "Host %d", slot + 1);
    d.drawString(line, (d.width() - d.textWidth(line)) / 2, 44);
    d.setTextSize(1);
    d.setTextColor(COL_GRAY, COL_BLACK);
    const char *sub = "switching...";
    d.drawString(sub, (d.width() - d.textWidth(sub)) / 2, 74);

    prefs.begin("kbd", false);
    prefs.putUChar("slot", slot);
    prefs.end();
    delay(400);
    ESP.restart();
}

static bool lastConnected = false;
static uint32_t lastBatteryMs = 0;

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // true = init keyboard
    M5Cardputer.Display.setRotation(1);

    // Which host slot are we? Default 0 on a fresh device.
    prefs.begin("kbd", true);
    currentSlot = prefs.getUChar("slot", 0);
    prefs.end();
    if (currentSlot >= NUM_SLOTS) currentSlot = 0;

    drawStatus(false);

    // Per-slot BLE identity, BEFORE any BLE init.
    applySlotAddress(currentSlot);

    char name[20];
    slotName(currentSlot, name, sizeof(name));
    kbd = new BleKeyboard(name, "M5Stack", 100);
    kbd->begin();

    // Override the security the T-vK library set inside begin(): it
    // hardcodes MITM-required, which on a NoInputNoOutput keyboard is
    // a contradiction. Force Just-Works: bonding + Secure Connections,
    // NO MITM, IO capability NONE. NimBLE reads these globals at
    // pairing time, so setting them after begin() takes effect on the
    // next connect. This is the documented fix for macOS HID pairing.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    Serial.print("Slot ");
    Serial.print(currentSlot + 1);
    Serial.print(" BLE address: ");
    Serial.println(NimBLEDevice::getAddress().toString().c_str());
}

void loop() {
    M5Cardputer.update();

    bool connected = kbd->isConnected();
    if (connected != lastConnected) {
        lastConnected = connected;
        drawStatus(connected);
    }

    // Report the real battery level every 3 min (and once right after
    // connecting). M5.Power.getBatteryLevel() returns 0-100, or -1 if
    // the gauge can't read it — skip the update in that case rather
    // than reporting a bogus 0%.
    uint32_t now = millis();
    if (connected && (lastBatteryMs == 0 || now - lastBatteryMs > 180000)) {
        lastBatteryMs = now;
        int32_t level = M5.Power.getBatteryLevel();
        if (level >= 0) {
            kbd->setBatteryLevel((uint8_t)level);
        }
    }
    if (!connected) {
        lastBatteryMs = 0;  // force a fresh report on the next connect
    }

    if (M5Cardputer.Keyboard.isChange()) {
        KeyReport report = {};
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();

            // Fn + 1/2/3 switches host slot. Detect it first and never
            // forward the digit to the host. switchToSlot() reboots
            // when the slot actually changes; on the current slot it's
            // a no-op (and we still swallow the digit).
            if (st.fn) {
                for (auto u : st.hid_keys) {
                    if (u >= 0x1E && u < 0x1E + NUM_SLOTS) {
                        switchToSlot(u - 0x1E);  // reboots if different
                    }
                }
            }

            if (st.ctrl)  report.modifiers |= 0x01;  // Left Ctrl
            if (st.shift) report.modifiers |= 0x02;  // Left Shift
            if (st.opt)   report.modifiers |= 0x04;  // Left Alt (Option)
            if (st.alt)   report.modifiers |= 0x08;  // Left GUI (Command)

            int n = 0;
            for (auto usage : st.hid_keys) {
                // Swallow the slot-switch digits so Fn+1/2/3 never type.
                if (st.fn && usage >= 0x1E && usage < 0x1E + NUM_SLOTS) {
                    continue;
                }
                addKey(report, n, st.fn ? fnRemap(usage) : usage);
            }
            // Belt and braces: the dedicated state flags cover keys
            // that some library versions report only as flags, not
            // in hid_keys. addKey dedupes, so double-reporting is
            // harmless.
            if (st.enter) addKey(report, n, 0x28);
            if (st.space) addKey(report, n, 0x2C);
            if (st.tab)   addKey(report, n, 0x2B);
            if (st.del)   addKey(report, n, st.fn ? 0x4C : 0x2A);
        }
        if (connected) {
            kbd->sendReport(&report);
        }
    }

    delay(5);
}
