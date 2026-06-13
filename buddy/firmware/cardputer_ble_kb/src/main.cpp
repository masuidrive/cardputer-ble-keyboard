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

// BleKeyboard.h must come FIRST: M5Cardputer's Keyboard_def.h
// #defines KEY_BACKSPACE/KEY_TAB/... as bare numbers, which would
// macro-expand inside BleKeyboard.h's `const uint8_t KEY_*`
// declarations and break the build. We never use those KEY_*
// constants ourselves (raw HID usages only), so the reverse order
// is safe.
#include <BleKeyboard.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>

BleKeyboard bleKeyboard("Cardputer KB", "M5Stack", 100);

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

static void drawStatus(bool connected) {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BLACK);
    d.fillRect(0, 0, d.width(), 20, COL_DARK);
    d.fillRect(0, 20, d.width(), 1, COL_ORANGE);
    d.setTextColor(COL_ORANGE, COL_DARK);
    d.setTextSize(1);
    d.drawString("BLE Keyboard (Arduino)", 6, 6);

    const char *msg = connected ? "CONNECTED" : "ADVERTISING as Cardputer KB";
    d.setTextColor(connected ? COL_GREEN : COL_CREAM, COL_BLACK);
    d.drawString(msg, (d.width() - d.textWidth(msg)) / 2, 55);

    d.setTextColor(COL_GRAY, COL_BLACK);
    const char *hint = connected ? "Fn+;,./ = arrows  alt = Cmd"
                                 : "Pair from Bluetooth settings";
    d.drawString(hint, (d.width() - d.textWidth(hint)) / 2, 110);
}

static bool lastConnected = false;

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // true = init keyboard
    M5Cardputer.Display.setRotation(1);
    drawStatus(false);

    bleKeyboard.begin();

    // Override the security the T-vK library set inside begin(): it
    // hardcodes MITM-required, which on a NoInputNoOutput keyboard is
    // a contradiction. Force Just-Works: bonding + Secure Connections,
    // NO MITM, IO capability NONE. NimBLE reads these globals at
    // pairing time, so setting them after begin() takes effect on the
    // next connect. This is the documented fix for macOS HID pairing.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    // Print the actual BLE address so we can confirm what macOS sees
    // (and whether any stale bond would match it).
    Serial.print("BLE address: ");
    Serial.println(NimBLEDevice::getAddress().toString().c_str());
}

static uint32_t lastBatteryMs = 0;

void loop() {
    M5Cardputer.update();

    bool connected = bleKeyboard.isConnected();
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
            bleKeyboard.setBatteryLevel((uint8_t)level);
        }
    }
    if (!connected) {
        lastBatteryMs = 0;  // force a fresh report on the next connect
    }

    if (M5Cardputer.Keyboard.isChange()) {
        KeyReport report = {};
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
            if (st.ctrl)  report.modifiers |= 0x01;  // Left Ctrl
            if (st.shift) report.modifiers |= 0x02;  // Left Shift
            if (st.opt)   report.modifiers |= 0x04;  // Left Alt (Option)
            if (st.alt)   report.modifiers |= 0x08;  // Left GUI (Command)

            int n = 0;
            for (auto usage : st.hid_keys) {
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
            bleKeyboard.sendReport(&report);
        }
    }

    delay(5);
}
