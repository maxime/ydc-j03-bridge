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

Telnet prints each frame. From boot, every edge is copied in both directions. `pass off` stops the copy. On and Off replay the saved pulse train on GPIO 15. The other commands are still sent as 4800 8N1.

| LED | Meaning |
|---|---|
| Blue | Running, no Telnet client |
| Green | Telnet connected |
| Yellow | Panel frame relayed to the main board |
| Cyan | Main-board frame relayed to the panel |
| Red | Command typed on Telnet |

## Line encoding

The wire is a pulse-width, Manchester, NEC-style 1-wire protocol. It idles high. Each frame starts with a long low pulse, which a 4800 8N1 decoder prints as `00`. After that, each bit is one pulse, about 200 µs or about 400 µs:

| Waveform | Bit |
|---|---|
| `1011` | 0 |
| `1111` | 1 |

One 4800 bit is about 208 µs, so those two cells are one bit-time of low inside four bit-times, or four bit-times of high. The hex in the Telnet log is only what a 4800 8N1 decoder reads from those same pulses. The bridge still copies the bytes. A `FF` between frames is the idle gap, not a data byte.

## Frames

Frames start with `00 89 A8`. A `FF` between frames is only the gap.

| Name | Frame | Notes |
|---|---|---|
| Status | `00 89 A8 A9 A9 89 29 A1 0E` | Seen about every 6.3 s. Last byte also appears as `0F`. |
| On | `00 E9 AF A9 A9 B9 A9 B9` | Fan toggle. Web On replays the saved 54-edge panel burst. |
| Off | same 54-edge burst as On | That burst toggles the fan, so Off replays it too. |
| Humidity | `00 89 A8 A9 A9 28 2B A8 0E` | One step. The percentage is not in this frame. |
| Fan | `00 89 A8 A9 89 29 8B 89 39` | Same for high and low |
| Sleep | `00 89 A8 A9 A1 29 8B 28 39` | Same for on and off |
| Timer | `00 89 A8 A9 29 89 2B 88 39` | Repeats while the timer changes |

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

`main` and `panel` take `2400`, `4800`, or `9600`, plus `inv` or `norm`. That sets the receive polarity, and the opposite transmitter follows it. `tx inv` and `tx norm` change only the output toward the main board. On and Off replay the saved pulses. Humidity, fan, sleep, and timer pause the copy, send their frame, then resume. Default is panel receive normal, main receive normal, main transmit normal, pass on. Both wires idle high.
