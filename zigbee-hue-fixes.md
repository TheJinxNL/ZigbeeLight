# Philips Hue + ESP32 Zigbee — Confirmed Fixes

## Hardware
- ESP32-H2-DevKitM-1, WS2812B on GPIO 8, PlatformIO pioarduino/platform-espressif32 55.03.38-1

## Fix 1: ZLL TC Link Key (root cause of instant LEAVE / zero APS frames)
In `ZigbeeCore.cpp` `esp_zb_task()`, BEFORE `esp_zb_start()`:
```cpp
#include "esp_zigbee_secur.h"   // add to includes

esp_zb_enable_joining_to_distributed(true);
uint8_t hue_tclk[] = {0x81, 0x42, 0x86, 0x86, 0x5D, 0xC1, 0xC8, 0xB2,
                       0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8};
esp_zb_secur_TC_standard_distributed_key_set(hue_tclk);
```
- Hue uses the ZLL commissioning key (not `ZigBeeAlliance09`) for TC link key transport
- Without this the device cannot decrypt the transported NWK key → bridge sends `ZDO Leave` at ~700 ms with zero APS frames exchanged
- `esp_zb_enable_joining_to_distributed` is in `esp_zigbee_core.h` guarded by `ZB_DISTRIBUTED_SECURITY_ON` (pre-defined in `zb_vendor_default.h` via `zboss_api.h` — no extra build flag needed)

## Fix 2: app_device_version = 1
Call `zbLight.setVersion(1)` in `setup()` before `Zigbee.begin()`.
This sets `_ep_config.app_device_version = 1` in the endpoint descriptor.

Without this the Hue bridge caches the device (per-MAC) and refuses to interview it on subsequent attempts — the device joins the network but never appears in the Hue app.

## Fix 3: "Off with Effect" (ZCL command 0x40)
In `ZigbeeColorDimmableLight.cpp` constructor, after cluster list creation:
```cpp
uint16_t on_time = 0;
uint8_t global_scene_ctrl = 0;
esp_zb_attribute_list_t *on_off_cluster = esp_zb_cluster_list_get_cluster(
    _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
esp_zb_on_off_cluster_add_attr(on_off_cluster, ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME, &on_time);
esp_zb_on_off_cluster_add_attr(on_off_cluster, ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL, &global_scene_ctrl);
```
- The Hue bridge sends "Off with Effect" (ZCL command 0x40) instead of plain "Off" (0x00)
- Without `on_time` and `global_scene_control` present in the cluster, ZBOSS silently ignores the command

## Diagnostic tip
Enable APS-frame logging to distinguish TCLK failures (Fix 1) from interview failures (Fix 2):
- If you see `ZDO Leave` with **zero APS frames** → Fix 1 is missing
- If the device joins (`Joined network successfully`) but Hue never shows it → Fix 2 is missing
- If on/off works but "off" from Hue app does nothing → Fix 3 is missing

## References
- https://wejn.org/2025/01/zigbee-hue-llo-world/
- https://github.com/espressif/esp-zigbee-sdk/issues/358
- https://github.com/espressif/esp-zigbee-sdk/issues/519
