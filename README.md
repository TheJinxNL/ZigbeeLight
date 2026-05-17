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
| `NUM_LEDS` | `8` | Number of LEDs in the strip |
| `COLOR_ORDER` | `GRB` | Byte order (WS2812B = GRB) |
| `DEFAULT_BRIGHTNESS` | `50` | Power-on brightness (0–255) |
| `ZIGBEE_ENDPOINT` | `10` | Zigbee endpoint number |
| `ZIGBEE_CHANNEL` | `25` | Zigbee channel (must match coordinator) |
| `DEVICE_MANUFACTURER` | `"JINX"` | Shown in Hue app / z2m |
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
> `lib/Zigbee/`. PlatformIO uses it automatically in place of the global package.
> Patches applied vs upstream:
> - **`TriggerEffect` ZCL command** (`ESP_ZB_CORE_IDENTIFY_EFFECT_CB_ID`) — added to `ZigbeeHandlers.cpp`; missing from upstream
> - **Scene store / recall** (`ESP_ZB_CORE_SCENES_STORE_SCENE_CB_ID` / `_RECALL_SCENE_CB_ID`) — added to `ZigbeeHandlers.cpp`; missing from upstream
> - **Philips Hue TC link key** — `ZigbeeCore.cpp` calls `esp_zb_enable_joining_to_distributed()` and sets the ZLL commissioning key before stack start; without this the bridge sends an immediate `ZDO Leave`
> - **`On/Off` cluster attributes** — `on_time` and `global_scene_control` added to `ZigbeeColorDimmableLight`; required for the Hue "Off with Effect" command (ZCL 0x40) to be processed
> - **Color capabilities guard** — `setLightColorCapabilities(0x001F)` exposed; without it the internal guard silently drops all HS and CT commands from the bridge
> - **CT range** — `setLightColorTemperatureRange(153, 500)` exposed; advertises the physical 2000–6500 K range to the bridge and Google Home

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
3. If the device has previously been on a different network, hold **BOOT** for
   3 s to factory-reset it first
4. Hue app → Settings → Light setup → **Add light → Search**
5. The device joins automatically — no button press required during search

> The firmware includes all three compatibility fixes required by the Hue bridge:
> ZLL TC link key, `app_device_version = 1`, and On/Off cluster attributes for
> "Off with Effect". No bridge configuration changes are needed.

## Zigbee Features

| Feature | ZCL cluster | Notes |
|---|---|---|
| On / Off | `OnOff` (0x0006) | |
| Brightness | `LevelControl` (0x0008) | 0–255 |
| RGB colour | `ColorControl` (0x0300) | Hue/Saturation, Enhanced Hue, XY |
| Colour temperature | `ColorControl` (0x0300) | 153–500 mireds (2000–6500 K) |
| Color Loop | `ColorControl` (0x0300) | Rainbow cycle via ZCL Color Loop Set (0x44) |
| Scenes | `Scenes` (0x0005) | Store / recall via Hue scenes |
| State persistence | NVS | On/RGB/level/CT/mireds saved across reboots |
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

> Any normal light command (colour, brightness, on/off) also stops an active effect.

### Color Loop

Activate the color loop from the Hue app's scene / entertainment effects menu.
The bridge sends a ZCL **Color Loop Set** command (0x44) which writes the
`ColorLoopActive` attribute. The firmware then cycles the full hue wheel across
all LEDs continuously. Direction and speed are controlled by the `ColorLoopDirection`
and `ColorLoopTime` ZCL attributes sent with the command.

The loop runs until the bridge sends `ColorLoopActive = 0`, or until any normal
colour / brightness / on/off command is received.

### Scenes

The Hue app stores and recalls scenes using ZCL **Store Scene** and **Recall Scene**
commands. This firmware saves a complete light-state snapshot (on/off, RGB, level,
colour temperature, CT mode) in NVS keyed by `(group_id, scene_id)` and restores it
on recall. An arbitrary number of scenes can be stored up to the NVS capacity.

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
