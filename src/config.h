#pragma once

// ─────────────────────────────────────────────
//  LED Strip
// ─────────────────────────────────────────────

// GPIO pin connected to the WS2812B data line.
// On the ESP32-H2-DevKitM-1 any free GPIO works; GPIO8 is a safe choice.
#define LED_DATA_PIN        8

// Total number of LEDs in the strip
#define NUM_LEDS            8

// WS2812B uses GRB byte order (not RGB)
#define COLOR_ORDER         GRB

// Power-on brightness (0–255). Kept low to limit inrush current.
#define DEFAULT_BRIGHTNESS  50

// ─────────────────────────────────────────────
//  Zigbee
// ─────────────────────────────────────────────

// Zigbee endpoint number (1–240; 10 is conventional for a single light)
#define ZIGBEE_ENDPOINT     10

// Zigbee channel to join (must match your coordinator).
// Common values: zigbee2mqtt=11, Hue bridge=11/15/20/25.
// Check: Hue app → Settings → Hue Bridges → i → Zigbee channel.
#define ZIGBEE_CHANNEL      11

// ─────────────────────────────────────────────
//  Device identity
//  Shown in the Hue app; keep under 32 chars each.
// ─────────────────────────────────────────────
#define DEVICE_MANUFACTURER "JINX"
#define DEVICE_MODEL        "ZBLightStrip"

// ─────────────────────────────────────────────
//  Hardware — BOOT button (hold 3 s → factory reset)
// ─────────────────────────────────────────────
#define FACTORY_RESET_PIN   9   // GPIO9 = BOOT button on ESP32-H2-DevKitM-1
