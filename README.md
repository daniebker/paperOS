# paperOS
A firmware for the PaperDIY by M5Stack.

paperOS is a small app launcher for the PaperDIY's 4.7" e-paper display. Planned apps include an e-reader, a to-do list and a calendar view. Right now it ships with a sample **Hello World** app. It greets you in five languages (press A / B to switch) and introduces the owner named in `config.toml`.

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
test/                     host unit tests for the core and apps
```

The core and apps never include M5Stack headers. That keeps them portable and lets them be unit tested on a PC.

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e paperdiy
```

```bash
pio run -e paperdiy -t upload
```

```bash
pio test -e native
```

The firmware uses [M5Unified](https://github.com/m5stack/M5Unified) 0.2.29 and [M5GFX](https://github.com/m5stack/M5GFX) 0.2.36. These are the versions that added PaperDIY support: board auto-detection, e-paper power through the M5PM1, and buttons A (GPIO4) and B (GPIO3).

## Adding an app

1. Create `apps/<your-app>/` with a `library.json` and a `src/` folder. Copy `apps/hello-world-app` as a starting point.
2. Subclass `paperos::App`:
   - Implement `name()` and `draw()`.
   - Override `onInput()` to react to `Previous` / `Next` / `Select`. Return `true` when the screen should be redrawn.
   - Leave `Back` unhandled (return `false`) to let the shell exit the app. Handle it yourself only to go back within the app, for example from a book to the library.
3. To read settings, take a `const paperos::Config&` in the constructor and call `config.getString("section", "key", "default")`. `HelloWorldApp` shows how.
4. In `src/main.cpp`, include the app header, create a static instance and call `shell.addApp(&yourApp)` in `setup()`.

PlatformIO finds everything under `apps/` automatically (`lib_extra_dirs = apps`).
