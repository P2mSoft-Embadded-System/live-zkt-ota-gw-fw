# ESP32 pull-OTA test (blink + rollback)

A blinking-LED firmware for the ESP32 DevKit that updates itself from this
repository. On every boot the device downloads `versions.txt`, and if a newer
image is listed it installs it and reboots. A bad image is rolled back
automatically.

## Layout

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Firmware: blink, OTA check, self-test, rollback tracking |
| `include/ota_config.h` | Repo URL, LED pin, timeouts |
| `include/ota_root_ca.h` | Pinned root CAs for `raw.githubusercontent.com` |
| `versions.txt` | Release list read by the device |
| `firmware/` | Published `.bin` images |
| `tools/release.sh` | Build + publish a release |
| `tools/monitor.py` | Serial logger / command sender |

## versions.txt

One release per line, `#` starts a comment:

```
<version> <file> <size-bytes> <sha256>
1.0.1 firmware/firmware-v1.0.1.bin 960720 3b1f...
```

The device installs the highest `major.minor.patch` that is greater than the
version it is running. Order of lines does not matter.

## First flash (USB)

```sh
FW_VERSION=1.0.0 pio run -t upload
tools/monitor.py "ssid <wifi name>" "pass <wifi password>" reboot
```

WiFi credentials live in NVS on the device. They are never compiled into the
image, because the images in this repo are public. 2.4 GHz networks only.

## Publishing a release

```sh
tools/release.sh 1.0.1 200 --push    # version, blink half-period in ms
```

This builds the image, copies it to `firmware/`, appends a line to
`versions.txt`, commits and pushes. Devices pick it up on their next restart.
GitHub's CDN caches `versions.txt` for up to 5 minutes.

## How an update is made safe

1. **Download** – HTTPS with pinned root CAs; size and SHA-256 must match
   `versions.txt`, otherwise nothing is switched and the old image keeps running.
2. **First boot of the new image** – the bootloader marks it `PENDING_VERIFY`.
3. **Self-test** – the new image must connect to WiFi and fetch `versions.txt`
   (up to 5 tries, so one dropped connection does not condemn a good image).
   Pass: it marks itself valid. Fail: it rolls back and reboots.
4. **Crash or hang before the self-test passes** – any reset while still
   `PENDING_VERIFY` makes the bootloader boot the previous image. A 60 s
   watchdog covers hangs.
5. **No update loop** – a release that fails to validate twice is blacklisted on
   that device and skipped until a higher version is published. Send `forget`
   over serial to clear the blacklist.

## Testing rollback

```sh
tools/release.sh 1.0.2 100 --bad --push   # crashes on boot, on purpose
```

Expected on the device: install 1.0.2, crash, bootloader rollback, retry once,
rollback again, blacklist 1.0.2, keep running the previous version.

## Serial commands

`ssid <name>`, `pass <password>`, `reboot`, `status`, `forget`
