# Air dryer bridge

The ESP32-S3 sits between the control panel and the main board. Both original signal wires are disconnected and landed on the ESP32.

## Wiring

Both directions are a pulse train that also reads as 4800 8N1, and both idle high. The ESP32 copies each edge as it happens, so the 200 µs and 400 µs pulses are not rebuilt as UART bytes. Tie the grounds together. Pass-through is on from boot.

| Signal | ESP32 pin | Notes |
|---|---|---|
| Panel TX | GPIO 16 | Into the ESP32. Use the 10k/20k divider if this wire is 5 V. |
| Panel RX | GPIO 17 | Out of the ESP32, 3.3 V, no divider. |
| Main TX | GPIO 18 | Into the ESP32. Use the 10k/20k divider if this wire is 5 V. |
| Main RX | GPIO 15 | Out of the ESP32, 3.3 V push-pull, no divider. |

The wire that showed button codes is panel TX, so it goes to GPIO 16. The steady 84 ms stream is main TX, so it goes to GPIO 18. GPIO 17 feeds the panel, and GPIO 15 feeds the main board.

## What the bridge does

Telnet prints each frame. From boot, every edge is copied in both directions. `pass off` stops the copy. On, Off, Humidity, Fan, Sleep, and Timer replay one saved pulse train on GPIO 15. A repeated Humidity, Fan, Sleep, or Timer train from the panel is not copied for 2 seconds, so one press is one step. A panel press that is not the idle hello also prints `wave` and the pulse edges.

| LED | Meaning |
|---|---|
| Blue | Running, no Telnet client |
| Green | Telnet connected |
| Yellow | Panel frame relayed to the main board |
| Cyan | Main-board frame relayed to the panel |
| Red | Command typed on Telnet |

## Line encoding

The wire is an NEC-style pulse-distance 1-wire protocol that idles high. Each frame starts with a ~4.4 ms LOW pulse and a ~2.25 ms HIGH header, followed by 250 µs LOW pulses separated by ~250 µs HIGH (`0`) or ~750 µs HIGH (`1`), MSB first, ending with an 8-bit checksum `(sum + 0x5A) & 0xFF`.

- **Panel → Main** is 3 bytes (24 bits): `[0x30, button_mask, checksum]`, where `button_mask` is `0x00` idle, `0x01` On/Off, `0x02` Humidity, `0x04` Timer, `0x08` Sleep, `0x10` Fan.
- **Main → Panel** is 9 bytes (72 bits): `[0x90, digit1_7seg, digit2_7seg, ..., checksum]`. `digit1_7seg` and `digit2_7seg` are standard 7-segment codes (`0x3F`=`0`, `0x06`=`1`, `0x5B`=`2`, `0x4F`=`3`, `0x66`=`4`, `0x6D`=`5`, `0x7D`=`6`, `0x07`=`7`, `0x7F`=`8`, `0x6F`=`9`, `0x00`=blank).

The hex in the Telnet log is what a 4800 8N1 UART reads from those pulses (`00` for the start pulse, then `A9`/`AF`/`B9`/`E9`/`EF`/`F9`). A `FF` between frames is the idle gap, not a data byte.

## Frames

Frames start with `00 89 A8`. A `FF` between frames is only the gap.

| Name | Frame | Notes |
|---|---|---|
| Status | `00 89 A8 A9 A9 89 29 A1 0E` | Seen about every 6.3 s. Last byte also appears as `0F`. |
| On | `00 E9 AF A9 A9 B9 A9 B9` | Fan toggle. Web On replays the saved 54-edge panel burst. |
| Off | same 54-edge burst as On | That burst toggles the fan, so Off replays it too. |
| Humidity | `00 E9 AF A9 A9 AF A9 AF` | One step. Web Humidity replays the saved 54-edge burst. |
| Fan | `00 E9 AF A9 E9 A9 E9 E9 F9` | Same for high and low. Web Fan replays the saved burst. |
| Sleep | `00 E9 AF A9 B9 A9 E9 AF F9` | Same for on and off. Web Sleep replays the saved burst. |
| Timer | `00 E9 AF A9 A9 E9 A9 EF F9` | One step. Web Timer replays one saved 54-edge burst. |

The 6.3 s status frame did not change when the humidity setpoint moved from 45% to 60%. The number on the panel, including `HI`, is expected on the main-board-to-panel side. Telnet marks a main-board frame `unchanged`, or lists the bytes that moved (`b5:29>2D`).

## Telnet commands

These are sent to the main board as if the panel had sent them:

```text
main norm
main inv
main 9600
panel 4800 inv
pass on
tx norm
tx inv
on
off
humidity
fan
sleep
timer
```

`main` and `panel` take `2400`, `4800`, or `9600`, plus `inv` or `norm`. That sets the receive polarity, and the opposite transmitter follows it. `tx inv` and `tx norm` change only the output toward the main board. On, Off, Humidity, Fan, Sleep, and Timer replay one saved pulse train. Default is panel receive normal, main receive normal, main transmit normal, pass on. Both wires idle high.
