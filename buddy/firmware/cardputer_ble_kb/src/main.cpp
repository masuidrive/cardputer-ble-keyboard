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
// The display stays on continuously, showing a status screen;
// holding Fn overrides it with a guide of the useful chords (host
// switch / IME / unpair / brightness). Brightness is Fn+[ / Fn+]
// adjustable and saved to NVS.
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

// Backlight brightness when awake — adjustable with Fn+[ / Fn+] and
// persisted to NVS. Min stays above 0 so the screen never adjusts
// itself fully dark.
static const uint8_t BRIGHT_DEFAULT = 26;     // ~10% of 255
static uint8_t dispBright = BRIGHT_DEFAULT;
static const uint8_t BRIGHT_MIN = 16;
static const uint8_t BRIGHT_MAX = 255;
static const uint8_t BRIGHT_STEP = 32;

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
    if (usage == 0x27) return true;                              // 0  unpair
    if (usage == 0x2F || usage == 0x30) return true;            // [ ] brightness
    if (usage == 0x0A) return true;                             // g  hidden game
    return false;
}

// Step the backlight brightness and persist it. Applied live so the
// change is visible while the Fn guide is on screen.
static void adjustBrightness(int delta) {
    int v = (int)dispBright + delta;
    if (v < BRIGHT_MIN) v = BRIGHT_MIN;
    if (v > BRIGHT_MAX) v = BRIGHT_MAX;
    dispBright = (uint8_t)v;
    M5Cardputer.Display.setBrightness(dispBright);
    prefs.begin("kbd", false);
    prefs.putUChar("bright", dispBright);
    prefs.end();
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

    // Header bar: title + battery top-right.
    d.fillRect(0, 0, d.width(), 24, COL_DARK);
    d.fillRect(0, 24, d.width(), 1, COL_ORANGE);
    d.setTextSize(2);
    d.setTextColor(COL_ORANGE, COL_DARK);
    d.drawString("BLE Keyboard", 6, 4);

    int32_t batt = M5.Power.getBatteryLevel();
    char bat[8];
    if (batt >= 0) snprintf(bat, sizeof(bat), "%ld%%", (long)batt);
    else           snprintf(bat, sizeof(bat), "--");
    d.setTextColor(batt >= 0 && batt <= 20 ? COL_ORANGE : COL_CREAM, COL_DARK);
    d.drawString(bat, d.width() - d.textWidth(bat) - 6, 4);

    // The headline: which host slot is active.
    char host[12];
    snprintf(host, sizeof(host), "Host %d", currentSlot + 1);
    d.setTextSize(3);
    d.setTextColor(COL_CREAM, COL_BLACK);
    d.drawString(host, (d.width() - d.textWidth(host)) / 2, 34);

    // Connection state, colored.
    const char *msg = connected ? "CONNECTED" : "ADVERTISING";
    d.setTextSize(2);
    d.setTextColor(connected ? COL_GREEN : COL_ORANGE, COL_BLACK);
    d.drawString(msg, (d.width() - d.textWidth(msg)) / 2, 68);

    // Device name + hint, small.
    char name[20];
    slotName(currentSlot, name, sizeof(name));
    d.setTextSize(1);
    d.setTextColor(COL_CREAM, COL_BLACK);
    d.drawString(name, (d.width() - d.textWidth(name)) / 2, 94);
    d.drawString("Hold Fn for shortcuts",
                 (d.width() - d.textWidth("Hold Fn for shortcuts")) / 2, 110);
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
    d.drawString("1/2/3/0=Host/unpair", 6, 40);
    d.drawString("Spc/Ent = EN/JP",     6, 68);

    char bl[20];
    snprintf(bl, sizeof(bl), "[ ]=bright %d%%", (int)dispBright * 100 / 255);
    d.drawString(bl, 6, 96);
}

// Persist the target slot and reboot into it.
static void switchToSlot(uint8_t slot) {
    if (slot >= NUM_SLOTS || slot == currentSlot) return;

    auto &d = M5Cardputer.Display;
    d.setBrightness(dispBright);
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
    d.setBrightness(dispBright);
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

// ---- hidden mini-game: a Chrome-dino-style runner (Fn+G) ----
// Self-contained: runs its own loop with M5Canvas double-buffering,
// reads the keyboard directly, returns to the keyboard UI on Esc. BLE
// stays connected; we just don't send HID while playing. Space = jump,
// Esc (the ` key) = quit. Rendered as the white-on-black "night mode"
// Chrome dino. The character/obstacle art is defined as pixel grids
// below so it's easy to read and tweak.

// Original homage art (not Google's asset): a chunky right-facing
// runner drawn body + animated legs so it shuffles like the Chrome
// dino while keeping its own silhouette.
static const char *DINO_BODY[] = {
    "           ######   ",
    "           #######  ",
    "           ## ####  ",   // eye = the gap
    "           #######  ",
    "           #######  ",
    "           ####### #",
    "  #        #######  ",
    "  ##       #######  ",
    "  ###      #######  ",
    "   #####  ######### ",
    "   ################ ",
    "   ################ ",
    "  ################  ",
    "   ##############   ",
    "   #############    ",
    "   ###########      ",
    "   ##########       ",
};
static const char *DINO_LEGS_STAND[] = {
    "   ###  ###         ",
    "   ##    ##         ",
    "   ##    ##         ",
    "   #     #          ",
    "   #     #          ",
};
static const char *DINO_LEGS_RUN1[] = {
    "   ###  ###         ",
    "   ##    ##         ",
    "    #    ##         ",
    "         #          ",
    "         #          ",
};
static const char *DINO_LEGS_RUN2[] = {
    "   ###  ###         ",
    "   ##    ##         ",
    "   ##    #          ",
    "   #                ",
    "   #                ",
};
static const int DINO_BODY_H = 17, DINO_H = 22;

static const char *CACTUS_ART[] = {
    "  ##  ",
    "  ##  ",
    "# ## #",
    "# ## #",
    "######",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
};
static const int CACTUS_W = 6, CACTUS_H = 16;

static void drawArt(M5Canvas &cv, int x, int y, const char *const *art,
                    int rows, uint16_t color) {
    for (int r = 0; r < rows; r++) {
        const char *line = art[r];
        for (int c = 0; line[c]; c++)
            if (line[c] == '#') cv.drawPixel(x + c, y + r, color);
    }
}

static void playDino() {
    auto &disp = M5Cardputer.Display;
    uint8_t savedBright = dispBright;
    disp.setBrightness(160);  // the 10% default would be too dark to play

    M5Canvas cv(&disp);
    if (cv.createSprite(240, 135) == nullptr) {
        disp.fillScreen(COL_BLACK);
        disp.setTextColor(COL_ORANGE, COL_BLACK);
        disp.setTextSize(1);
        disp.drawString("game: low memory", 20, 60);
        delay(1200);
        disp.setBrightness(savedBright);
        return;
    }

    const int groundY = 118;
    const int dinoX = 24;
    // Physics matched to the Chrome runner's feel at 60 fps: gravity
    // 0.6/frame, a ~0.5 s jump arc, sub-pixel horizontal motion, and a
    // shorter hop when Space is released early (variable-height jump).
    const float GRAVITY = 0.6f;
    const float JUMP_V = -8.8f;
    const float SPEED0 = 2.6f, SPEED_MAX = 6.0f, SPEED_ACC = 0.0010f;

    float dinoY = groundY - DINO_H, vy = 0;
    bool onGround = true, gameOver = false, prevJump = false;

    struct Obs { float x; int count; bool active; };  // count = adjacent cacti
    Obs obs[3] = {};
    int frames = 0, score = 0, best = 0;
    float speed = SPEED0;
    uint32_t lastSpawn = 0;
    int spawnGap = 1100;
    randomSeed(micros());

    while (true) {
        M5Cardputer.update();
        auto ks = M5Cardputer.Keyboard.keysState();
        bool jump = ks.space;
        bool quit = false;
        for (auto u : ks.hid_keys) if (u == 0x35) quit = true;  // ` / Esc
        if (quit) break;

        if (!gameOver) {
            if (jump && !prevJump && onGround) { vy = JUMP_V; onGround = false; }
            // Variable jump: rising with Space released -> fall faster.
            vy += (!jump && vy < 0) ? GRAVITY * 2.2f : GRAVITY;
            dinoY += vy;
            if (dinoY >= groundY - DINO_H) { dinoY = groundY - DINO_H; vy = 0; onGround = true; }

            uint32_t nowm = millis();
            if (nowm - lastSpawn > (uint32_t)spawnGap) {
                for (auto &o : obs) if (!o.active) {
                    o.active = true; o.x = 240; o.count = 1 + random(0, 3);
                    break;
                }
                lastSpawn = nowm;
                spawnGap = 600 + random(0, 700);
            }

            for (auto &o : obs) if (o.active) {
                o.x -= speed;
                int ow = o.count * CACTUS_W;
                if (o.x + ow < 0) o.active = false;
                bool xo = o.x < dinoX + 17 && o.x + ow > dinoX + 3;
                bool yo = (dinoY + DINO_H) > (groundY - CACTUS_H);
                if (xo && yo) gameOver = true;
            }

            if (++frames % 3 == 0) score++;
            speed += SPEED_ACC;
            if (speed > SPEED_MAX) speed = SPEED_MAX;
        } else {
            if (score > best) best = score;
            if (jump && !prevJump) {  // retry
                gameOver = false; score = 0; frames = 0; speed = SPEED0;
                dinoY = groundY - DINO_H; vy = 0; onGround = true;
                for (auto &o : obs) o.active = false;
                lastSpawn = millis();
            }
        }
        prevJump = jump;

        cv.fillSprite(COL_BLACK);
        cv.drawFastHLine(0, groundY, 240, COL_CREAM);
        // dino: body + animated legs (legs hold still while airborne)
        drawArt(cv, dinoX, (int)dinoY, DINO_BODY, DINO_BODY_H, COL_CREAM);
        const char *const *legs = !onGround ? DINO_LEGS_STAND
                                  : ((frames / 6) % 2 ? DINO_LEGS_RUN1 : DINO_LEGS_RUN2);
        drawArt(cv, dinoX, (int)dinoY + DINO_BODY_H, legs, 5, COL_CREAM);
        for (auto &o : obs) if (o.active)
            for (int i = 0; i < o.count; i++)
                drawArt(cv, (int)o.x + i * CACTUS_W, groundY - CACTUS_H,
                        CACTUS_ART, CACTUS_H, COL_CREAM);

        cv.setTextSize(1);
        cv.setTextColor(COL_GRAY);
        cv.drawString("Fn-game  Esc=quit", 4, 4);
        cv.setTextColor(COL_CREAM);
        char s[24];
        snprintf(s, sizeof(s), "%05d", score);
        cv.drawString(s, 240 - cv.textWidth(s) - 4, 4);
        if (gameOver) {
            cv.setTextSize(2);
            cv.setTextColor(COL_ORANGE);
            cv.drawString("GAME OVER", (240 - cv.textWidth("GAME OVER")) / 2, 42);
            cv.setTextSize(1);
            cv.setTextColor(COL_CREAM);
            snprintf(s, sizeof(s), "score %d   best %d", score, best);
            cv.drawString(s, (240 - cv.textWidth(s)) / 2, 66);
            cv.setTextColor(COL_GRAY);
            const char *r = "Space=retry  Esc=quit";
            cv.drawString(r, (240 - cv.textWidth(r)) / 2, 80);
        }
        cv.pushSprite(0, 0);
        delay(16);  // ~60 fps
    }

    cv.deleteSprite();
    disp.setBrightness(savedBright);
}

// ---- runtime state ----

enum Disp { D_STATUS, D_GUIDE };
static Disp dispMode = D_STATUS;
static bool forceRedraw = false;
static bool lastConnected = false;
static uint32_t lastBatteryMs = 0;
static uint32_t connectedAtMs = 0;
static bool peerCaptured = false;

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // true = init keyboard
    M5Cardputer.Display.setRotation(1);

    prefs.begin("kbd", false);
    currentSlot = prefs.getUChar("slot", 0);
    dispBright = prefs.getUChar("bright", BRIGHT_DEFAULT);
    // One-time adoption of the 10% default on devices that still
    // hold the previous (brighter) saved value. After this, Fn+[/]
    // adjustments persist normally.
    if (prefs.getUChar("bver", 0) < 1) {
        dispBright = BRIGHT_DEFAULT;
        prefs.putUChar("bright", dispBright);
        prefs.putUChar("bver", 1);
    }
    prefs.end();
    if (currentSlot >= NUM_SLOTS) currentSlot = 0;
    if (dispBright < BRIGHT_MIN) dispBright = BRIGHT_MIN;

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

    M5Cardputer.Display.setBrightness(dispBright);
    drawStatus(false);
    dispMode = D_STATUS;
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
        forceRedraw = true;  // repaint the status for the new state
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

    // Always-on display: status is shown continuously; holding Fn
    // overrides it with the shortcut guide.
    Disp want = fnHeld ? D_GUIDE : D_STATUS;
    if (want != dispMode || forceRedraw) {
        dispMode = want;
        forceRedraw = false;
        M5Cardputer.Display.setBrightness(dispBright);
        if (want == D_GUIDE) drawGuide();
        else drawStatus(connected);
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
                    else if (u == 0x2F) { adjustBrightness(-BRIGHT_STEP); forceRedraw = true; }
                    else if (u == 0x30) { adjustBrightness(+BRIGHT_STEP); forceRedraw = true; }
                    else if (u == 0x0A) { playDino(); forceRedraw = true; }  // Fn+G
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
