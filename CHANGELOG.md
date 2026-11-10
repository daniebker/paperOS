# Changelog

All notable changes to paperOS are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-11-10

The first release of paperOS, an app launcher for the M5Stack PaperDIY's 4.7"
e-paper display.

### Core

- Home screen that lists the apps: press A / B to move, hold A to open, hold B
  to go back. Shown in portrait (540x960) by default.
- `config.toml` read from the root of the SD card at boot, including
  `[[array]]` tables.
- Battery level in the header of every screen, and a Wi-Fi icon while
  downloading: outlined while joining the network, filled once connected.
- Clock set over SNTP, with time zone names or POSIX TZ rules
  (`[time] timezone`).
- HTTPS downloads over Wi-Fi (`[wifi] ssid`, `password`), with Wi-Fi switched
  off between downloads.
- Sleep after `[power] sleep_minutes` without a button press, or on a double
  press of the power button: draws a random image from `/wallpapers` and
  switches off. A single press starts the device again.
- WebDAV / CalDAV client for signing in, downloading and uploading files.
- Shared streaming HTML/XHTML to plain text parser and ASCII folding.
- Debug mode (`[Developer] debug`): log files on the SD card (`/debug.N.log`,
  256 KB each) and an explanation of Wi-Fi failures in the Calendar.

### Apps

- **E-Reader** for EPUB books in `/books`. Books are indexed once into a page
  cache, the last page read is saved on every page turn, and pages remaining
  are shown in the library and while reading. A book menu sets the font size
  (three sizes) and line height in a preview of the current page; both are
  remembered. Reading positions survive font changes and firmware updates.
- **Calendar** for iCal links (`https://`, `http://`, `webcal://`): upcoming
  events grouped by day, with recurring events, exceptions and moved or
  cancelled occurrences, converted to the local time zone. The saved agenda
  in `/.cache/calendar` shows at once, and calendars download again in the
  background every `refresh_minutes`.
- **Tasks** from [Mindwtr](https://mindwtr.app)'s `data.json` on a WebDAV
  server: Today's Focus, Next Actions, and inbox, next and waiting tasks
  coming up within `[tasks] days`. Tasks can be viewed in detail and marked
  as done; the change is uploaded without overwriting edits made elsewhere.
- **RSS** reader for RSS and Atom feeds: one list, newest first, with unread
  articles marked and counted. Articles are laid out in pages. Feeds are saved
  in `/.cache/rss` and download again in the background every
  `refresh_minutes`.
- **Settings**, with **Sync** to check the sign-in to the WebDAV server.
- **Hello World** sample app that greets you in five languages and introduces
  the owner named in `config.toml`.

### Build and release

- PlatformIO project for the PaperDIY (ESP32-S3) with host unit tests
  (`pio test -e native`).
- GitHub Actions workflow that tests and builds the firmware, and a release
  workflow that publishes binaries when a version tag is pushed.
- README with flashing instructions, controls and every `config.toml` setting.

[1.0.0]: https://github.com/daniebker/paperOS/releases/tag/v1.0.0
