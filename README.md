# CrossOrbit

> **CrossOrbit is a personal fork of [CrossInk](https://github.com/uxjulia/CrossInk)** (itself a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)) - open-source e-reader firmware for Xteink X3/X4 and Seeed Studio Sticky devices.

CrossOrbit tracks CrossInk closely and adds one thing on top: **BookOrbit sync** - syncing your reading data with a self-hosted [BookOrbit](https://github.com/bookorbit/bookorbit) library server. Everything else - reader fonts, themes, reading stats, dictionary, nearby sync, the web server, and so on - comes straight from CrossInk and is documented in the [upstream CrossInk README](https://github.com/uxjulia/CrossInk#readme).

### Supported devices

- Xteink X3
- Xteink X4
- Seeed Studio Sticky

---

## What CrossOrbit adds over CrossInk

### BookOrbit sync

A dedicated **BookOrbit Sync** feature that syncs your reading to a self-hosted BookOrbit instance (a KOReader-plugin-compatible library server). It lives in its own menu (Settings → System → BookOrbit), also appears in the reader menu, and can be assigned to a button shortcut. You enter your BookOrbit URL and login once; every endpoint is derived from that single URL. Each part can be enabled or disabled independently:

- **Reading progress** - an interactive apply/upload picker, mirroring the KOReader Sync flow.
- **Reading sessions** - each completed session is buffered on the SD card and uploaded on the next sync, so BookOrbit records reading time and progress per book. Devices with a real-time clock date sessions from the RTC; devices without one date them at sync time.
- **Highlights** - clippings are matched to their exact position in the book (whole-chapter, typography-tolerant text matching) and uploaded as BookOrbit highlights.
- **Bookmarks** - two-way sync for the open book: bookmarks you add are pushed, bookmarks added in BookOrbit are pulled down and placed in the book, and removals propagate both directions. Bookmarks for other books are pushed one-way.

BookOrbit uses the KOReader plugin's endpoints and auth, so it works alongside - not instead of - CrossInk's own KOReader Sync. Only what you have pending is sent, and the sync screen reports each step (progress, sessions, highlights, bookmarks).

### Over-the-air updates from this fork

Release builds point the device's **Check for Updates** at this repository's GitHub releases, so you can update over Wi-Fi instead of re-flashing over USB. See [Releases and OTA](#releases-and-ota).

### On-device rebrand

Boot/sleep branding and version strings read **CrossOrbit**, so it is clear which firmware is flashed.

---

## Releases and OTA

Releases are built by the [`CrossOrbit Release`](.github/workflows/crossorbit-release.yml) GitHub Actions workflow (hosted runners, no extra secrets): push a `v*` tag (for example `v1.5.0.1`) or run it manually with a version. It builds the `default` (X3/X4) and `sticky` firmware with OTA pointed at this repo, then publishes a GitHub Release with the flashable `firmware-*.bin` files.

- **OTA (recommended):** on the device, Settings → System → Check for Updates pulls the latest release from this repo over Wi-Fi and verifies the firmware against GitHub's asset checksum before flashing.
- **Manual flash:** download a `firmware-*.bin` from the [releases page](https://github.com/jordandrako/CrossOrbit/releases) and flash it with `esptool` or the CrossInk web installer. See CrossInk's [Installation](./docs/installation.md) guide.

Use fork version tags that sort above upstream (e.g. `v1.5.0.1`) so the device recognizes a release as newer.

The signed catalog / web-flasher integration that upstream uses is intentionally not part of this fork - that pipeline is tied to CrossInk's own signing key and hosted flasher. OTA and direct `.bin` flashing cover the fork's needs.

---

## Staying current with CrossInk

CrossOrbit is a small set of commits rebased on top of upstream CrossInk. The BookOrbit code is deliberately sequestered into its own `lib/BookOrbit/` library and `*BookOrbit*` files, and reuses (rather than modifies) upstream helpers where possible, so pulling in new CrossInk releases rarely conflicts. Where a shared fix touches code BookOrbit duplicated (for example upstream issue #480, the NTP core-lock crash), the fix is mirrored into the BookOrbit path.

---

## Development

CrossOrbit uses PlatformIO. The hardware environments are `default` (X3/X4) and `sticky`:

```sh
pio run -e default --target upload   # Xteink X3 / X4
pio run -e sticky                    # Seeed Studio Sticky
```

To point your own local builds' OTA at this fork, add to `platformio.local.ini` (git-ignored):

```ini
[env]
build_flags =
  -DCROSSINK_OTA_RELEASE_URL=\"https://api.github.com/repos/jordandrako/CrossOrbit/releases/latest\"
```

See CrossInk's [Getting Started](./docs/development/getting-started.md) for prerequisites and validation commands, and the broader docs under [`docs/`](./docs/).

---

For everything CrossOrbit inherits from CrossInk, refer to the upstream [CrossInk documentation](https://github.com/uxjulia/CrossInk#readme) and [releases](https://github.com/uxjulia/CrossInk/releases).
