# Zigbee WS2812B Light Strip

ESP32-H2 firmware for a Zigbee-controlled WS2812B LED strip. Integrates with
**zigbee2mqtt** (Home Assistant) and the **Philips Hue bridge** as a standard
Color Dimmable Light (ZHA device type 0x0102).

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-H2-DevKitM-1 |
| LED strip | WS2812B addressable LEDs |
| Data pin | GPIO8 |
| Factory reset | GPIO9 (BOOT button, hold ≥ 3 s) |
| Power | 5 V / 2 A (USB-C or dedicated supply) |

### Wiring

```
5V supply ──┬── ESP32-H2 5V pin
            └── WS2812B VCC

GND ────────┬── ESP32-H2 GND
            └── WS2812B GND

ESP32-H2 GPIO8 ──[300Ω]── WS2812B DIN
```

> For permanent installations, power the strip directly from a 5 V supply
> and connect only GND and the data line to the ESP32-H2.

## Configuration

Edit [src/config.h](src/config.h) before building:

| Define | Default | Description |
|---|---|---|
| `LED_DATA_PIN` | `8` | GPIO connected to WS2812B DIN |
| `NUM_LEDS` | `18` | Number of LEDs in the strip |
| `COLOR_ORDER` | `GRB` | Byte order (WS2812B = GRB) |
| `DEFAULT_BRIGHTNESS` | `50` | Power-on brightness (0–255) |
| `ZIGBEE_ENDPOINT` | `10` | Zigbee endpoint number |
| `ZIGBEE_CHANNEL` | `11` | Zigbee channel (must match coordinator) |
| `DEVICE_MANUFACTURER` | `"Espressif"` | Shown in Hue app / z2m |
| `DEVICE_MODEL` | `"ZBLightStrip"` | Shown in Hue app / z2m |
| `FACTORY_RESET_PIN` | `9` | BOOT button GPIO |

## Build & Flash

Requires [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Flash
pio run --target upload

# Full erase + flash (required when changing Zigbee mode or partition table)
pio run --target erase
pio run --target upload

# Serial monitor
pio device monitor
```

> The project includes a patched copy of the Arduino Zigbee library under
> `lib/Zigbee/`. PlatformIO automatically uses this instead of the global
> package. The patch adds support for the `TriggerEffect` ZCL command
> (`ESP_ZB_CORE_IDENTIFY_EFFECT_CB_ID`), which is missing from the upstream
> library.

## Pairing

### zigbee2mqtt

1. Set `ZIGBEE_CHANNEL` in [src/config.h](src/config.h) to match your z2m channel
   (check **zigbee2mqtt → Settings → Advanced → ZigBee channel**)
2. Build and upload
3. In zigbee2mqtt, click **Permit join (All)**
4. Press the **RESET** button on the ESP32-H2
5. The device joins within one scan cycle and appears in the Devices list

> If the device has previously joined a different network, hold the **BOOT**
> button for 3 seconds after boot to factory-reset before joining.

### Philips Hue bridge

1. Set `ZIGBEE_CHANNEL` to match your bridge (Hue app → Settings →
   Hue Bridges → ⓘ → Zigbee channel — typically 15, 20, or 25)
2. Build and upload
3. Hue app → Settings → Light setup → **Add light → Search**
4. While the search is active, press **RESET** on the ESP32-H2

## Zigbee Features

| Feature | ZCL cluster | Notes |
|---|---|---|
| On / Off | `OnOff` (0x0006) | |
| Brightness | `LevelControl` (0x0008) | 0–255 |
| RGB colour | `ColorControl` (0x0300) | XY colour space |
| Colour temperature | `ColorControl` (0x0300) | 153–500 mireds (2000–6500 K) |
| Identify / Effects | `Identify` (0x0003) | See effects table below |

### Identify Effects

Effects are triggered from the zigbee2mqtt device page (Effect dropdown) or via
ZCL `TriggerEffect` commands:

| ID | z2m name | LED behaviour |
|---|---|---|
| 0x00 | `blink` | 3 × white flash (200 ms on/off, ~1.2 s total) |
| 0x01 | `breathe` | Sine-wave brightness pulse (~4 s) |
| 0x02 | `okay` | Continuous rainbow cycle across all LEDs |
| 0x0b | `channel_change` | Fade out → pause → fade in (1.5 s) |
| 0xfe | `finish_effect` | Stop effect, restore light state |
| 0xff | `stop_effect` | Stop effect, restore light state |

> **Note:** The `colorloop` and `stop_colorloop` entries shown in zigbee2mqtt do
> not send a `TriggerEffect` command and have no effect on this firmware.
> Use `okay` to start the rainbow and `stop_effect` to stop it.
> Any normal light command (colour, brightness, on/off) also stops the effect.

## Partition Table

A custom partition table ([partitions.csv](partitions.csv)) is required to
provide the Zigbee stack storage partitions used by ZBOSS:

| Partition | Size | Purpose |
|---|---|---|
| `nvs` | 20 KB | Arduino NVS |
| `otadata` | 8 KB | OTA state |
| `app0` / `app1` | 1280 KB each | OTA firmware slots |
| `zb_storage` | 16 KB | Zigbee NVRAM (network keys, tables) |
| `zb_fct` | 4 KB | Zigbee factory/calibration data |
| `coredump` | 64 KB | Crash core dump |

## Troubleshooting

**Device does not appear in zigbee2mqtt after joining**
- Click the re-interview button (↺) next to the device in z2m Devices
- Ensure the device is physically close to the coordinator during interview

**Interview fails with "can not get node descriptor"**
- Add to `configuration.yaml` in your zigbee2mqtt config:
  ```yaml
  advanced:
    interview_retry_timeout: 15000
    node_descriptor_retrieval_timeout: 30
  ```
- Restart zigbee2mqtt

**Device keeps rebooting without joining**
- Check `ZIGBEE_CHANNEL` matches the coordinator channel
- Hold BOOT for 3 s to factory-reset and clear stale network state
- Move the device closer to the coordinator during pairing

**No serial output**
- Ensure the serial monitor is open before the device boots
- Confirm `ARDUINO_USB_CDC_ON_BOOT=1` is set in [platformio.ini](platformio.ini)
