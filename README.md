# paperOS
A firmware for the PaperDIY by M5Stack.

paperOS is a small app launcher for the PaperDIY's 4.7" e-paper display, shown in portrait (540x960) by default. Planned apps include an e-reader, a to-do list and a calendar view. Right now it ships with a sample **Hello World** app. It greets you in five languages (press A / B to switch) and introduces the owner named in `config.toml`.

## Controls

The two side buttons handle all navigation:

| Input        | Home screen          | Inside an app                      |
|--------------|----------------------|------------------------------------|
| Press **A**  | Move selection up    | Passed to the app ("previous")     |
| Press **B**  | Move selection down  | Passed to the app ("next")         |
| Hold **A**   | Open selected app    | Passed to the app ("select")       |
| Hold **B**   | –                    | Exit the app, back to home screen  |

A hold counts as a long press once it reaches 600 ms. It fires while the button is still down.

## Configuration

At boot, paperOS reads `config.toml` from the root of the SD card. There's an example in [`sdcard/config.toml`](sdcard/config.toml):

```toml
[owner]
  name = "Dan"
```

| Setting        | Used by     | Effect                                         |
|----------------|-------------|------------------------------------------------|
| `[owner] name` | Hello World | Shows "My name is <name>" under the greeting, in the selected language |

The file is only read at startup, so restart the device after changing it. If the card or file is missing, paperOS still starts and apps use their defaults.

The parser supports a subset of TOML: `[section]` headers, `key = "string"`, `key = 'literal'`, bare values such as numbers and booleans, and `#` comments. Lines it can't parse are skipped and reported on the serial log. The display fonts are ASCII-only for now, so accented letters in the name won't render yet.

## Repository layout

```
platformio.ini            build configuration (PaperDIY firmware + native test env)
src/                      firmware entry point and M5Stack hardware glue
  main.cpp                boots the device, reads buttons, registers apps
  M5Display.{h,cpp}       paperos::Display implemented with M5GFX
  SdConfig.{h,cpp}        mounts the SD card and loads /config.toml
lib/paperos-core/         hardware-independent OS core
  src/paperos/App.h       base class every app implements
  src/paperos/Config.*    config.toml parser and lookup
  src/paperos/Display.h   drawing interface apps render to
  src/paperos/Input.*     button -> gesture -> navigation event mapping
  src/paperos/Shell.*     home screen and app switching
apps/                     one folder per app
  hello-world-app/        sample app
sdcard/                   example files to copy onto the SD card
.github/workflows/        CI: tests + firmware build, and releases on version tags
test/                     host unit tests for the core and apps
```

The core and apps never include M5Stack headers. That keeps them portable and lets them be unit tested on a PC.

## Flashing the firmware

### 1. Install PlatformIO

Install either one:

- **VS Code:** the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
- **Command line:** [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), for example:

```bash
pip install -U platformio
```

On Linux, install the PlatformIO [udev rules](https://docs.platformio.org/en/latest/core/installation/udev-rules.html) so you can open the serial port without root. Depending on your distribution you may also need to add yourself to the `dialout` (or `uucp`) group, then log out and back in.

### 2. Get the code

```bash
git clone https://github.com/daniebker/paperOS.git
```

```bash
cd paperOS
```

### 3. Build

```bash
pio run -e paperdiy
```

The first build takes a few minutes. PlatformIO downloads the ESP32-S3 toolchain, the Arduino framework, M5Unified and M5GFX.

### 4. Connect the PaperDIY and upload

1. Connect the PaperDIY to your computer with a USB-C **data** cable. Some cables only carry power.
2. Upload:

```bash
pio run -e paperdiy -t upload
```

PlatformIO picks the port automatically. If more than one device is connected, name the port. It is usually `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem*` on macOS and `COMx` on Windows:

```bash
pio run -e paperdiy -t upload --upload-port /dev/ttyACM0
```

In VS Code, use **PlatformIO: Upload** from the status bar or the PlatformIO sidebar. Select the `paperdiy` environment first.

When the upload finishes, the device restarts into paperOS and shows the home screen.

### 5. Prepare the SD card (optional)

1. Copy [`sdcard/config.toml`](sdcard/config.toml) to the root of a FAT32-formatted microSD card.
2. Edit `name` to your own.
3. Insert the card and restart the device.

### 6. Watch the logs (optional)

The serial log shows boot messages, including whether `config.toml` was found and parsed:

```bash
pio device monitor -e paperdiy
```

### Troubleshooting

- **No serial port shows up, or the upload can't connect:** try another USB-C cable or port. Then put the PaperDIY into download (boot) mode by long-holding the power button, and run the upload again. If it doesn't start paperOS after the upload finishes, restart it.
- **Permission denied on the port (Linux):** see the udev rules and group step in [Install PlatformIO](#1-install-platformio).
- **Screen stays blank:** check the log for `need OPI-PSRAM enabled`. The e-paper driver needs the octal PSRAM settings in `platformio.ini`, so build with the `paperdiy` environment rather than a generic ESP32-S3 one.

### Flashing a prebuilt binary without PlatformIO

Released versions are on the repository's [Releases page](https://github.com/daniebker/paperOS/releases). Each release has the files below, named `paperOS-<version>-paperdiy-<file>.bin`, plus `SHA256SUMS.txt` for checking the downloads.

For an unreleased build, use GitHub Actions. It builds the firmware on every push to `main` and every pull request (`.github/workflows/build.yml`). Open the run on the repository's **Actions** tab and download the `paperOS-paperdiy-<commit>` artifact.

Both contain:

| File                          | Flash offset |
|-------------------------------|--------------|
| `paperOS-paperdiy-merged.bin` | `0x0` (everything in one image) |
| `bootloader.bin`              | `0x0`        |
| `partitions.bin`              | `0x8000`     |
| `boot_app0.bin`               | `0xe000`     |
| `firmware.bin`                | `0x10000`    |

The simplest option is to flash the merged image (`paperOS-<version>-paperdiy-merged.bin` in a release) with [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/):

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 paperOS-paperdiy-merged.bin
```

Or flash the separate files:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

If you built locally, the images are in `.pio/build/paperdiy/`, and `boot_app0.bin` is in `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/`.

## Publishing a release

Pushing a version tag runs `.github/workflows/release.yml`. It runs the tests, builds the firmware and publishes a GitHub release with the binaries, checksums and flashing notes:

```bash
git tag v0.1.0
```

```bash
git push origin v0.1.0
```

Tags must look like `vMAJOR.MINOR.PATCH`. A tag with a suffix, such as `v0.2.0-beta.1`, is published as a pre-release. If the workflow is re-run for a tag that already has a release, the assets on that release are replaced.

## Running the tests

The core and apps have unit tests that run on your computer, with no device needed:

```bash
pio test -e native
```

## Hardware notes

The firmware uses [M5Unified](https://github.com/m5stack/M5Unified) 0.2.29 and [M5GFX](https://github.com/m5stack/M5GFX) 0.2.36. These are the versions that added PaperDIY support: board auto-detection, e-paper power through the M5PM1, and buttons A (GPIO4) and B (GPIO3).

To change the screen orientation, set `kDisplayRotation` in `src/main.cpp`: 0 is portrait (the default), 2 is portrait upside down, and 1 or 3 is landscape.

## Adding an app

1. Create `apps/<your-app>/` with a `library.json` and a `src/` folder. Copy `apps/hello-world-app` as a starting point.
2. Subclass `paperos::App`:
   - Implement `name()` and `draw()`.
   - Override `onInput()` to react to `Previous` / `Next` / `Select`. Return `true` when the screen should be redrawn.
   - Leave `Back` unhandled (return `false`) to let the shell exit the app. Handle it yourself only to go back within the app, for example from a book to the library.
3. To read settings, take a `const paperos::Config&` in the constructor and call `config.getString("section", "key", "default")`. `HelloWorldApp` shows how.
4. In `src/main.cpp`, include the app header, create a static instance and call `shell.addApp(&yourApp)` in `setup()`.

PlatformIO finds everything under `apps/` automatically (`lib_extra_dirs = apps`).
