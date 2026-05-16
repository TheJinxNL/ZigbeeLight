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
#include "config.h"

// ── Globals ───────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];
ZigbeeColorDimmableLight zbLight(ZIGBEE_ENDPOINT);

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
enum class Effect : uint8_t { NONE, BLINK, BREATHE, RAINBOW, CHANNEL_CHANGE };
struct { volatile Effect active = Effect::NONE; uint32_t startedAt = 0; } effectState;

// Function pointer called by patched ZigbeeHandlers.cpp for TriggerEffect commands
void (*zigbee_identify_effect_cb)(uint8_t, uint8_t) = nullptr;

static void onIdentifyEffect(uint8_t effectId, uint8_t /*variant*/) {
    Serial.printf("Effect ID: 0x%02X\n", effectId);
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
        default: break;
    }
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
        uint16_t kelvin = miredsToKelvin(lightState.mireds);
        uint8_t warm = (uint8_t)constrain(map(kelvin, 2000, 6500, 255, 0), 0, 255);
        uint8_t cool = (uint8_t)constrain(map(kelvin, 2000, 6500,   0, 255), 0, 255);
        float b = lightState.level / 255.0f;
        fill_solid(leds, NUM_LEDS, CRGB((uint8_t)(warm*b), (uint8_t)(warm*b), (uint8_t)(cool*b)));
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
    Serial.printf("OnOff state=%d\n", state);
    effectState.active = Effect::NONE;
    lightState.on = state;
    lightState.dirty = true;
}

// ── Colour callbacks (kept for future use, not registered) ───────────────
// Called for colour scenes (XY → RGB conversion handled by the stack)
void onRgbChange(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
    Serial.printf("RGB  state=%d  r=%u g=%u b=%u  level=%u\n", state, r, g, b, level);
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
    Serial.printf("HSV  state=%d  h=%u s=%u v=%u\n", state, hue, sat, value);
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
    Serial.printf("CT   state=%d  level=%u  mireds=%u\n", state, level, mireds);
    effectState.active = Effect::NONE;
    lightState.on     = state;
    lightState.level  = level;
    lightState.mireds = mireds ? mireds : 370;
    lightState.isCT   = true;
    lightState.dirty  = true;
}

// Called by the coordinator / Hue bridge when it wants to identify the device
void onIdentify(uint16_t timeSeconds) {
    Serial.printf("Identify for %u s\n", timeSeconds);
    lightState.dirty = true;
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // ── WS2812B strip ────────────────────────────────────────────────────
    FastLED.addLeds<WS2812B, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
           .setCorrection(TypicalLEDStrip);
    // Hard cap to protect the strip and power supply
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);  // 5 V, 2 A
    FastLED.setBrightness(DEFAULT_BRIGHTNESS);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();

    // ── Factory-reset button ─────────────────────────────────────────────
    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

    // ── Zigbee endpoint configuration ────────────────────────────────────
    // Device name shown in the Hue app
    zbLight.setManufacturerAndModel(DEVICE_MANUFACTURER, DEVICE_MODEL);

    zbLight.onLightChangeRgb(onRgbChange);
    zbLight.onLightChangeHsv(onHsvChange);
    zbLight.onLightChangeTemp(onTempChange);
    zbLight.onIdentify(onIdentify);

    // Declare mains power and firmware versions — read by the Hue bridge
    // during device interview (Basic cluster attribute read).
    zbLight.setPowerSource(ZB_POWER_SOURCE_MAINS);
    zbLight.setVersion(1);
    zbLight.setHardwareVersion(1);

    Zigbee.addEndpoint(&zbLight);

    // Restrict scan to the coordinator's channel for fast joining.
    Zigbee.setPrimaryChannelMask(1 << ZIGBEE_CHANNEL);

    // This is a mains-powered device — disable radio sleep so the coordinator
    // can reach it at any time (required for zigbee2mqtt interview to succeed).
    esp_zb_sleep_enable(false);

    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to initialise — rebooting");
        ESP.restart();
    }

    Serial.println("Connecting to Zigbee network...");
    while (!Zigbee.connected()) {
        Serial.print('.');
        delay(100);
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
    Serial.println("\nConnected!");
    zigbee_identify_effect_cb = onIdentifyEffect;
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    static bool wasConnected = false;
    if (Zigbee.connected() && !wasConnected) {
        wasConnected = true;
        Serial.println("Zigbee network connected!");
    }

    tickEffect();

    // Apply pending LED state from Zigbee callbacks
    if (lightState.dirty) {
        lightState.dirty = false;
        applyLEDs();
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
