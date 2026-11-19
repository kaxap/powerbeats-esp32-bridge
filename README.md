# Powerbeats HR Bridge

Use the heart-rate sensor in your **Powerbeats Pro 2** with any iPhone or Mac app
that supports a Bluetooth heart-rate strap. A small ESP32-C6 board relays it.

```
Powerbeats Pro 2  ──BLE──▶  ESP32-C6 bridge  ──BLE──▶  iPhone / Mac app or web app
 "Powerbeats HR"             relays each reading       connects to "pb-hr-esp32-bridge"
```

* **Live heart rate** about once a second, as a standard Bluetooth heart-rate sensor.
* **About 40 ms** from earbuds to app (under 90 ms at worst).
* **Earbuds' battery level**, through the standard Battery Service.
* **Works in web apps** too: Bluefy on iPhone, Chrome or Edge on Mac.
* **Status LED** that shows searching, bridging or error at a glance.

## Contents

* [Why this exists](#why-this-exists)
* [Getting started](#getting-started)
* [Using it](#using-it)
* [Status LED](#status-led)
* [Latency](#latency)
* [Limitations](#limitations)
* [Troubleshooting](#troubleshooting)
* [Reference](#reference): serial console, settings
* [How it works](#how-it-works)

## Why this exists

With heart-rate sharing on, the Powerbeats Pro 2 act as a standard Bluetooth
heart-rate sensor called **"Powerbeats HR"**. Apple devices can't use it. On an
iPhone or Mac, "Powerbeats HR" never shows up as a Bluetooth sensor, even when the
earbuds aren't paired. Apple only gives you the earbuds' heart rate through
Apple Health, which only works with Health-enabled apps and adds latency.

The bridge connects to "Powerbeats HR" itself and re-broadcasts each reading as
an ordinary heart-rate sensor named **`pb-hr-esp32-bridge`**. iPhone and Mac apps
can connect to that directly, just like a chest strap.

## Getting started

### You need

* A Seeed Studio **XIAO ESP32-C6**. Other ESP32 boards with BLE should work with
  small changes.
* **Powerbeats Pro 2**.
* **Arduino IDE** with the **esp32** board package (3.x) and the
  **NimBLE-Arduino** library (2.x) by h2zero.

Tested with esp32 core 3.3.4 and NimBLE-Arduino 2.5.2.

### Flash the board

1. In Arduino IDE, open **Tools → Manage Libraries…** and install
   **NimBLE-Arduino** by h2zero.
2. Select the board **XIAO_ESP32C6**.
3. Open `main/main.ino` and click **Upload**.

Or from a terminal, using the CLI that ships with Arduino IDE (macOS path shown):

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" lib install "NimBLE-Arduino"
"$CLI" compile --fqbn esp32:esp32:XIAO_ESP32C6 main
"$CLI" board list                       # find the board's port
"$CLI" upload --fqbn esp32:esp32:XIAO_ESP32C6 -p /dev/cu.usbmodemXXXX main
```

Close the IDE's Serial Monitor before uploading, because it holds the port.

### Connect

1. Turn on heart-rate sharing on the Powerbeats Pro 2, and wear both earbuds.
2. Power the bridge from any USB port or charger.
3. Wait for the LED to show **blink-blink, pause**. That means it's bridging.
4. In your app, add a Bluetooth heart-rate sensor and pick
   **pb-hr-esp32-bridge**.

After a dropout, the bridge finds the earbuds and reconnects on its own.

## Using it

### iPhone and Mac apps

Any app that supports a Bluetooth heart-rate strap can use the bridge. Pair it
from inside the app, not in system Bluetooth settings. No pairing code is needed.

### Web apps (Bluefy, Chrome)

Web apps that use Web Bluetooth can read the bridge too.

* **iPhone:** Safari doesn't support Web Bluetooth, but the
  [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)
  browser does. Open a Web Bluetooth heart-rate site in Bluefy, such as a workout
  dashboard, a training game or an HR logger. Pick **pb-hr-esp32-bridge** when
  it asks for a device.
* **Mac:** Chrome and Edge support Web Bluetooth without anything extra.

If you're building a web app, the bridge looks like any heart-rate strap:

```js
const device = await navigator.bluetooth.requestDevice({
  filters: [{ services: ['heart_rate'] }],   // or [{ name: 'pb-hr-esp32-bridge' }]
  optionalServices: ['battery_service'],     // needed to read the battery
});
const server = await device.gatt.connect();

// Heart rate: one notification per reading.
const hr = await (await server.getPrimaryService('heart_rate'))
  .getCharacteristic('heart_rate_measurement');
hr.addEventListener('characteristicvaluechanged', (e) => {
  const v = e.target.value;
  const bpm = v.getUint8(0) & 1 ? v.getUint16(1, true) : v.getUint8(1);
  console.log(bpm);
});
await hr.startNotifications();

// Battery: empty until the bridge has its first reading.
const batt = await (await server.getPrimaryService('battery_service'))
  .getCharacteristic('battery_level');
const level = await batt.readValue();
if (level.byteLength) console.log('battery', level.getUint8(0), '%');
```

### Battery level

The bridge publishes the earbuds' charge through the standard Battery Service
(`0x180F`), so apps that show a sensor's battery will show it.

* It reports the **lower of the two buds**, in **10% steps**.
* It refreshes about **once a minute**.
* It's **empty until the first reading**, usually a few seconds after connecting.
* The case's charge is **not reported**.

## Status LED

The onboard yellow LED:

| LED | Meaning |
|---|---|
| Fast, nonstop blinking | Searching for the earbuds, or waiting for their first reading |
| Blink-blink, pause | Bridging: heart rate is coming in and being relayed |
| Solid on | Something is wrong. The serial log says what |

The LED goes solid when:

* **Connecting failed.** Connecting or subscribing to the earbuds didn't work.
  The LED stays on for 15 s or until the next successful connection, and
  searching carries on meanwhile.
* **No heart rate.** The earbuds are connected but sent nothing for 20 s. After
  30 s of silence the bridge drops them and searches again.
* **Not advertising.** The bridge couldn't advertise itself for more than 5 s.
* **Bluetooth didn't start.** Reboot to recover.

The LED doesn't show whether an app is connected. The serial log does.

## Latency

### Earbuds to app: about 40 ms

| Step | Why | Typical | Worst |
|---|---|---|---|
| Earbuds → bridge | Connection held at 50 ms. A reading waits for the next connection event | ~25 ms | ~50 ms |
| Inside the bridge | Each reading is forwarded as soon as it arrives | 0.5 ms | ~5 ms |
| Bridge → iPhone/Mac | The receiver used a 30 ms connection interval | ~15 ms | ~30 ms |
| **Total** | | **~40 ms** | **~85 ms** |

The connection intervals and the time inside the bridge were measured on the
board: usually 0.5 ms, with a 4.8 ms maximum over 2 minutes. The per-step times
are worked out from those. Press `i` on the serial console to see the numbers for
your own setup.

**Why 50 ms.** Right after connecting, the earbuds ask for a 500 ms connection
interval. Accepting that would add about 225 ms on average (up to 450 ms). The
bridge refuses the request (`KEEP_FAST_SOURCE_INTERVAL`). In testing the earbuds
accepted the refusal and stayed connected. The cost in earbud battery hasn't
been measured.

### What the earbuds send

Measured over 2 minutes at rest:

| | Result |
|---|---|
| Readings | One every **1.0 s** (range 0.65–1.30 s) |
| Changes in bpm | 22 in 2 minutes, **1.1 s to 11 s** apart (median about 4 s) |
| Size of each change | Always exactly **±1 bpm** |

The bpm sometimes changed on back-to-back readings (57 → 58 one second apart), so
the earbuds don't hold each value for a fixed 5 seconds. At rest the number just
doesn't move often.

The steady 1 bpm steps suggest the earbuds smooth the heart rate before sending
it. If so, the reading may lag a sharp spike by a few seconds. That happens
inside the earbuds and affects every path, Apple Health included. It hasn't been
measured during exercise. To check, turn on `v` during a hard interval and watch
how fast the bpm climbs.

### Compared with Apple Health

| | Through the bridge | Through Apple Health |
|---|---|---|
| Latency | ~40 ms typical, under ~90 ms | **Not published.** Apple calls it real-time but gives no figure |
| How readings arrive | Pushed to the app over Bluetooth as each one arrives | The iPhone saves samples to Health, and the app reads them from there |
| Which apps | Any app with Bluetooth heart-rate strap support, plus Web Bluetooth web apps | Only Health-enabled iPhone apps |
| Also wearing an Apple Watch | No effect | Apple says the watch's heart rate takes priority |

Apple Health is built as a health record that apps look things up in, not as a
live sensor feed, so there's no guarantee how quickly a reading reaches the app.
The Health path hasn't been measured here. To compare on your own iPhone, run a
Health-based app next to one connected to `pb-hr-esp32-bridge` and watch the lag.

## Limitations

* **Music from the iPhone.** Apple says the iPhone can't play audio to the
  earbuds while they share heart rate with third-party equipment. The bridge
  connects the same way gym equipment does, so this likely applies.
* **Both earbuds.** Apple says taking one earbud out pauses heart-rate monitoring.
* **One connection at a time.** Don't connect a phone or app to "Powerbeats HR"
  directly while the bridge is running. A heart-rate sensor usually serves only
  one connection.
* **Battery source.** The bridge reads the battery from any Powerbeats Pro 2 and
  takes the strongest signal. If someone nearby has a pair closer to the bridge,
  it could report their battery instead.
* **Not yet measured:** the earbuds' battery use at 50 ms, and how the earbuds'
  smoothing affects heart rate during hard exercise.

## Troubleshooting

* **LED keeps fast-blinking.** The bridge can't find the earbuds. Check that
  heart-rate sharing is on, the earbuds are out of the case, and nothing else is
  connected to "Powerbeats HR".
* **The earbuds are found but there's no heart rate.** Wear both earbuds. Press
  `v` to see whether readings arrive.
* **Your app can't find the bridge.** Press `i` and check the status shows
  `adv=1`. Then scan with **nRF Connect** for `pb-hr-esp32-bridge` and subscribe
  to *Heart Rate Measurement*. If readings arrive there, the bridge is working
  and the problem is on the app's side.
* **The earbuds keep disconnecting or their battery drains fast.** Set
  `KEEP_FAST_SOURCE_INTERVAL` to `false`. That adds about 225 ms of latency on
  average.
* **The earbuds advertise under a different name.** Press `v` to list nearby
  names, then change `SOURCE_NAME_PREFIX`.

## Reference

### Serial console

Connect at **115200 baud**. A normal start looks like this:

```
Ready. Advertising as pb-hr-esp32-bridge, looking for "Powerbeats*".
[led] searching
>>> RECEIVER CONNECTED (49:a6:…, interval 30.00 ms)
Found "Powerbeats HR" (rssi -58)
Earbuds asked for interval 485.00-500.00 ms (latency 0): refused, keeping the fast interval
Earbuds connected (interval 50.00 ms), waiting for first reading...
Earbuds battery 80% (buds 80/80%, case -1%)
Heart rate flowing: 60 bpm
[led] bridging
```

`case -1%` means the case's charge wasn't in the broadcast. That's normal when
the case is closed or out of range.

| Key | Action |
|---|---|
| `i` | Print status and timing (connection intervals, time inside the bridge) |
| `v` | Verbose on/off: every reading, its relay time, and nearby Bluetooth devices |
| `b` | Read the earbuds' battery now |
| `g` | List the services the earbuds offer over their heart-rate connection |
| `s` | Search for the earbuds again |
| `d` | Disconnect from the earbuds |
| `a` | Restart advertising |
| `p` | Pause/resume searching |
| `r` | Reboot |

### Settings

Constants at the top of `main/main.ino`:

| Setting | Default | What it does |
|---|---|---|
| `BRIDGE_NAME` | `"pb-hr-esp32-bridge"` | Name the bridge advertises (22 characters max) |
| `SOURCE_NAME_PREFIX` | `"Powerbeats"` | Connect to the first device whose name starts with this |
| `SOURCE_ALLOW_PAIRING` | `true` | Pair with the earbuds if they require it. Not needed so far |
| `KEEP_FAST_SOURCE_INTERVAL` | `true` | Refuse the earbuds' 500 ms interval and keep 30–50 ms |
| `KEEPALIVE_MS` | `1200` | Re-send the last value if no new reading arrives for this long |
| `HR_STALE_MS` | `20000` | Mark a reading older than this as "no contact" |
| `ZOMBIE_MS` | `30000` | Drop the earbuds after this much silence |
| `FIRST_HR_GRACE_MS` | `60000` | How long to wait for the first reading after connecting |
| `RELAY_BATTERY` | `true` | Publish the earbuds' battery level |
| `SOURCE_APPLE_MODEL` | `0x201D` | Apple model ID to read battery from (Powerbeats Pro 2). `0` = any AirPods/Beats |
| `BATTERY_SCAN_EVERY_MS` | `60000` | How often to refresh the battery level |
| `CONNECT_ERROR_HOLD_MS` | `15000` | How long a failed connection keeps the LED solid |
| `ADV_DOWN_ERROR_MS` | `5000` | How long advertising can be down before it counts as an error |
| `USE_EXTERNAL_ANTENNA` | `false` | Set `true` for a u.FL antenna |

## How it works

* **Two roles at once.** The bridge is a client of the earbuds, subscribed to
  their Heart Rate Measurement (`0x2A37`). It also runs its own Heart Rate
  Service (`0x180D`) for apps.
* **Instant forwarding.** Each reading is forwarded as soon as it arrives. If the
  earbuds go quiet, the last value is repeated about once a second.
* **Never 0 bpm.** When readings stop, the bridge keeps sending the last value
  with the sensor's "contact" flag cleared, which tells the app the number is
  stale.
* **Found by name.** The earbuds' Bluetooth address keeps changing, and matching
  on the heart-rate service alone could grab someone else's strap. Testing turned
  up a WHOOP strap nearby.
* **Battery from Apple broadcasts.** The earbuds' heart-rate connection offers
  only Heart Rate (`0x180D`) and Tx Power (`0x1804`). Like AirPods, though, they
  regularly broadcast Apple status packets that include each bud's charge. The
  bridge listens for those for a few seconds each minute. It recognizes them by
  the Powerbeats Pro 2 model ID (`0x201D`).
* **Open to apps.** Connecting to the bridge needs no pairing, just like a
  regular chest strap.

## Sources

* Apple, [Monitor your heart rate with Powerbeats Pro 2](https://support.apple.com/guide/beats/devbce04ee52/web)
* DC Rainmaker, [Powerbeats Pro 2 in-depth review](https://dcrainmaker.com/2025/02/apple-beats-powerbeats-2-pro-sports-fitness-heart-rate-in-depth-review.html)

## Credits and license

Based on [HuskerMinion/hr-bridge](https://github.com/HuskerMinion/hr-bridge),
which does the same job for a Pixel Watch and a treadmill. Its README has more
design notes.

[MIT](LICENSE). The original hr-bridge copyright notice is kept alongside this
project's.
