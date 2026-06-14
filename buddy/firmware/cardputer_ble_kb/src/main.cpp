// BLE HID keyboard for the M5Stack Cardputer-Adv.
//
// The M5Cardputer library's Keyboard_Class delivers true key-down /
// key-up state (the TCA8418 driver tracks the full matrix), so we
// build the 8-byte HID report directly from keysState() on every
// change: held keys stay held, the host does its own key repeat,
// and modifier chords (Cmd+C etc.) work like on a real keyboard.
//
// Modifier mapping (left side of the bottom row, Mac semantics):
//   ctrl  -> Left Ctrl   (0x01)
//   shift -> Left Shift  (0x02)
//   opt   -> Left Alt    (0x04)  = Option on macOS
//   alt   -> Left GUI    (0x08)  = Command on macOS
//
// Fn layer (Fn is not a HID modifier; we remap usages while held):
//   Fn + ; , . /  -> arrow keys (Up Left Down Right)
//   Fn + `        -> Escape
//   Fn + Backspace-> Forward Delete
//   Fn + Space    -> LANG2 (0x91, 英数 — alphanumeric input)
//   Fn + Enter    -> LANG1 (0x90, かな — Japanese input)
//   Fn + 1 / 2 / 3-> switch host slot (multi-device, below)
//   Fn + 0        -> unpair the current host slot
//
// Holding Fn shows an on-screen guide of the useful chords (host
// switch / IME / unpair). The display is otherwise off while typing
// to save power, waking only on a Fn hold or a connect/disconnect.
//
// Multi-host: three independent host "slots." Each slot advertises
// under its own BLE address (factory MAC with the low nibble set to
// the slot index) and its own name, so each host bonds to what it
// sees as a separate keyboard. Fn+N persists the target slot and
// reboots into it; the host bonded there reconnects. Bonds for all
// slots coexist in NimBLE's NVS store (MAX_BONDS raised in
// platformio.ini). Per-slot identity + reboot is the design proven
// to work on NimBLE-Arduino for ESP32-S3; live switching is flaky.

// BleKeyboard.h must come FIRST: M5Cardputer's Keyboard_def.h
// #defines KEY_BACKSPACE/KEY_TAB/... as bare numbers, which would
// macro-expand inside BleKeyboard.h's `const uint8_t KEY_*`
// declarations and break the build. We use raw HID usages only.
#include <BleKeyboard.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_mac.h"

static const uint8_t NUM_SLOTS = 3;
static const uint8_t DISP_ON = 180;           // backlight brightness when awake
static const uint32_t SHOW_CONNECTED_MS = 8000;   // status dwell after connect
static const uint32_t SHOW_ADVERTISING_MS = 30000; // longer while waiting to pair

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
        case 0x33: return 0x52;  // ;     -> Up arrow
        case 0x36: return 0x50;  // ,     -> Left arrow
        case 0x37: return 0x51;  // .     -> Down arrow
        case 0x38: return 0x4F;  // /     -> Right arrow
        case 0x35: return 0x29;  // `     -> Escape
        case 0x2A: return 0x4C;  // BS    -> Forward Delete
        case 0x2C: return 0x91;  // Space -> LANG2 (英数)
        case 0x28: return 0x90;  // Enter -> LANG1 (かな)
        default:   return usage;
    }
}

// True for usages that the Fn layer consumes as a command (host
// switch / unpair) and must NOT be typed to the host.
static bool isFnCommandKey(uint8_t usage) {
    if (usage >= 0x1E && usage < 0x1E + NUM_SLOTS) return true;  // 1..NUM_SLOTS
    if (usage == 0x27) return true;                              // 0
    return false;
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
// universal/multicast bits intact. Must run BEFORE NimBLE init.
static void applySlotAddress(uint8_t slot) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
    mac[5] = (mac[5] & 0xF0) | (slot & 0x0F);
    esp_base_mac_addr_set(mac);
}

static void slotName(uint8_t slot, char *out, size_t n) {
    snprintf(out, n, "Cardputer KB %d", slot + 1);
}

// ---- per-slot peer (for unpair when not currently connected) ----

static void saveSlotPeer(uint8_t slot, const NimBLEAddress &a) {
    char k[8];
    prefs.begin("kbd", false);
    snprintf(k, sizeof(k), "p%d", slot);
    prefs.putString(k, a.toString().c_str());
    snprintf(k, sizeof(k), "pt%d", slot);
    prefs.putUChar(k, a.getType());
    prefs.end();
}

static bool loadSlotPeer(uint8_t slot, NimBLEAddress &out) {
    char k[8];
    prefs.begin("kbd", true);
    snprintf(k, sizeof(k), "p%d", slot);
    String s = prefs.getString(k, "");
    snprintf(k, sizeof(k), "pt%d", slot);
    uint8_t t = prefs.getUChar(k, 0);
    prefs.end();
    if (s.length() < 17) return false;  // "xx:xx:xx:xx:xx:xx"
    out = NimBLEAddress(std::string(s.c_str()), t);
    return true;
}

static void clearSlotPeer(uint8_t slot) {
    char k[8];
    prefs.begin("kbd", false);
    snprintf(k, sizeof(k), "p%d", slot);
    prefs.remove(k);
    snprintf(k, sizeof(k), "pt%d", slot);
    prefs.remove(k);
    prefs.end();
}

// ---- screens ----

static void drawStatus(bool connected) {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BLACK);

    // Header bar.
    d.fillRect(0, 0, d.width(), 24, COL_DARK);
    d.fillRect(0, 24, d.width(), 1, COL_ORANGE);
    d.setTextSize(2);
    d.setTextColor(COL_ORANGE, COL_DARK);
    d.drawString("BLE Keyboard", 6, 4);

    // Big connection state.
    const char *msg = connected ? "CONNECTED" : "ADVERTISING";
    d.setTextSize(2);
    d.setTextColor(connected ? COL_GREEN : COL_CREAM, COL_BLACK);
    d.drawString(msg, (d.width() - d.textWidth(msg)) / 2, 32);

    // Detail lines.
    d.setTextSize(1);
    d.setTextColor(COL_CREAM, COL_BLACK);
    char buf[40], name[20];
    slotName(currentSlot, name, sizeof(name));
    snprintf(buf, sizeof(buf), "Host %d   %s", currentSlot + 1, name);
    d.drawString(buf, 6, 56);

    d.setTextColor(COL_GRAY, COL_BLACK);
    snprintf(buf, sizeof(buf), "Addr %s",
             NimBLEDevice::getAddress().toString().c_str());
    d.drawString(buf, 6, 68);

    int32_t batt = M5.Power.getBatteryLevel();
    int bonds = NimBLEDevice::getNumBonds();
    if (batt >= 0)
        snprintf(buf, sizeof(buf), "Battery %ld%%   Paired %d", (long)batt, bonds);
    else
        snprintf(buf, sizeof(buf), "Paired %d host(s)", bonds);
    d.drawString(buf, 6, 80);

    // Footer hints.
    d.setTextColor(COL_GRAY, COL_BLACK);
    d.drawString("Hold Fn for shortcuts", 6, 100);
    d.drawString("Fn+1/2/3 switch host", 6, 112);
}

static void drawGuide() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BLACK);

    d.fillRect(0, 0, d.width(), 24, COL_DARK);
    d.fillRect(0, 24, d.width(), 1, COL_ORANGE);
    d.setTextSize(2);
    d.setTextColor(COL_ORANGE, COL_DARK);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "Fn  Host %d", currentSlot + 1);
    d.drawString(hdr, 6, 4);

    // Battery, top-right.
    int32_t batt = M5.Power.getBatteryLevel();
    char bat[8];
    if (batt >= 0) snprintf(bat, sizeof(bat), "%ld%%", (long)batt);
    else           snprintf(bat, sizeof(bat), "--");
    d.setTextSize(2);
    d.setTextColor(batt >= 0 && batt <= 20 ? COL_ORANGE : COL_CREAM, COL_DARK);
    d.drawString(bat, d.width() - d.textWidth(bat) - 6, 4);

    d.setTextSize(2);
    d.setTextColor(COL_CREAM, COL_BLACK);
    d.drawString("1/2/3 = host",  6, 34);
    d.drawString("Space = EN",    6, 56);
    d.drawString("Enter = JP",    6, 78);
    d.setTextColor(COL_ORANGE, COL_BLACK);
    d.drawString("0 = unpair",    6, 100);
}

// Persist the target slot and reboot into it.
static void switchToSlot(uint8_t slot) {
    if (slot >= NUM_SLOTS || slot == currentSlot) return;

    auto &d = M5Cardputer.Display;
    d.setBrightness(DISP_ON);
    d.fillScreen(COL_BLACK);
    d.setTextSize(3);
    d.setTextColor(COL_ORANGE, COL_BLACK);
    char line[16];
    snprintf(line, sizeof(line), "Host %d", slot + 1);
    d.drawString(line, (d.width() - d.textWidth(line)) / 2, 46);
    d.setTextSize(2);
    d.setTextColor(COL_GRAY, COL_BLACK);
    const char *sub = "switching...";
    d.drawString(sub, (d.width() - d.textWidth(sub)) / 2, 80);

    prefs.begin("kbd", false);
    prefs.putUChar("slot", slot);
    prefs.end();
    delay(400);
    ESP.restart();
}

// Forget the host bonded to the current slot, then reboot so the slot
// advertises clean and a different device can pair to it.
static void unpairCurrentSlot() {
    auto &d = M5Cardputer.Display;
    d.setBrightness(DISP_ON);
    d.fillScreen(COL_BLACK);
    d.setTextSize(2);
    d.setTextColor(COL_ORANGE, COL_BLACK);
    char line[20];
    snprintf(line, sizeof(line), "Host %d unpaired", currentSlot + 1);
    d.drawString(line, (d.width() - d.textWidth(line)) / 2, 50);

    NimBLEServer *srv = NimBLEDevice::getServer();
    if (srv && srv->getConnectedCount() > 0) {
        NimBLEConnInfo ci = srv->getPeerInfo(0);
        NimBLEDevice::deleteBond(ci.getIdAddress());
    }
    NimBLEAddress saved;
    if (loadSlotPeer(currentSlot, saved)) {
        NimBLEDevice::deleteBond(saved);
    }
    clearSlotPeer(currentSlot);

    delay(900);
    ESP.restart();
}

// ---- runtime state ----

enum Disp { D_OFF, D_STATUS, D_GUIDE };
static Disp dispMode = D_STATUS;
static bool forceRedraw = false;
static uint32_t statusExpireMs = 0;
static bool lastConnected = false;
static uint32_t lastBatteryMs = 0;
static uint32_t connectedAtMs = 0;
static bool peerCaptured = false;

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // true = init keyboard
    M5Cardputer.Display.setRotation(1);

    prefs.begin("kbd", true);
    currentSlot = prefs.getUChar("slot", 0);
    prefs.end();
    if (currentSlot >= NUM_SLOTS) currentSlot = 0;

    applySlotAddress(currentSlot);  // BEFORE BLE init

    char name[20];
    slotName(currentSlot, name, sizeof(name));
    kbd = new BleKeyboard(name, "M5Stack", 100);
    kbd->begin();

    // Override the security the T-vK library set inside begin(): it
    // hardcodes MITM-required, which on a NoInputNoOutput keyboard is
    // a contradiction. Force Just-Works: bonding + Secure Connections,
    // NO MITM, IO capability NONE. This is the documented fix for
    // macOS HID pairing (Bluedroid's cross-transport key derivation
    // on the BLE-only S3 is what wedged it before NimBLE + no-MITM).
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    Serial.print("Slot ");
    Serial.print(currentSlot + 1);
    Serial.print(" BLE address: ");
    Serial.println(NimBLEDevice::getAddress().toString().c_str());

    M5Cardputer.Display.setBrightness(DISP_ON);
    drawStatus(false);
    dispMode = D_STATUS;
    statusExpireMs = millis() + SHOW_ADVERTISING_MS;
}

void loop() {
    M5Cardputer.update();
    uint32_t now = millis();
    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    bool fnHeld = st.fn;

    bool connected = kbd->isConnected();
    if (connected != lastConnected) {
        lastConnected = connected;
        connectedAtMs = now;
        peerCaptured = false;
        statusExpireMs = now + (connected ? SHOW_CONNECTED_MS : SHOW_ADVERTISING_MS);
        forceRedraw = true;
    }

    // Capture the peer's identity address once the (re)connection has
    // settled (pairing/encryption complete), so we can unpair this
    // slot later even when the host isn't currently connected.
    if (connected && !peerCaptured && now - connectedAtMs > 2500) {
        NimBLEServer *srv = NimBLEDevice::getServer();
        if (srv && srv->getConnectedCount() > 0) {
            saveSlotPeer(currentSlot, srv->getPeerInfo(0).getIdAddress());
        }
        peerCaptured = true;
    }

    // Real battery level, refreshed every 3 min (and right after connect).
    if (connected && (lastBatteryMs == 0 || now - lastBatteryMs > 180000)) {
        lastBatteryMs = now;
        int32_t level = M5.Power.getBatteryLevel();
        if (level >= 0) kbd->setBatteryLevel((uint8_t)level);
    }
    if (!connected) lastBatteryMs = 0;

    // Display power state. Off while typing; the Fn guide overrides;
    // status shows for a window after connect/disconnect.
    Disp want = fnHeld ? D_GUIDE : (now < statusExpireMs ? D_STATUS : D_OFF);
    if (want != dispMode || forceRedraw) {
        dispMode = want;
        forceRedraw = false;
        if (want == D_OFF) {
            M5Cardputer.Display.setBrightness(0);
        } else {
            M5Cardputer.Display.setBrightness(DISP_ON);
            if (want == D_GUIDE) drawGuide();
            else drawStatus(connected);
        }
    }

    if (M5Cardputer.Keyboard.isChange()) {
        KeyReport report = {};
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

            // Fn command keys: host switch (1/2/3) and unpair (0).
            // These reboot, so they never fall through to typing.
            if (ks.fn) {
                for (auto u : ks.hid_keys) {
                    if (u >= 0x1E && u < 0x1E + NUM_SLOTS) switchToSlot(u - 0x1E);
                    else if (u == 0x27) unpairCurrentSlot();
                }
            }

            if (ks.ctrl)  report.modifiers |= 0x01;  // Left Ctrl
            if (ks.shift) report.modifiers |= 0x02;  // Left Shift
            if (ks.opt)   report.modifiers |= 0x04;  // Left Alt (Option)
            if (ks.alt)   report.modifiers |= 0x08;  // Left GUI (Command)

            int n = 0;
            for (auto usage : ks.hid_keys) {
                if (ks.fn && isFnCommandKey(usage)) continue;  // swallow 1/2/3/0
                addKey(report, n, ks.fn ? fnRemap(usage) : usage);
            }
            // Belt and braces: the dedicated state flags cover keys
            // some library versions report only as flags. addKey
            // dedupes, so double-reporting is harmless. Fn remaps
            // match fnRemap(): Fn+Enter->LANG1 (かな), Fn+Space->LANG2
            // (英数), Fn+Backspace->Forward Delete.
            if (ks.enter) addKey(report, n, ks.fn ? 0x90 : 0x28);
            if (ks.space) addKey(report, n, ks.fn ? 0x91 : 0x2C);
            if (ks.tab)   addKey(report, n, 0x2B);
            if (ks.del)   addKey(report, n, ks.fn ? 0x4C : 0x2A);
        }
        if (connected) {
            kbd->sendReport(&report);
        }
    }

    delay(5);
}
