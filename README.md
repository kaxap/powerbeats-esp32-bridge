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
and get live readings about once a second. It also reports the earbuds'
**battery level** through the standard Battery Service.

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

### Battery level

Apps that show a sensor's battery see the earbuds' charge on
`pb-hr-esp32-bridge`, through the standard Battery Service (`0x180F`).

* **Lower bud:** the bridge reports the lower of the two buds, in 10% steps.
* **Update rate:** the value refreshes about once a minute.
* **Before the first reading:** the battery value stays empty until the bridge
  has a real reading, usually within a few seconds of connecting.
* **Not reported:** the case's charge.

The earbuds' heart-rate connection has no battery service of its own. Like
AirPods, though, the earbuds regularly broadcast Apple status packets that
include each bud's charge. The bridge listens for those for a few seconds every
minute. It recognizes them by the Powerbeats Pro 2 model ID (`0x201D`) and picks
the strongest signal. If someone nearby also has Powerbeats Pro 2 and theirs are
closer to the bridge, it could report their battery instead.

### In the browser: Bluefy and Web Bluetooth apps

The bridge also makes the Powerbeats heart rate usable in **web apps** on an
iPhone. Safari on iOS doesn't support Web Bluetooth, but the
[Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)
browser does. Open any Web Bluetooth heart-rate site in Bluefy, such as a
workout dashboard, a training game or an HR logger. When it asks for a device,
pick **pb-hr-esp32-bridge**.

On a Mac, Chrome and Edge support Web Bluetooth out of the box, so the same web
apps work there too.

If you're building such a web app, the bridge looks like any heart-rate strap:

```js
const device = await navigator.bluetooth.requestDevice({
  filters: [{ services: ['heart_rate'] }],   // or [{ name: 'pb-hr-esp32-bridge' }]
  optionalServices: ['battery_service'],     // needed to read the battery
});
const server = await device.gatt.connect();
const service = await server.getPrimaryService('heart_rate');
const hr = await service.getCharacteristic('heart_rate_measurement');
hr.addEventListener('characteristicvaluechanged', (e) => {
  const v = e.target.value;
  const bpm = v.getUint8(0) & 1 ? v.getUint16(1, true) : v.getUint8(1);
  console.log(bpm);
});
await hr.startNotifications();

const batt = await (await server.getPrimaryService('battery_service'))
  .getCharacteristic('battery_level');
const level = await batt.readValue();       // empty until the bridge has a reading
if (level.byteLength) console.log('battery', level.getUint8(0), '%');
```

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
>>> RECEIVER CONNECTED (49:a6:…, interval 30.00 ms)
Found "Powerbeats HR" (rssi -58)
Earbuds asked for interval 485.00-500.00 ms (latency 0): refused, keeping the fast interval
Earbuds connected (interval 50.00 ms), waiting for first reading...
Earbuds battery 80% (buds 80/80%, case -1%)
Heart rate flowing: 60 bpm
[led] bridging
```

`case -1%` means the case's charge wasn't in the broadcast. That's normal when
the case is closed or far away.

Send one of these keys to control the bridge:

| Key | Action |
|---|---|
| `i` | Print status and timing (connection intervals, delay inside the bridge) |
| `v` | Verbose on/off: every reading, how fast it was relayed, and every named Bluetooth device nearby |
| `s` | Search for the earbuds again |
| `d` | Disconnect from the earbuds |
| `a` | Restart advertising |
| `p` | Pause/resume searching |
| `b` | Read the earbuds' battery now instead of waiting for the next minute |
| `g` | List the services the earbuds offer over their heart-rate connection |
| `r` | Reboot |

## Settings

These constants are at the top of `main/main.ino`.

| Setting | Default | What it does |
|---|---|---|
| `BRIDGE_NAME` | `"pb-hr-esp32-bridge"` | Name the bridge advertises (22 characters max) |
| `SOURCE_NAME_PREFIX` | `"Powerbeats"` | Connects to the first device whose name starts with this |
| `SOURCE_ALLOW_PAIRING` | `true` | Pair with the earbuds if they demand it. Pairing hasn't been needed so far |
| `KEEP_FAST_SOURCE_INTERVAL` | `true` | Refuse the earbuds' request for a slow 500 ms interval and keep 30–50 ms. Lower latency, maybe more earbud battery use |
| `KEEPALIVE_MS` | `1200` | If no new reading arrives for this long, re-send the last one |
| `RELAY_BATTERY` | `true` | Read the earbuds' battery from their Apple broadcasts and publish it |
| `SOURCE_APPLE_MODEL` | `0x201D` | Apple model ID to take battery from (Powerbeats Pro 2). `0` = any AirPods/Beats. With `v` on, every model ID heard is logged |
| `BATTERY_SCAN_EVERY_MS` | `60000` | How often to refresh the battery level |
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
* **Found but no heart rate.** Wear both earbuds. Apple says taking one out
  pauses heart-rate monitoring. Press `v` to see whether any readings arrive.
* **The iPhone or Mac can't find the bridge.** Press `i` and check that the
  status shows `adv=1`. Then scan with **nRF Connect** for `pb-hr-esp32-bridge`
  and subscribe to *Heart Rate Measurement* to confirm readings come through.
* **The earbuds keep disconnecting, or their battery drains quickly.** Set
  `KEEP_FAST_SOURCE_INTERVAL` to `false` so they get the 500 ms interval they
  ask for. That adds about 225 ms of latency on average.
* **The earbuds advertise under a different name.** Press `v` to list nearby
  device names, then change `SOURCE_NAME_PREFIX`.

## How it works

* The bridge connects to the earbuds as a client and subscribes to their
  standard Heart Rate Measurement characteristic (`0x2A37`).
* At the same time it runs its own Heart Rate Service (`0x180D`) and forwards
  each reading to whatever connects as soon as it arrives. If the earbuds go
  quiet, it re-sends the last value about once a second.
* It finds the earbuds by name. Their Bluetooth address changes regularly, and
  matching on the heart-rate service alone could pick up someone else's strap.
  This matters in practice: testing turned up a WHOOP strap advertising heart
  rate nearby.
* Over that connection the earbuds offer only Heart Rate (`0x180D`) and Tx Power
  (`0x1804`). The battery level comes from their separate Apple status
  broadcasts instead (see [Battery level](#battery-level)).
* It never sends 0 bpm. If readings stop, it keeps sending the last value with
  the sensor's "contact" flag cleared, which tells the app the number is stale.
* Connecting to the bridge needs no pairing, just like a regular chest strap.

## Latency

A reading takes **about 40 ms on average, and under 90 ms**, to get from the
earbuds to the iPhone or Mac.

| Step | Why | Typical | Worst |
|---|---|---|---|
| Earbuds → bridge | Bluetooth connection interval held at 50 ms. A reading waits for the next connection event | ~25 ms | ~50 ms |
| Inside the bridge | Each reading is forwarded as soon as it arrives | 0.5 ms | ~5 ms |
| Bridge → iPhone/Mac | The receiver used a 30 ms connection interval | ~15 ms | ~30 ms |
| **Total** | | **~40 ms** | **~85 ms** |

The measured values are the two connection intervals and the time a reading
spends inside the bridge: usually 0.5 ms, with a 4.8 ms maximum over 2 minutes.
The per-step times follow from those. The table doesn't include the time the
earbuds take to compute a heart rate.

**About the 50 ms interval.** Right after connecting, the earbuds ask for a 500 ms
interval. Accepting it would add about 225 ms on average (up to 450 ms), because
a reading can only cross at a connection event. The bridge refuses that request
(`KEEP_FAST_SOURCE_INTERVAL`). In testing, the earbuds accepted the refusal,
didn't ask again, and stayed connected. The faster interval probably costs the
earbuds some battery. That hasn't been measured.

Press `i` on the serial console to see these numbers for your own setup.

### What the earbuds send

Measured over 2 minutes at rest:

| | Result |
|---|---|
| Readings from the earbuds | One every **1.0 s** (range 0.65–1.30 s) |
| How often the bpm changed | 22 times in 2 minutes. Gaps ranged from **1.1 s to 11 s**, with a median of about 4 s |
| Size of each change | Always exactly **±1 bpm** |

The bpm sometimes changed on back-to-back readings (for example 57 → 58 one
second apart), so the earbuds don't wait a fixed 5 seconds between values. At
rest the number simply doesn't move often.

The steady ±1 bpm steps suggest the earbuds smooth the heart rate before sending
it. If that also limits how fast the number can move, the reading will trail a
rapid spike during intervals by a few seconds. That delay happens inside the
earbuds, so it applies to the bridge and Apple Health alike. It hasn't been
measured during exercise yet. To check, run with `v` on during a hard interval
and watch how quickly the bpm climbs.

### Compared with Apple Health

Without the bridge, the only way to get the earbuds' heart rate on an iPhone is
through Apple Health. Apple's guide says the workout app "must be
HealthKit-enabled". The iPhone saves heart-rate samples to Health, and the app
reads them from there.

| | Through the bridge | Through Apple Health |
|---|---|---|
| Latency | ~40 ms typical, under ~90 ms (measured intervals, see above) | **Not published.** Apple calls it real-time but gives no figure, and no independent measurement could be found |
| How readings arrive | Each reading is pushed to the app over Bluetooth as soon as it arrives, about once a second | The app reads samples after the iPhone saves them to Health. Apple doesn't say how often samples are saved or how quickly apps hear about them |
| Which apps | Any iPhone or Mac app that supports a Bluetooth heart-rate strap, plus Web Bluetooth web apps (Bluefy on iPhone, Chrome/Edge on Mac) | Only HealthKit-enabled apps on iPhone |
| If you also wear an Apple Watch | No effect, the app reads the bridge directly | Apple says the watch's heart rate takes priority |

HealthKit is built as a health record that apps query, not as a live sensor feed,
so nothing guarantees how quickly a reading reaches the app. That delay is what
this bridge avoids. The bridge path is the same kind of direct link a chest strap
uses, with a known delay of tens of milliseconds.

The Health path hasn't been measured here. To compare on your own iPhone, run
both paths side by side and watch how far the Health-based app lags behind one
connected to `pb-hr-esp32-bridge`.

**Audio limit.** Apple says that while the earbuds share heart rate with
third-party equipment, the iPhone can't play audio to them. The bridge connects
the same way gym equipment does, so the same limit likely applies.

Sources: Apple, [Monitor your heart rate with Powerbeats Pro 2](https://support.apple.com/guide/beats/devbce04ee52/web);
DC Rainmaker, [Powerbeats Pro 2 in-depth review](https://dcrainmaker.com/2025/02/apple-beats-powerbeats-2-pro-sports-fitness-heart-rate-in-depth-review.html).

## Credits

Based on [HuskerMinion/hr-bridge](https://github.com/HuskerMinion/hr-bridge)
(MIT), which does the same job for a Pixel Watch and a treadmill. Its README
has more design notes.

## License

[MIT](LICENSE). The original hr-bridge copyright notice is kept alongside this
project's.
