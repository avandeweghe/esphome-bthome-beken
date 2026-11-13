# esphome-bthome-beken

ESPHome external component that broadcasts [BTHome v2](https://bthome.io) over
Bluetooth LE on Beken BK72xx chips (tested on BK7231N), using the Beken SDK's own
BLE stack. LibreTiny ships with that stack switched off; this component turns it
on and drives it directly.

Built for battery sensors (for example the Tuya XT-V2 water leak sensor): wake,
broadcast for about a second, deep sleep. Home Assistant picks the readings up
through any Bluetooth proxy with the BTHome integration.

## Use

```yaml
esphome:
  platformio_options:
    custom_options.sys_config#h:
      - "CFG_SUPPORT_BLE = 1"   # must be a list

external_components:
  - source: github://avandeweghe/esphome-bthome-beken
    components: [bthome_beken]

bthome_beken:
  id: ble
  local_name: "Sump"          # up to 8 characters
  moisture: water_leak        # binary_sensor id (optional)
  battery: battery_level      # sensor id, % (optional)
  adv_interval: 50ms
  auto_start: true            # false: start later with bthome_beken.start
  refresh_interval: 0s        # >0: re-send at this interval even with no change
  on_broadcast:               # advertising started
    - logger.log: "on air"
  on_packet:                  # every time new data goes out
    - light.turn_on: led
```

Actions:

- `bthome_beken.start: ble` starts the BLE stack (with `auto_start: false`).
- `bthome_beken.button_event: {id: ble, type: press}` sends a button event
  (`press`, `double_press`, `long_press`).

Broadcast objects: packet id, battery, moisture, button, and a "count" holding
the milliseconds from boot to the first broadcast.

See [examples/tuya-water-sensor.yaml](examples/tuya-water-sensor.yaml) for a
complete deep-sleep water sensor with a Wi-Fi update mode on a button hold.

## Notes

- Do not set `wifi: enable_on_boot: false` on BK72xx; it crashes in OTA setup.
  Let Wi-Fi start and `wifi.disable` it in `on_boot` (priority 200).
- Start BLE from the main loop, not during setup, and not while Wi-Fi is
  associating.
- The packet id is kept in flash so Home Assistant does not drop a wake as a
  duplicate after deep sleep.
- `deep_sleep` on BK72xx has no `IGNORE` wakeup mode; use `INVERT_WAKEUP` for a
  pin that may stay active.

## License

MIT
