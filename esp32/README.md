# PetKit BLE -> Telegram Gateway (ESP32-C3 Firmware)

Firmware for **ESP32-C3 SuperMini** running 24/7. Replaces the PC prototype (`petkit_pc.py`) while keeping `core/petkit_core.c` completely unchanged.

## Features
- **ESP-IDF v5.5 + NimBLE Central**: Low memory footprint (~35 KB RAM for BLE).
- **Sequential Memory Architecture**: BLE is stopped before Wi-Fi/TLS starts, avoiding heap contention on ESP32-C3's ~400 KB RAM.
- **Power Safe (8.5 dBm)**: Wi-Fi/BLE TX power capped to 8.5 dBm to prevent brownout resets on the SuperMini's compact regulator.
- **USB-Serial/JTAG Console**: Configured for native USB console logging on `COM3`.
- **NVS Persistence**: Saves alarm states and day counts across power cuts.

## Building & Flashing

1. **Generate Secrets Header:**
   ```bash
   python tools/gen_secrets.py --force
   ```

2. **Set Target:**
   ```bash
   idf.py set-target esp32c3
   ```

3. **Build:**
   ```bash
   idf.py build
   ```

4. **Flash & Monitor:**
   ```bash
   idf.py -p COM3 flash monitor
   ```

## Measured on the board (22 Aug 2026)

| | |
|---|---|
| Cycle | 13.1 s of every 300 s |
| Free heap | 128 KB, minimum ever 63.8 KB |
| Main task stack headroom | 13.0 KB of 16 KB |
| ATT MTU | 158 |
| Link | RSSI -49 to -57 |

## Traps that already cost a day

Four things here are not obvious, and three of them fail *silently*. If a future
change breaks polling, start with these before reading any other code.

**1. The ATT MTU must be negotiated by us.** NimBLE only registers a handler for
`BLE_ATT_OP_MTU_REQ` under `MYNEWT_VAL(BLE_GATTS)`, which follows
`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` - and this is a central-only build, so that
config is off. The fountain's own MTU request is therefore *dropped*
(`ATT handler not found; op=0x02`) and the link stays at the 23-byte default. A
notification then carries 20 bytes while the CMD 230 state frame needs 42 of
payload alone, so `pk_frame_parse` never sees its terminating `0xFB` and every
poll dies as "could not decode state". `pk_ble.c` calls
`ble_gattc_exchange_mtu()` right after connecting to fix this. Do not remove it.

**2. `sdkconfig.defaults` is intent; `sdkconfig` is truth.** Defaults are only
consulted when `sdkconfig` does not already exist, so editing the defaults file
after the first build changes nothing. That is how the main task stack stayed at
3584 B while the defaults file claimed 12288. Diff the two after any config edit.

**3. Size the main task stack against the once-a-day path, not the hourly one.**
`emit_report()` puts a 1600-byte buffer on the *main task* stack and only runs at
the day boundary. With the old 3584 B stack the board had 316 B of headroom and
would have looked perfectly healthy right up until it crashed on its first daily
report. `main.c` now logs `STACK LOW` if headroom ever drops under 2 KB.

**4. `CONFIG_NEWLIB_NANO_FORMAT=y` has no 64-bit conversions.** `%lld` prints the
literal text `ld`. The frozen core is safe (it uses only `%u %d %s %%`), but
platform code must avoid `long long` in format strings. Format warnings are
suppressed *only* for `core/petkit_core.c`, deliberately - see `main/CMakeLists.txt`.

## Startup contract

The gateway refuses to run on a clock it cannot trust. On boot it always attempts
an SNTP sync; if that fails but the RTC survived a reboot and is still plausible,
it runs on the RTC and corrects itself at the next 12-hourly re-sync. If the clock
is unusable it retries every 60 s and does **not** poll, because every core
decision - which civil day a drink belongs to, when the report is due, how long
the cat has gone without water - is a function of that number.

A wedged poll is handled by the task watchdog (180 s, panic-and-reboot); the
5-minute idle is slept in 10-second slices so the watchdog stays fed while
still catching a real hang.

## Do not send these commands

`67` and `69` (stream acks) mark history records synced and the PetKit phone app
can then never display them again - the whole read-without-acking design depends
on never sending them. `73` permanently writes deviceId+secret and can lock the
official app out of the fountain; only a physical factory reset undoes it.
`220 221 222 225 226` change device state and are out of scope. The firmware
sends exactly five commands: `213 86 210 212 80`.
