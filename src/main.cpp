/**
 * Zigbee WS2812B Light Strip
 *
 * Hardware : ESP32-H2 + WS2812B addressable LED strip
 * Zigbee   : End Device, Color Dimmable Light (ZHA device 0x0102)
 * Protocol : XY color + Color Temperature — fully discoverable by the
 *            Philips Hue bridge via standard Zigbee 3.0 network steering.
 *
 * Pairing with Hue bridge
 * ───────────────────────
 * 1. Power on (or factory-reset) this device.
 * 2. In the Hue app → Settings → Lights → Add Light → Search.
 * 3. The bridge opens its network; the device joins automatically.
 *
 * Factory reset
 * ─────────────
 * Hold the BOOT button (GPIO9) for ≥ 3 seconds.
 */

#include <Arduino.h>
#include <math.h>

#ifndef ZIGBEE_MODE_ED
#error "Set board_build.zigbee_mode = ed in platformio.ini"
#endif

#include "Zigbee.h"
#include <FastLED.h>
#include <Preferences.h>
#include "config.h"

// ── Globals ───────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];
ZigbeeColorDimmableLight zbLight(ZIGBEE_ENDPOINT);
static Preferences prefs;

// ── LED state (written from Zigbee callbacks, read from loop) ─────────────
struct LightState {
    bool     on      = false;
    uint8_t  r       = 255, g = 255, b = 255;
    uint8_t  level   = DEFAULT_BRIGHTNESS;
    uint16_t mireds  = 370;  // ~2700 K warm white
    bool     isCT    = false;  // true = colour-temp mode, false = RGB mode
    volatile bool dirty = false;
} lightState;

// ── Effect state ──────────────────────────────────────────────────────────
enum class Effect : uint8_t { NONE, BLINK, BREATHE, RAINBOW, CHANNEL_CHANGE, COLOR_LOOP };
struct {
    volatile Effect active    = Effect::NONE;
    uint32_t        startedAt = 0;
    uint8_t         loopDir   = 1;   // 0 = decrement hue, 1 = increment (ZCL Color Loop Direction)
    uint16_t        loopTime  = 25;  // full hue cycle in seconds (ZCL Color Loop Time default = 25)
} effectState;

// Function pointers called by ZigbeeHandlers.cpp for scene and identify commands
void (*zigbee_identify_effect_cb)(uint8_t, uint8_t) = nullptr;
void (*zigbee_scene_store_cb)(uint16_t, uint8_t)    = nullptr;
void (*zigbee_scene_recall_cb)(uint16_t, uint8_t)   = nullptr;

// ── Scene store / recall ──────────────────────────────────────────────────
// Scene state blob stored in NVS namespace "scenes", key "sGGGGSS" (hex).
struct SceneState { bool on; uint8_t r, g, b, level; uint16_t mireds; bool isCT; };

static void onStoreScene(uint16_t group_id, uint8_t scene_id) {
    char key[10];
    snprintf(key, sizeof(key), "s%04x%02x", group_id, scene_id);
    SceneState snap = { lightState.on, lightState.r, lightState.g, lightState.b,
                        lightState.level, lightState.mireds, lightState.isCT };
    prefs.begin("scenes", false);
    prefs.putBytes(key, &snap, sizeof(snap));
    prefs.end();
}

static void onRecallScene(uint16_t group_id, uint8_t scene_id) {
    char key[10];
    snprintf(key, sizeof(key), "s%04x%02x", group_id, scene_id);
    SceneState snap;
    prefs.begin("scenes", true);
    size_t len = prefs.getBytes(key, &snap, sizeof(snap));
    prefs.end();
    if (len != sizeof(snap)) return;  // scene not stored yet
    lightState.on     = snap.on;
    lightState.r      = snap.r;
    lightState.g      = snap.g;
    lightState.b      = snap.b;
    lightState.level  = snap.level;
    lightState.mireds = snap.mireds;
    lightState.isCT   = snap.isCT;
    effectState.active = Effect::NONE;
    lightState.dirty  = true;
}

static void onIdentifyEffect(uint8_t effectId, uint8_t /*variant*/) {
    switch (effectId) {
        case 0x00: effectState.active = Effect::BLINK;          break;
        case 0x01: effectState.active = Effect::BREATHE;        break;
        case 0x02: effectState.active = Effect::RAINBOW;        break;
        case 0x0b: effectState.active = Effect::CHANNEL_CHANGE; break;
        case 0xfe: case 0xff:
            effectState.active = Effect::NONE;
            lightState.dirty = true;
            return;
        default: return;
    }
    effectState.startedAt = millis();
}

static void tickEffect() {
    if (effectState.active == Effect::NONE) return;
    uint32_t ms = millis() - effectState.startedAt;
    switch (effectState.active) {
        case Effect::BLINK: {
            fill_solid(leds, NUM_LEDS, ((ms / 200) % 2 == 0) ? CRGB::White : CRGB::Black);
            FastLED.show();
            if (ms >= 1200) { effectState.active = Effect::NONE; lightState.dirty = true; }
            break;
        }
        case Effect::BREATHE: {
            uint8_t b = (uint8_t)(127.5f * (1.0f + sinf((ms % 2000) / 2000.0f * 2.0f * (float)M_PI - (float)M_PI / 2.0f)));
            fill_solid(leds, NUM_LEDS, CRGB(b, b, b));
            FastLED.show();
            if (ms >= 4000) { effectState.active = Effect::NONE; lightState.dirty = true; }
            break;
        }
        case Effect::RAINBOW: {
            uint8_t hue = (uint8_t)(ms / 20);  // full hue cycle ~5 s
            fill_rainbow(leds, NUM_LEDS, hue, 255 / NUM_LEDS);
            FastLED.show();
            // runs until stop_effect or a new light command
            break;
        }
        case Effect::CHANNEL_CHANGE: {
            float bri = ms < 500 ? 1.0f - ms / 500.0f : ms < 1000 ? 0.0f : (ms - 1000) / 500.0f;
            uint8_t b = (uint8_t)(255 * constrain(bri, 0.0f, 1.0f));
            fill_solid(leds, NUM_LEDS, CRGB(b, b, b));
            FastLED.show();
            if (ms >= 1500) { effectState.active = Effect::NONE; lightState.dirty = true; }
            break;
        }
        case Effect::COLOR_LOOP: {
            uint32_t cycle_ms = (uint32_t)effectState.loopTime * 1000;
            float    frac     = (float)(ms % cycle_ms) / (float)cycle_ms;
            uint8_t  hue      = (uint8_t)(frac * 255.0f);
            if (effectState.loopDir == 0) hue = 255 - hue;
            fill_rainbow(leds, NUM_LEDS, hue, 255 / NUM_LEDS);
            FastLED.show();
            break;  // runs until onColorLoopChange(false, ...) stops it
        }
        default: break;
    }
}

// ── NVS state persistence ────────────────────────────────────────────────
static void clearAppNVS() {
    prefs.begin("light",  false); prefs.clear(); prefs.end();
    prefs.begin("scenes", false); prefs.clear(); prefs.end();
}

static void saveState() {
    prefs.begin("light", false);
    prefs.putBool("on",     lightState.on);
    prefs.putUChar("r",     lightState.r);
    prefs.putUChar("g",     lightState.g);
    prefs.putUChar("b",     lightState.b);
    prefs.putUChar("level", lightState.level);
    prefs.putUShort("mir",  lightState.mireds);
    prefs.putBool("isCT",   lightState.isCT);
    prefs.end();
}

static void loadState() {
    prefs.begin("light", true);
    lightState.on     = prefs.getBool("on",      false);
    lightState.r      = prefs.getUChar("r",      255);
    lightState.g      = prefs.getUChar("g",      255);
    lightState.b      = prefs.getUChar("b",      255);
    lightState.level  = prefs.getUChar("level",  DEFAULT_BRIGHTNESS);
    lightState.mireds = prefs.getUShort("mir",   370);
    lightState.isCT   = prefs.getBool("isCT",    false);
    prefs.end();
}

// ── Colour-temperature helpers ────────────────────────────────────────────
static inline uint16_t kelvinToMireds(uint16_t k) { return k ? 1000000u / k : 370; }
static inline uint16_t miredsToKelvin(uint16_t m) { return m ? 1000000u / m : 2700; }

// ── LED helpers ───────────────────────────────────────────────────────────
static void applyLEDs() {
    if (!lightState.on) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
        return;
    }
    if (lightState.isCT) {
        // Planckian-locus approximation for warm→cool white on an RGB LED strip.
        // 2000K: R=255 G=137 B=14  (warm amber-white)
        // 4000K: R=255 G=209 B=163 (neutral white)
        // 6500K: R=255 G=252 B=255 (daylight white)
        uint16_t kelvin = miredsToKelvin(lightState.mireds);
        float t = constrain((float)(kelvin - 2000) / (6500.0f - 2000.0f), 0.0f, 1.0f);
        float b = lightState.level / 255.0f;
        uint8_t rr = 255;
        uint8_t gg = (uint8_t)(137.0f + t * (252.0f - 137.0f));
        uint8_t bb = (uint8_t)(14.0f  + t * (255.0f - 14.0f));
        fill_solid(leds, NUM_LEDS, CRGB((uint8_t)(rr*b), (uint8_t)(gg*b), (uint8_t)(bb*b)));
    } else {
        float b = lightState.level / 255.0f;
        fill_solid(leds, NUM_LEDS,
                   CRGB((uint8_t)(lightState.r * b),
                        (uint8_t)(lightState.g * b),
                        (uint8_t)(lightState.b * b)));
    }
    FastLED.show();
}

// ── Zigbee callbacks — only update state; loop() drives the LEDs ──────────

// Basic on/off callback — registered with the current ZigbeeLight endpoint
void onOnOffChange(bool state) {
    effectState.active = Effect::NONE;
    lightState.on = state;
    lightState.dirty = true;
}

// ── Colour callbacks (kept for future use, not registered) ───────────────
// Called for colour scenes (XY → RGB conversion handled by the stack)
void onRgbChange(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
    effectState.active = Effect::NONE;
    lightState.on    = state;
    lightState.r     = r;
    lightState.g     = g;
    lightState.b     = b;
    lightState.level = level;
    lightState.isCT  = false;
    lightState.dirty = true;
}

// Called when Hue bridge sends Move-to-Hue-and-Saturation / Move-Hue commands
void onHsvChange(bool state, uint8_t hue, uint8_t sat, uint8_t value) {
    effectState.active = Effect::NONE;
    // Convert HSV to RGB with full value; the level attribute handles brightness.
    CRGB rgb;
    hsv2rgb_rainbow(CHSV(hue, sat, 255), rgb);
    lightState.on    = state;
    lightState.r     = rgb.r;
    lightState.g     = rgb.g;
    lightState.b     = rgb.b;
    lightState.level = value;
    lightState.isCT  = false;
    lightState.dirty = true;
}

// Called for white / colour-temperature scenes
void onTempChange(bool state, uint8_t level, uint16_t mireds) {
    effectState.active = Effect::NONE;
    lightState.on     = state;
    lightState.level  = level;
    lightState.mireds = mireds ? mireds : 370;
    lightState.isCT   = true;
    lightState.dirty  = true;
}

// Called by the coordinator / Hue bridge when it wants to identify the device
void onIdentify(uint16_t timeSeconds) {
    lightState.dirty = true;
}

// Called when ZCL Color Loop Set command (0x44) activates or deactivates the color loop
void onColorLoopChange(bool active, uint8_t direction, uint16_t time_s) {
    if (active) {
        effectState.loopDir   = direction;
        effectState.loopTime  = time_s ? time_s : 25;
        effectState.active    = Effect::COLOR_LOOP;
        effectState.startedAt = millis();
    } else if (effectState.active == Effect::COLOR_LOOP) {
        effectState.active = Effect::NONE;
        lightState.dirty   = true;
    }
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    loadState();

    // ── WS2812B strip ────────────────────────────────────────────────────
    FastLED.addLeds<WS2812B, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
           .setCorrection(TypicalLEDStrip);
    // Hard cap to protect the strip and power supply
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);  // 5 V, 2 A
    FastLED.setBrightness(DEFAULT_BRIGHTNESS);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    lightState.dirty = true;  // apply loaded state on first loop iteration

    // ── Factory-reset button ─────────────────────────────────────────────
    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

    // ── Zigbee endpoint configuration ────────────────────────────────────
    // Device name shown in the Hue app
    zbLight.setManufacturerAndModel(DEVICE_MANUFACTURER, DEVICE_MODEL);

    zbLight.onLightChangeRgb(onRgbChange);
    zbLight.onLightChangeHsv(onHsvChange);
    zbLight.onLightChangeTemp(onTempChange);
    zbLight.onIdentify(onIdentify);
    zbLight.onColorLoop(onColorLoopChange);

    // Declare mains power and firmware versions — read by the Hue bridge
    // during device interview (Basic cluster attribute read).
    zbLight.setPowerSource(ZB_POWER_SOURCE_MAINS);
    zbLight.setVersion(1);
    zbLight.setHardwareVersion(1);
    // Enable all five ZCL ColorCapabilities bits so the Hue bridge (and Google
    // Home via Hue) can use HS, XY and CT modes.  Without this the internal
    // guard in ZigbeeColorDimmableLight silently drops every HS and CT command
    // even though the ZCL attribute already advertises full capabilities.
    zbLight.setLightColorCapabilities(
        ZIGBEE_COLOR_CAPABILITY_HUE_SATURATION |
        ZIGBEE_COLOR_CAPABILITY_ENHANCED_HUE   |
        ZIGBEE_COLOR_CAPABILITY_COLOR_LOOP     |
        ZIGBEE_COLOR_CAPABILITY_X_Y            |
        ZIGBEE_COLOR_CAPABILITY_COLOR_TEMP);
    // Advertise the physical CT range that applyLEDs() can reproduce.
    // 153 mireds = 6500 K (cool white), 500 mireds = 2000 K (warm white).
    zbLight.setLightColorTemperatureRange(153, 500);

    Zigbee.addEndpoint(&zbLight);
    Zigbee.onFactoryReset(clearAppNVS);

    // This is a mains-powered device — disable radio sleep so the coordinator
    // can reach it at any time (required for zigbee2mqtt interview to succeed).
    esp_zb_sleep_enable(false);

    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to initialise — rebooting");
        ESP.restart();
    }

    Serial.println("Connecting to Zigbee network...");
    // Use full brightness for discovery pulsation; restore afterward.
    FastLED.setBrightness(255);
    while (!Zigbee.connected()) {
        // Pulsate blue to indicate discovery / pairing mode (3-second breath cycle).
        // Phase +π/2 so the wave starts at full brightness (immediately visible).
        uint8_t b = (uint8_t)(127.5f * (1.0f + sinf(
            (millis() % 3000) / 3000.0f * 2.0f * (float)M_PI + (float)M_PI / 2.0f)));
        fill_solid(leds, NUM_LEDS, CRGB(0, 0, b));
        FastLED.show();
        delay(20);
        // Factory-reset check while waiting to join
        if (digitalRead(FACTORY_RESET_PIN) == LOW) {
            delay(100);
            unsigned long t = millis();
            while (digitalRead(FACTORY_RESET_PIN) == LOW) {
                delay(50);
                if (millis() - t > 3000) {
                    Serial.println("\nFactory resetting Zigbee...");
                    delay(500);
                    Zigbee.factoryReset();
                }
            }
        }
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.setBrightness(DEFAULT_BRIGHTNESS);
    FastLED.show();
    Serial.println("\nConnected!");
    zigbee_identify_effect_cb = onIdentifyEffect;
    zigbee_scene_store_cb     = onStoreScene;
    zigbee_scene_recall_cb    = onRecallScene;
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    static bool wasConnected = false;
    if (Zigbee.connected() && !wasConnected) {
        wasConnected = true;
        Serial.println("Zigbee network connected!");
        effectState.active    = Effect::BLINK;
        effectState.startedAt = millis();
    }

    tickEffect();

    // Apply pending LED state from Zigbee callbacks
    if (lightState.dirty) {
        lightState.dirty = false;
        applyLEDs();
        saveState();
    }

    // Hold BOOT button for ≥ 3 s to factory-reset and re-pair
    if (digitalRead(FACTORY_RESET_PIN) == LOW) {
        delay(100);
        unsigned long pressedAt = millis();
        while (digitalRead(FACTORY_RESET_PIN) == LOW) {
            delay(50);
            if (millis() - pressedAt > 3000) {
                Serial.println("Factory resetting Zigbee — rebooting in 1 s");
                delay(1000);
                Zigbee.factoryReset();
            }
        }
    }
    delay(10);
}
