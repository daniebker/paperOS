# paperOS
A firmware for the PaperDIY by M5Stack.

paperOS is a small app launcher for the PaperDIY's 4.7" e-paper display, shown in portrait (540x960) by default. It ships with an **E-Reader** for EPUB books, a **Calendar** that shows your upcoming events from iCal links, **Tasks** from [Mindwtr](https://mindwtr.app), an **RSS** reader, **Settings**, and a sample **Hello World** app, which greets you in five languages (press A / B to switch) and introduces the owner named in `config.toml`.

## Controls

The two side buttons handle all navigation:

| Input        | Home screen          | Inside an app                      |
|--------------|----------------------|------------------------------------|
| Press **A**  | Move selection up    | Passed to the app ("previous")     |
| Press **B**  | Move selection down  | Passed to the app ("next")         |
| Hold **A**   | Open selected app    | Passed to the app ("select")       |
| Hold **B**   | –                    | Exit the app, back to home screen  |

A hold counts as a long press once it reaches 600 ms. It fires while the button is still down.

Double press the **power button** to switch off, leaving a wallpaper on the screen, and press it once to switch back on (see [Sleep](#sleep)).

## E-Reader

Copy `.epub` files into a `books` folder on the SD card, then open **E-Reader** from the home screen. Books are listed by title with the pages you have left.

| Input        | Library              | Reading a book       | Book menu            |
|--------------|----------------------|----------------------|----------------------|
| Press **A**  | Move selection up    | Previous page        | Move selection up    |
| Press **B**  | Move selection down  | Next page            | Move selection down  |
| Hold **A**   | Open selected book   | –                    | Choose selected item |
| Hold **B**   | Back to home screen  | Open the book menu   | Back to the book     |

The home screen, library, book menu and the bottom of each page show the battery level. It's estimated from the battery voltage, so treat it as approximate; it reads high while charging. It updates when the screen redraws.

The first time you open a book, paperOS indexes it: it extracts the text and lays it out into pages for this screen. A progress bar shows how far along it is, and holding B cancels. The result is cached on the SD card, so the book opens straight away after that.

Your place is saved on every page turn, so you can switch off at any time, and it's kept on the SD card, so it survives firmware updates. The bottom of each page shows your position and the pages remaining, for example `Page 41 of 635` and `594 pages left`.

The book menu has **Font size**, **Line height** and **Back to library**. Choosing font size or line height opens a preview of your current page:

| Input        | Preview                         |
|--------------|---------------------------------|
| Press **A**  | Increase                        |
| Press **B**  | Decrease                        |
| Hold **A**   | Keep this setting               |
| Hold **B**   | Cancel, changing nothing        |

There are three font sizes (Small, Medium, Large) and four line heights (100% to 150%). Your choice applies to every book and is remembered across restarts. Changing either keeps you at the same place in the text, though the page numbers change. A book is indexed once for each combination you read it at, so going back to one you've already used is instant. In the library, books not yet indexed at the current settings show "Not indexed yet", and keep your place when you open them.

Good to know:

- The cache lives in `/books/.cache`, along with your saved places, font size and line height. It's safe to delete, but books will need indexing again and restart from page 1, and the text settings go back to their defaults.
- Replacing a book with a different file starts it again from page 1.
- Text only. Images are skipped, so books made of scanned pages show "This book has no readable text".
- The display fonts are ASCII-only, so curly quotes and dashes become plain ones and accents are dropped (`café` shows as `cafe`). Scripts with no ASCII equivalent, such as Cyrillic, Greek or CJK, show as `?`.
- DRM-protected books can't be read.

## Calendar

The Calendar shows the coming two weeks of events from one or more iCal (`.ics`) links, such as the secret address of a Google, Outlook, iCloud or Nextcloud calendar. It downloads them over Wi-Fi, so add your network, time zone and calendars to `config.toml`:

```toml
[time]
  timezone = "Europe/Berlin"

[wifi]
  ssid = "MyNetwork"
  password = "secret"

[[calendar.feed]]
  name = "Personal"
  url = "https://calendar.google.com/calendar/ical/.../basic.ics"

[[calendar.feed]]
  name = "Work"
  url = "webcal://outlook.office365.com/owa/calendar/.../calendar.ics"
```

Add a `[[calendar.feed]]` block for each calendar. Events are listed by day with their time and location; with more than one calendar, each event also shows which calendar it's from.

| Input        | Calendar                     |
|--------------|------------------------------|
| Press **A**  | Previous page                |
| Press **B**  | Next page                    |
| Hold **A**   | Download the calendars now   |
| Hold **B**   | Back to home screen          |

When you open the Calendar it shows the agenda saved on the SD card straight away, even if it's out of date. If that copy is older than `refresh_minutes`, it downloads the calendars again in the background and redraws with the new agenda when they arrive, and it keeps doing so while it stays open. The footer shows when the calendars were last updated, or what went wrong. Wi-Fi is switched off again after each download. While it's on, three bars next to the battery level in the header show it: outlined while joining the network, filled once connected.

The device has no clock of its own that survives a restart unless the board has a battery-backed RTC, so the first download also sets the time from the internet. If the date isn't known yet, the Calendar says so and gets it as soon as it can connect.

Good to know:

- The calendars and the agenda built from them are saved in `/.cache/calendar` on the SD card. It's safe to delete; they're downloaded again. Older versions saved the calendars in `/calendar`, which you can delete.
- Recurring events are supported, including exceptions and single occurrences that were moved or cancelled. Rules that repeat more often than daily show only their first occurrence.
- Times are converted to your `timezone` using the time zones in the calendar file. Times in a zone the file doesn't describe are taken to be in your own time zone.
- https certificates are checked, so a calendar behind a proxy that intercepts TLS won't download.
- Titles and locations are ASCII-only, like the E-Reader: `café` shows as `cafe`.

## Tasks

**Tasks** shows your [Mindwtr](https://mindwtr.app) tasks from the `data.json` that Mindwtr's WebDAV sync keeps in your sync folder. Set up [Sync](#sync) with the same folder, username and password as Mindwtr. The list has three parts:

- **Today's Focus**: tasks you've focused for today, in Mindwtr's focus order.
- **Next Actions**: next actions that have started, or have no start date. Ones with a due date come first, soonest first. Overdue ones say so.
- **Upcoming**: inbox, next and waiting tasks that start or are due in the coming `days` (7 by default), soonest first.

Each task shows its due or start date, project and contexts underneath. Done, archived, deleted, someday and reference tasks aren't shown.

| Input        | List                   | Task menu            | Details             |
|--------------|------------------------|----------------------|---------------------|
| Press **A**  | Previous task          | Move selection up    | Previous page       |
| Press **B**  | Next task              | Move selection down  | Next page           |
| Hold **A**   | Open the task menu     | Choose selected item | –                   |
| Hold **B**   | Back to home screen    | Back to the list     | Back to the menu    |

The task menu has **Mark as done** and **View details**. The details screen shows everything about the task: status, project, dates, repeat, priority, time estimate, contexts, tags, location, checklist and notes.

**Mark as done** downloads the latest `data.json`, changes only that task, and uploads it. Mindwtr picks the change up on its next sync. If Mindwtr changed the file at the same moment, paperOS notices, downloads it again and retries, so nothing made elsewhere is lost. The footer says when it's done, or why it couldn't save.

When you open Tasks it shows the copy saved on the SD card straight away, then downloads a new one. It downloads again every `refresh_minutes` while open. With nothing on the list, hold A to download again.

Good to know:

- Repeating tasks can't be marked done on paperOS. Mindwtr creates the next one when you complete it, so do it there.
- If you edit a task in Mindwtr after paperOS last downloaded it and before Mindwtr syncs, Mindwtr's version wins and the task isn't marked done.
- Encrypted sync isn't supported: paperOS needs the plain `data.json`.
- The saved copy is in `/.cache/tasks` on the SD card. It's safe to delete.
- Sequential projects aren't taken into account, so all their next actions are listed.

## RSS

The RSS reader shows articles from RSS and Atom feeds, newest first, with every feed in one list. It downloads them over Wi-Fi, so add your network and feeds to `config.toml`:

```toml
[[rss.feed]]
  url = "https://example.com/feed.xml"

[[rss.feed]]
  name = "Blog"
  url = "https://blog.example.com/atom.xml"
```

Add a `[[rss.feed]]` block for each feed. `name` is optional; without it, the feed's own title is used.

| Input        | List                       | Article             |
|--------------|----------------------------|---------------------|
| Press **A**  | Previous article           | Previous page       |
| Press **B**  | Next article               | Next page           |
| Hold **A**   | Read the selected article  | –                   |
| Hold **B**   | Back to home screen        | Back to the list    |

Each article shows its feed and when it was published. A square marks unread ones, and the header counts them. Opening an article marks it read. The article shows its title, feed and date, then the text, laid out in pages.

Like the Calendar, it shows the list saved on the SD card straight away when you open it, even if it's out of date. If that copy is older than `refresh_minutes`, it downloads the feeds again in the background and redraws with the new articles. It keeps doing so while it stays open. The footer shows when the feeds were last updated, or what went wrong. With nothing to read, hold A to download now.

Good to know:

- The feeds, the list built from them, the article text and which articles you've read are saved in `/.cache/rss` on the SD card. It's safe to delete; the feeds are downloaded again, but every article shows as unread.
- Each feed keeps its newest `articles` (20 by default). The text of each article is cut at about 16,000 characters.
- Articles show the text the feed includes. Many feeds carry only a summary; the full article stays on the website.
- Text only: images, videos and embedded content are skipped, and text is ASCII-only, like the E-Reader.
- If a download fails, or a url gives a web page instead of a feed, the last copy of that feed is kept.

## Sync

paperOS can sign in to a WebDAV server, such as Nextcloud. CalDAV servers are WebDAV underneath, so they work too. [Tasks](#tasks) reads Mindwtr's file from there. **Settings > Sync** checks that the server accepts your username and password. Add the server and the folder that holds your files to `config.toml`, along with your `[wifi]` network:

```toml
[sync]
  url = "https://cloud.example.com/remote.php/dav/files/dan/mindwtr/"
  username = "dan"
  password = "app-password"
```

Open **Settings** from the home screen, then **Sync**. It joins Wi-Fi, signs in and shows **Signed in**, or what went wrong: wrong username or password, folder not found, or an address that isn't a WebDAV server. Wi-Fi is switched off again afterwards. Hold A to check again. Hold B goes back to Settings, and again to the home screen.

Good to know:

- The username and password are sent with HTTP basic authentication, so use an `https://` address. With two-factor sign-in, as on many Nextcloud accounts, create an app password for paperOS.
- `url` is the folder, not a file. Redirects aren't followed, so use the address the server ends up at.
- The password is kept in plain text on the SD card.

## Sleep

After `sleep_minutes` without a button press (10 by default), or when you **double press the power button**, paperOS switches itself off to save the battery. The e-paper keeps its image without power, so it leaves a picture on the screen: a random one from the `wallpapers` folder on the SD card each time.

Press the **power button** once to wake. The device starts again at the home screen; the E-Reader remembers your page as usual.

Good to know:

- Wallpapers can be `.jpg`, `.png` or `.bmp`. They're scaled to fill the whole 540x960 screen and centred, so the edges of an image with a different shape are cropped: a landscape image loses its sides. Portrait images at 540x960 fit exactly. Greyscale images show best on the 16-level display.
- Progressive JPEGs can't be shown; save them as baseline JPEGs. An image that can't be shown is skipped for another one.
- With no `wallpapers` folder, or no images in it, the screen shows "paperOS" and how to wake it.
- While USB is plugged in the power stays on. paperOS then waits with the wallpaper up, and pressing any button starts it again.
- Set `sleep_minutes = 0` to never sleep.

## Configuration

At boot, paperOS reads `config.toml` from the root of the SD card. There's an example in [`sdcard/config.toml`](sdcard/config.toml):

```toml
[owner]
  name = "Dan"
```

| Setting        | Used by     | Effect                                         |
|----------------|-------------|------------------------------------------------|
| `[owner] name` | Hello World | Shows "My name is <name>" under the greeting, in the selected language |
| `[power] sleep_minutes` | All | Switch off after this many minutes without a button press, showing a wallpaper. `0` never sleeps. Default 10 |
| `[time] timezone` | Calendar | Local time zone: a name such as `Europe/Berlin`, or a POSIX TZ rule such as `CET-1CEST,M3.5.0,M10.5.0/3`. Default `UTC` |
| `[wifi] ssid`, `password` | Calendar, RSS, Tasks, Settings > Sync | Wi-Fi network for downloading calendars, signing in to the sync server and setting the clock |
| `[calendar] days` | Calendar | How many days ahead to show, 1 to 62. Default 14 |
| `[calendar] refresh_minutes` | Calendar | How often to download again while open. `0` downloads only when you hold A. Default 60 |
| `[[calendar.feed]] name`, `url` | Calendar | One block per calendar: its name, and its `https://`, `http://` or `webcal://` iCal address |
| `[sync] url`, `username`, `password` | Tasks, Settings > Sync | WebDAV or CalDAV folder to sync files with, and the account to sign in with |
| `[rss] refresh_minutes` | RSS | How often to download again while open. `0` downloads only when there's nothing to read and you hold A. Default 60 |
| `[rss] articles` | RSS | How many articles to keep from each feed, 1 to 100. Default 20 |
| `[[rss.feed]] name`, `url` | RSS | One block per feed: an optional name, and its `https://` or `http://` address |
| `[tasks] file` | Tasks | Mindwtr's file in the `[sync]` folder. Default `data.json` |
| `[tasks] days` | Tasks | How many days ahead Upcoming looks, 0 to 62. Default 7 |
| `[tasks] refresh_minutes` | Tasks | How often to download again while open. `0` only on opening. Default 15 |
| `[Developer] debug` | All | `"true"` saves log messages to `debug.0.log` at the root of the SD card, moving on to `debug.1.log` and so on as each reaches 256 KB, and logs the power button. The Calendar also shows why Wi-Fi couldn't connect: the network, the driver's reason code, how long it tried and the device's MAC address. Default `"false"` |

The file is only read at startup, so restart the device after changing it. If the card or file is missing, paperOS still starts and apps use their defaults.

The parser supports a subset of TOML: `[section]` headers, `[[table]]` arrays such as `[[calendar.feed]]`, `key = "string"`, `key = 'literal'`, bare values such as numbers and booleans, and `#` comments. Lines it can't parse are skipped and reported on the serial log. The display fonts are ASCII-only for now, so accented letters in the name won't render yet.

## Repository layout

```
platformio.ini            build configuration (PaperDIY firmware + native test env)
src/                      firmware entry point and M5Stack hardware glue
  main.cpp                boots the device, reads buttons, registers apps
  M5Battery.h             paperos::Battery implemented with M5Unified
  M5Display.{h,cpp}       paperos::Display implemented with M5GFX
  SdConfig.{h,cpp}        mounts the SD card and loads /config.toml
  SdFileSystem.{h,cpp}    paperos::FileSystem implemented with the SD library
  EspClock.{h,cpp}        paperos::Clock: system time, set over SNTP, saved to the RTC
  EspNetwork.{h,cpp}      paperos::Network: Wi-Fi and HTTP(S) requests
lib/paperos-core/         hardware-independent OS core
  src/paperos/App.h       base class every app implements
  src/paperos/AppMenu.*   a home-screen item that holds other apps, such as Settings
  src/paperos/AsciiFold.* Unicode to ASCII folding for the display fonts
  src/paperos/Battery.*   battery level interface and indicator
  src/paperos/Clock.h     wall-clock time interface
  src/paperos/Config.*    config.toml parser and lookup
  src/paperos/Display.h   drawing interface apps render to
  src/paperos/FileSystem.h  storage interface used by apps
  src/paperos/Input.*     button -> gesture -> navigation event mapping
  src/paperos/Network.h   Wi-Fi and HTTP interface used by apps
  src/paperos/Shell.*     home screen and app switching
  src/paperos/Sleep.*     idle timer and the sleep screen
  src/paperos/TimeZone.*  time zone names to POSIX TZ rules
  src/paperos/WebDav.*    WebDAV / CalDAV client: sign-in check and file downloads
  src/paperos/XhtmlParser.*  streaming HTML/XHTML to plain text
apps/                     one folder per app
  calendar-app/           agenda from iCal links
  ereader-app/            EPUB reader
  hello-world-app/        sample app
  rss-app/                RSS and Atom reader
  sync-app/               checks the sign-in to the sync server
  tasks-app/              Mindwtr tasks from data.json on the sync server
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
2. Edit `name` to your own. For the Calendar, also set your time zone, Wi-Fi network and calendar links (see [Calendar](#calendar)).
3. Create a `books` folder on the card and copy your `.epub` files into it.
4. Optionally, create a `wallpapers` folder and copy in images to show while the device sleeps (see [Sleep](#sleep)).
5. Insert the card and restart the device.

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
4. In `src/main.cpp`, include the app header, create a static instance and call `shell.addApp(&yourApp)` in `setup()`. For a settings screen, call `settingsMenu.addApp(&yourApp)` instead, to list it under **Settings**.

PlatformIO finds everything under `apps/` automatically (`lib_extra_dirs = apps`).
