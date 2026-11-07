# ESP32-S3 test

Wi-Fi name and password are in `src/secrets.h`. Copy `src/secrets.h.example` to `src/secrets.h` and fill them in. That file stays off Git.

From this folder:

```bash
pio run
pio run -t upload
pio device monitor
```

Upload and open the monitor together:

```bash
pio run -t upload && pio device monitor
```

Leave the monitor with `Ctrl+C`.

If `pio` is not found:

```bash
~/.platformio/penv/bin/pio run -t upload && ~/.platformio/penv/bin/pio device monitor
```

Use the USB socket labeled **USB** (the native port). Serial output is on that same port at 115200.

The ESP32 bridges the panel and the main board. Wiring is in `docs/air-dryer.md`: panel TX to GPIO 16, panel RX from GPIO 17, main TX to GPIO 18, main RX from GPIO 15. After boot, open the button page at http://192.168.3.164/ . The buttons are On, Off, Humidity, Fan, Sleep, and Timer. Telnet on port 23 still prints every frame. Close the monitor with `Ctrl+C` before uploading.
