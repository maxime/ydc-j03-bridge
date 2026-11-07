# Yamazen YDC-J03 ESP32-S3 bridge

Control a Yamazen YDC-J03 dehumidifier over Wi-Fi from a browser or Home Assistant. An ESP32-S3 sits between the original control panel and the main board. It passes signals in both directions so the physical buttons and display keep working, and it replays captured button pulses to send remote commands. [Yamazen product page](https://book.yamazen.co.jp/product/detail/I00009188) · [Amazon Japan listing](https://www.amazon.co.jp/dp/B0DXPG7LFN)

## Control the dryer

The built-in web page shows the panel display and lets you control power, humidity, fan speed, sleep mode, and the timer. You can set a humidity target from 40% to 70% in steps of 5%. The YDC-J03 shuts off after 1 to 8 hours on its timer, and 0 clears it. The web On and Off buttons both send the same physical power toggle. [Yamazen manual](https://book.yamazen.co.jp/downloads/W7N4004PYDC-J03/)

With the ESPHome build, open `http://yamazen-air-dryer.local/` or use the ESP32's IP address. These HTTP requests read the panel and step the fan speed from a script.

```bash
curl 'http://yamazen-air-dryer.local/state'
curl 'http://yamazen-air-dryer.local/cmd?name=fan'
```

The web interface and Telnet accept commands without credentials. Keep both on a trusted local network.

## Hardware and wiring

The firmware targets an ESP32-S3-DevKitC-1 N16R8 with 16 MB of flash and 8 MB of octal PSRAM. The installation replaces the dryer's two panel signal connections with the ESP32 bridge. Unplug the dryer before opening it. Verify the control signals' voltage and isolation from mains before connecting the ESP32 or its USB cable. [Wiring and protocol notes](docs/air-dryer.md)

## ESPHome and Home Assistant

Use ESPHome 2026.10.2 or newer. Copy [the secrets example](esphome/secrets.yaml.example) to `esphome/secrets.yaml`, enter your Wi-Fi details, and generate an API encryption key with `openssl rand -base64 32`.

For the first install on a fresh board, connect its USB port and run the command below from the repository root. Select the USB serial port when prompted.

```bash
esphome run esphome/yamazen-air-dryer.yaml
```

Add the discovered ESPHome device in Home Assistant and enter the same encryption key. If discovery is unavailable, add the ESPHome integration manually using the ESP32's IP address.

Home Assistant gets a Power switch, a Humidity sensor for history, and a Panel Display text sensor. Enter a Humidity Target or Shutoff Timer directly. Each field shows the last setting the bridge confirmed. Fan Speed offers Low and High and follows the physical panel while the dryer is running. Sleep Toggle is separate and forces the fan to Low until you leave Sleep mode. Humidity history pauses while the panel is off or showing nonnumeric text, and during timer or setpoint adjustments and for 10 seconds afterward.

| Home Assistant action | Input |
| --- | --- |
| `esphome.yamazen_air_dryer_set_humidity` | `percent`, 40 to 70 in steps of 5 |
| `esphome.yamazen_air_dryer_set_timer` | `hours`, 0 to 8 |

After the first install, update over Wi-Fi with the dryer idle. You can use the same configuration in ESPHome Device Builder by placing the device YAML, both `dryer_bridge` source files, and `partitions.csv` in its configuration directory and adding the secret keys to its `secrets.yaml`. [ESPHome OTA documentation](https://esphome.io/components/ota/esphome/)

```bash
esphome run esphome/yamazen-air-dryer.yaml --device yamazen-air-dryer.local
```

### Migrating from the standalone firmware

To preserve a power waveform learned by the standalone firmware, install over USB with the individual images listed in `flash_args`. This uses the original partition layout and leaves the NVS partition that holds the waveform alone. Run the commands from the repository root and substitute your USB serial port for `YOUR_USB_PORT`.

```bash
esphome compile esphome/yamazen-air-dryer.yaml
cd esphome/.esphome/build/yamazen-air-dryer/build
esptool --chip esp32s3 --port YOUR_USB_PORT write-flash @flash_args
```

### Second dryer

Give each dryer a unique network name. Save this as `esphome/yamazen-air-dryer-2.yaml` to reuse the first configuration. [ESPHome node names](https://esphome.io/components/esphome/#configuration-variables)

```yaml
packages:
  dryer: !include yamazen-air-dryer.yaml

esphome:
  name: yamazen-air-dryer-2
  friendly_name: Yamazen Air Dryer 2
```

Install this configuration on the second board over USB. Use its YAML and `yamazen-air-dryer-2.local` for later OTA updates.

## Standalone PlatformIO firmware

The original firmware in [`src/main.cpp`](src/main.cpp) provides the web controls on its own. Copy `src/secrets.h.example` to `src/secrets.h`, enter your Wi-Fi details, and connect the board's native USB port. Run these commands from the repository root.

```bash
pio run -t upload
pio device monitor
```

Open the device IP shown in the serial log in your browser. Both builds expose frame logs on USB serial at 115200 baud and over Telnet on port 23. ESPHome keeps the onboard RGB debug LED off.

Run `python3 esphome/check_protocol.py` to compare the command tables, measured pulses, and replay code in the two builds.
