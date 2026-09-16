# Greenhouse ESP8266 Relay Controller

Firmware for an ESP-01S relay carrier that ventilates a greenhouse: it reads a
DHT11 and switches a fan through the on-board relay.

See [docs/DESIGN.md](docs/DESIGN.md) for the hardware, wiring, control model and
toolchain notes.

Current state: milestones 1-7. The firmware reads a DHT11, runs the fan control
state machine, drives the relay, serves a status and settings page from its own
Wi-Fi access point, and persists its configuration in NVS.

Relay polarity and the GPIO it uses are unconfirmed on real hardware - see
"Bring-up" below before connecting a load.

## Build

Build the toolchain image once, **from the repository root**:

```sh
docker build -t greenhouse-esp8266-idf -f docker/esp8266.Dockerfile .
```

Then build the firmware:

```sh
docker/run.sh make defconfig    # first time only, creates sdkconfig
docker/run.sh make -j
```

`docker/run.sh` mounts this directory into the container, runs as your own user
so build output is not root-owned, sources the SDK environment and passes
`/dev/ttyUSB0` through when it exists.

## Flash and monitor

Put the ESP-01S in the flashing harness described in the design document, hold
`FLASH`, tap `RESET`, then:

```sh
docker/run.sh make -j flash
docker/run.sh make monitor      # exit with Ctrl-]
```

Use `ESPPORT=/dev/ttyUSB1 docker/run.sh ...` for a different serial device. If
opening the port is denied, add yourself to the `dialout` group on the host.

Expected output at 115200 baud:

```text
I (xx) greenhouse: greenhouse controller starting, sdk v3.4
I (xx) sensor: polling DHT11 on GPIO2 every 5 s
I (xx) wifi_ap: SSID Greenhouse-A1B2 up, browse to 192.168.4.1
I (xx) web: http server listening on port 80
I (xx) sensor: 22 C, 47 %RH
```

The ESP8266 ROM bootloader prints its first lines at 74880 baud, so those will
look like garbage in a 115200 terminal. That is normal.

## Using it

Join the `Greenhouse-XXXX` Wi-Fi network (default password `greenhouse`, change
it with `docker/run.sh make menuconfig`) and open <http://192.168.4.1>. The page
polls `/api/status` every two seconds.

| Path | Method | Purpose |
| --- | --- | --- |
| `/` | GET | Status and settings page |
| `/api/status` | GET | Reading, sensor health, device stats |
| `/api/config` | GET | Current configuration |
| `/api/config` | POST | Validate and save configuration |
| `/api/history` | GET | Samples for the chart; `?days=1..7` trims the range |
| `/api/relay/test` | POST | Energise the relay briefly; `?seconds=1..5` |
| `/api/reboot` | POST | Restart the controller |

```sh
curl http://192.168.4.1/api/status
```

```json
{"ssid":"Greenhouse-A1B2","valid":true,"stale":false,"temperature_c":22,
 "humidity_pct":47,"age_s":3,"reads_ok":120,"reads_failed":2,
 "last_error":"ok","clients":1,"uptime_s":610,"free_heap":31240}
```

Settings are edited on the page or posted directly. A POST updates only the
fields it names, and is rejected as a whole if any value is out of range — no
partial writes, and out-of-range values are refused rather than silently clamped:

```sh
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' \
  -d '{"temp_threshold_c":28,"start_delay_s":90}'
```

```json
{"saved":true}
```

| Setting | Default | Bounds |
| --- | ---: | ---: |
| `temp_threshold_c` | 30 | 0-50 |
| `humidity_threshold_pct` | 80 | 20-95 |
| `start_delay_s` | 120 | 0-3600 |
| `max_fan_duration_s` | 900 | 30-7200 |
| `grace_period_s` | 300 | 0-7200 |
| `sensor_poll_interval_s` | 5 | 2-60 |
| `fan_mode` | 1 (auto) | 0 off / 1 auto / 2 on |
| `device_name` | `"Fan"` | 1-23 bytes |
| `ap_ssid` | `""` (derive from MAC) | 0-32 bytes |
| `temp_enabled` | true | bool |
| `humidity_enabled` | true | bool |
| `relay_active_low` | false | bool |

Times are entered in minutes on the page; the API uses seconds throughout.

Settings persist across reboots in NVS. A stored config that is corrupt, written
by a different config version, or out of bounds is discarded and the defaults are
rewritten, so a bad value can never reach the controller.

Occasional entries in `reads_failed` are normal for a DHT11. A rising
`reads_failed` with `valid:false` means wiring — check the pull-up first.

### Power supply

Power the board from a real 5 V supply, not from a computer's USB port and not
from the 5 V of a USB-serial adapter. An ESP8266 draws ~300 mA peaks while
transmitting; a serial adapter's rail sags under that and starves the 3.3 V
regulator into dropout.

It does not fail in any way that looks like a power problem. It shows up as
humidity readings alternating between the true value and roughly double it, while
temperature looks fine. Worse supply adds temperature excursions, then read
timeouts, then all-zero frames. "Sensor Signal Integrity" in
[docs/DESIGN.md](docs/DESIGN.md) has the full diagnosis.

Fit the decoupling capacitors as well - 10 uF at the regulator output for loop
stability, and 220-470 uF plus 100 nF at the ESP-01's own VCC/GND pins. Both were
necessary; neither was sufficient.

## Sensor

A DHT22/AM2302 is fitted by default; select `DHT11` in `menuconfig` if that is
what is connected. Both use the same single wire and the same driver - they
differ in the host start pulse (about 1 ms versus 18 ms) and in how the 40
payload bits are encoded.

The DHT22 is the better part for this application, mostly for range rather than
accuracy: the DHT11 is rated 20-90 %RH and 0-50 C, and a greenhouse crosses both
bounds, 90 %RH being exactly the condition that should trigger ventilation.

| | DHT11 | DHT22 |
| --- | --- | --- |
| Humidity | 20-90 %RH, +/-5%, 1% steps | 0-100 %RH, +/-2%, 0.1% steps |
| Temperature | 0-50 C, +/-2 C, 1 C steps | -40 to +80 C, +/-0.5 C, 0.1 C steps |
| Minimum read interval | 1 s | 2 s |

Readings are carried in tenths throughout the firmware, so the DHT22's
resolution reaches the chart and the API; a DHT11 simply reports multiples of
10. Values outside the fitted part's rated range are shown but logged as
unreliable.

## Wiring the sensor

```text
Sensor VCC  -> ESP-01 3V3
Sensor DATA -> ESP-01 GPIO2 (pin 3)
Sensor GND  -> ESP-01 GND
4.7k-10k   -> between DATA and 3V3   (required, the internal pull-up is too weak)
```

GPIO2 is the only free GPIO on the ESP-01 header once the relay takes GPIO0, and
it must be high at boot — which a DHT11 with its pull-up already is. Change the
pin with `CONFIG_GREENHOUSE_SENSOR_GPIO` if your carrier wires the relay to GPIO2
instead. See [docs/DESIGN.md](docs/DESIGN.md) for how to check that.

## Layout

| Path | Purpose |
| --- | --- |
| `Makefile` | Project makefile for the ESP8266_RTOS_SDK make build system. |
| `main/main.c` | Application entry point. |
| `main/dht.c` | Bit-banged DHT11 / DHT22 driver. |
| `main/sensor.c` | Polling task and the shared reading it publishes. |
| `main/config.c` | NVS-backed runtime configuration with validation. |
| `main/history.c` | In-RAM ring buffer of downsampled samples for the chart. |
| `main/relay.c` | Relay output, runtime polarity, guarded test pulse. |
| `main/controller.c` | Fan control state machine. |
| `main/wifi_ap.c` | SoftAP bring-up. |
| `main/web.c` | Status and settings page, `/api/status`, `/api/config`. |
| `main/Kconfig.projbuild` | `menuconfig` options for pin, timings and AP. |
| `sdkconfig.defaults` | Flash size/mode defaults for ESP-01S. |
| `docker/esp8266.Dockerfile` | Pinned ESP8266_RTOS_SDK v3.4 toolchain image. |
| `docker/run.sh` | Runs a command inside that image against this project. |
| `docs/DESIGN.md` | Hardware, wiring, control model, milestones. |

## Bring-up

Two things about the relay carrier are unconfirmed, and both must be settled
before mains wiring goes anywhere near it. Do this with no load connected.

The bring-up controls live behind the collapsed **Bring-up tools** section at the
bottom of the page.

1. **Which GPIO drives the relay.** The firmware assumes GPIO0
   (`CONFIG_GREENHOUSE_RELAY_GPIO`). A minority of carriers use GPIO2, which
   would collide with the DHT11. Check with a continuity meter, unpowered,
   between the relay driver input and socket pins 3 (GPIO2) and 5 (GPIO0).
2. **Active-high or active-low.** Press *Test relay* on the web page and listen
   for the click. If the relay is energised when the fan should be off, press
   *Invert polarity*. This is a runtime setting stored in NVS, so no reflash is
   needed to try both.

The test pulse is capped at 5 seconds in firmware and turns itself off with a
timer armed after the relay switches on, so a lost connection or a crashed
browser cannot leave the relay latched.

Note the relay may click once at power-up: GPIO0 is the boot strap and is held
high by its pull-up until `relay_init()` runs. On an active-high carrier that
means a brief energised window. `relay_init()` is the first thing `app_main()`
does after loading config, to keep it short.

## Fan control

```text
IDLE           conditions normal, fan off
WAITING        over threshold, waiting out start_delay
FAN_ON         fan running
GRACE          conditions normal again, fan still running for grace_period
GRACE_FORCED   max_fan_duration hit; fan off and locked out for grace_period
FAULT          no usable reading for 60 s; fan off
```

## Network name

`ap_ssid` sets the Wi-Fi network name. Left empty, the device derives
`Greenhouse-XXXX` from the last two bytes of its own MAC, so several units stay
distinguishable without configuring each one.

The field lives behind the collapsed **Network settings** section near the bottom
of the page, next to **Bring-up tools**, with its own save button - it is not
part of the everyday settings form.

A change applies **on restart**, not immediately: reconfiguring the radio while
serving the request that asked for it would drop the reply, and the page could
not then tell success from failure. Save, then use *Restart device* under
*Bring-up tools*, then join the new network.

The AP password is still compile-time (`CONFIG_GREENHOUSE_AP_PASSWORD`). It is
deliberately not editable over the air: a mistyped password saved remotely locks
you out of the only interface the device has.

## Naming the load

The relay does not have to switch a fan. `device_name` sets what the page calls
it - "Cooling", "Water pump", "Vent" - and the label follows through the status
line, the chart legend, the trigger settings and every message. It is cosmetic;
no behaviour depends on it.

Quotes, backslashes and control characters are refused so the value needs no
escaping when it goes into JSON, and the page always renders it with
`textContent`, so markup inside a name is inert rather than interpreted. UTF-8 is
allowed.

## Fan mode

A three-way switch at the top of the page: **Off / Auto / On**.

Off and On are operator overrides that ignore the readings entirely. They are
stored in flash and survive a reboot, and the page shows a standing warning while
either is active, because a fan that cannot respond to conditions should never
look like normal operation.

Forced on has **no time limit**. `max_fan_duration_s` bounds automatic control
against a stuck sensor; it is not meant to overrule a deliberate choice, so a
forced-on fan runs until someone sets the mode back to Auto.

Returning to Auto restarts the state machine from idle rather than resuming the
timers that were frozen while forced.

## Fan triggers

The fan runs when **temperature or humidity** is at or above its threshold -
either alone is enough, since a hot dry greenhouse needs air just as a cool damp
one does.

Each input can be taken out of that decision independently
(`temp_enabled`, `humidity_enabled`, or the two switches on the page). A disabled
input is still measured, logged and charted; it simply stops voting. Disabling
both is allowed and means the fan never starts on its own - the page shows a
warning rather than letting that look like normal operation.

The machine requires *sustained* conditions, so a single bad reading can neither
start nor stop the fan. `max_fan_duration_s` bounds a run in case the sensor
sticks or a door is left open; unlike the normal grace period it switches the fan
**off**, because its job is protecting the fan and relay rather than mixing air.

Sensor failure policy: isolated read failures are ignored, but no valid reading
for 60 seconds forces the fan off and shows a fault. The AP and web UI stay up so
the wiring can be diagnosed.

## Configuration

```sh
docker/run.sh make menuconfig    # Greenhouse controller
```

| Option | Default |
| --- | --- |
| `GREENHOUSE_SENSOR_TYPE` | DHT22 |
| `GREENHOUSE_SENSOR_GPIO` | 2 |
| `GREENHOUSE_SENSOR_POLL_INTERVAL_S` | 5 (initial default only; runtime value lives in NVS) |
| `GREENHOUSE_SENSOR_STALE_TIMEOUT_S` | 60 |
| `GREENHOUSE_AP_PASSWORD` | `greenhouse` |
| `GREENHOUSE_AP_CHANNEL` | 1 |
| `GREENHOUSE_AP_MAX_CONN` | 4 |
| `GREENHOUSE_HISTORY_SAMPLES` | 2016 (7 days, ~6 kB) |
| `GREENHOUSE_HISTORY_INTERVAL_S` | 300 |

## History chart

The page charts 1 to 7 days, selectable above the chart, as two stacked panels
sharing one x-axis:
temperature and humidity, with orange bands marking when the fan ran and grey
bands where no valid reading was taken.

Two panels rather than one chart with two y-axes, deliberately. A dual-axis chart
lets the author place two unrelated scales so the lines appear to correlate, and
the reader has no way to tell that apart from a real relationship.

The range is applied on the device, not in the browser. Seven days is about
12 kB of JSON and the page refetches every minute; a one-day view is nearer
1.7 kB, which is worth having on a part with 62 kB of free heap.

Sensor polls are averaged into 5-minute buckets. A bucket with no valid reading
is stored as a gap and drawn as one - never as a zero, which would read as
"cold and dry" and is exactly the misreading a fan controller must not make.

History is RAM-only and is lost on reboot. Persisting it would mean a flash write
every 5 minutes, which wears the part out on a device meant to run for seasons.
2016 samples cost about 6 kB of static RAM.
