# Powerbeats HR Bridge

A tiny ESP32-C6 board that lets an iPhone or Mac read the heart rate from
**Powerbeats Pro 2** over Bluetooth, the same way it would read a chest strap.

```
Powerbeats Pro 2  ──BLE──▶  ESP32-C6 bridge  ──BLE──▶  iPhone / Mac app
 "Powerbeats HR"             relays the reading         sees "pb-hr-esp32-bridge"
```

## Why

With heart-rate sharing on, the Powerbeats Pro 2 advertise a standard Bluetooth
heart-rate sensor called **"Powerbeats HR"**. Apple devices can't use it. On an
iPhone or Mac, "Powerbeats HR" never shows up as a Bluetooth heart-rate sensor,
even when the earbuds aren't paired. Apple only lets you get the earbuds' heart
rate through the Health app, which adds latency.

The bridge fixes that. It connects to "Powerbeats HR", reads the heart rate as
it arrives, and re-broadcasts it as an ordinary Bluetooth heart-rate sensor
named **`pb-hr-esp32-bridge`**. iPhone and Mac apps can connect to that directly
and get live readings about once a second.

## What you need

* A Seeed Studio **XIAO ESP32-C6**. Other ESP32 boards with BLE should work
  with small changes.
* **Powerbeats Pro 2** with heart-rate sharing turned on.
* Arduino IDE, with the **esp32** board package (3.x) and the
  **NimBLE-Arduino** library (2.x) by h2zero.

## Install

1. In Arduino IDE, go to **Tools → Manage Libraries…** and install
   **NimBLE-Arduino** by h2zero.
2. Select the board **XIAO_ESP32C6**.
3. Open `main/main.ino` and click **Upload**.

Or from a terminal, using the CLI bundled with Arduino IDE:

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" lib install "NimBLE-Arduino"
"$CLI" compile --fqbn esp32:esp32:XIAO_ESP32C6 main
"$CLI" upload  --fqbn esp32:esp32:XIAO_ESP32C6 -p /dev/cu.usbmodem83101 main
```

Close the IDE's Serial Monitor before uploading, because it holds the port.

## Use

1. Turn on heart-rate sharing on the Powerbeats Pro 2.
2. Power the bridge from any USB port or charger.
3. Wait for the LED to show **blink-blink, pause**. That means it is bridging.
4. In the app on your iPhone or Mac, add a Bluetooth heart-rate sensor and pick
   **pb-hr-esp32-bridge**. Do this inside the app that will use the heart rate,
   not in system Bluetooth settings.

After a dropout, the bridge finds the earbuds and reconnects on its own.

## Status LED

The onboard yellow LED shows what the bridge is doing.

| LED | Meaning |
|---|---|
| Fast, nonstop blinking | Searching for the earbuds, or waiting for their first reading |
| Blink-blink, pause | Bridging: heart rate is coming in and being relayed |
| Solid on | Something is wrong. Check the serial log |

Errors that turn the LED solid:

* Connecting to the earbuds failed. The LED stays on for 15 s, or until the next
  successful connection. Searching continues meanwhile.
* The earbuds are connected but sent no heart rate for 20 s. After 30 s of
  silence the bridge drops them and starts searching again.
* The bridge couldn't advertise itself for more than 5 s.
* Bluetooth failed to start. Reboot to recover.

The LED doesn't show whether an iPhone or Mac is connected. The serial log does.

## Serial log

Open a serial monitor at **115200 baud** to see what is happening:

```
Ready. Advertising as pb-hr-esp32-bridge, looking for "Powerbeats*".
[led] searching
Found "Powerbeats HR" (rssi -62)
Earbuds connected, waiting for first reading...
Heart rate flowing: 59 bpm
[led] bridging
>>> RECEIVER CONNECTED (…)
```

Send one of these keys to control the bridge:

| Key | Action |
|---|---|
| `i` | Print status |
| `v` | Verbose on/off: every reading and every named Bluetooth device nearby |
| `s` | Search for the earbuds again |
| `d` | Disconnect from the earbuds |
| `a` | Restart advertising |
| `p` | Pause/resume searching |
| `r` | Reboot |

## Settings

These constants are at the top of `main/main.ino`.

| Setting | Default | What it does |
|---|---|---|
| `BRIDGE_NAME` | `"pb-hr-esp32-bridge"` | Name the bridge advertises (22 characters max) |
| `SOURCE_NAME_PREFIX` | `"Powerbeats"` | Connects to the first device whose name starts with this |
| `SOURCE_ALLOW_PAIRING` | `true` | Pair with the earbuds if they demand it. Pairing hasn't been needed so far |
| `NOTIFY_INTERVAL_MS` | `1000` | How often the reading is sent to the iPhone/Mac |
| `HR_STALE_MS` | `20000` | A reading older than this is marked "no contact" |
| `ZOMBIE_MS` | `30000` | Drop the earbuds after this much silence |
| `FIRST_HR_GRACE_MS` | `60000` | How long to wait for the first reading after connecting |
| `CONNECT_ERROR_HOLD_MS` | `15000` | How long a failed connection keeps the LED solid |
| `ADV_DOWN_ERROR_MS` | `5000` | How long advertising can be down before it counts as an error |
| `USE_EXTERNAL_ANTENNA` | `false` | Set `true` if you use a u.FL antenna |

## Troubleshooting

* **LED keeps fast-blinking.** The earbuds aren't advertising. Check that
  heart-rate sharing is on and the earbuds are out of the case.
  Make sure no other phone or app is connected to "Powerbeats HR" directly. A
  heart-rate sensor usually serves only one connection at a time.
* **Found but no heart rate.** One review reports the earbuds pausing heart-rate
  sharing while music is playing. Press `v` to see whether any readings arrive.
* **The iPhone or Mac can't find the bridge.** Press `i` and check that the
  status shows `adv=1`. Then scan with **nRF Connect** for `pb-hr-esp32-bridge`
  and subscribe to *Heart Rate Measurement* to confirm readings come through.
* **The earbuds advertise under a different name.** Press `v` to list nearby
  device names, then change `SOURCE_NAME_PREFIX`.

## How it works

* The bridge connects to the earbuds as a client and subscribes to their
  standard Heart Rate Measurement characteristic (`0x2A37`).
* At the same time it runs its own Heart Rate Service (`0x180D`) and sends the
  latest reading once a second to whatever connects.
* It finds the earbuds by name. Their Bluetooth address changes regularly, and
  matching on the heart-rate service alone could pick up someone else's strap.
* It never sends 0 bpm. If readings stop, it keeps sending the last value with
  the sensor's "contact" flag cleared, which tells the app the number is stale.
* Connecting to the bridge needs no pairing, just like a regular chest strap.

## Credits

Based on [HuskerMinion/hr-bridge](https://github.com/HuskerMinion/hr-bridge)
(MIT), which does the same job for a Pixel Watch and a treadmill. Its README
has more design notes.

## License

[MIT](LICENSE). The original hr-bridge copyright notice is kept alongside this
project's.
