# WLC — Wilkinson LAN Client

WLC is a fork of [VLC media player](https://www.videolan.org/vlc/) 3.0.x that
keeps everything VLC does and adds a **Control Center**: a media center, a much
bigger equalizer, a batch converter, a bulk renamer, a LAN media server and
WilkCast, a simple way to send what you are playing to another WLC on your
network. Its symbol is an orange construction barrel.

WLC is an independent project. It is not affiliated with or endorsed by
VideoLAN. "VLC" and "VideoLAN" are trademarks of the VideoLAN Association.

Version 0.2.0 · based on VLC 3.0.25-dev (commit f7ee56b) · GPL-2.0-or-later
(see `COPYING`) · WLC changes © 2026 Josh Wilk.

## What is new compared with VLC

Open **Window ▸ WLC Control Center** (⌥⌘K) on the Mac, or browse to
`http://127.0.0.1:48620/`. It opens by itself the first time WLC runs.

| | |
|---|---|
| **Media Center** | Add your movie, show, music and photo folders. Browse by folder with cover art, search everything, play a folder, queue, sort by recently added. |
| **Power EQ** | New audio engine module (`wlceq`): 10, 15 or 31 graphic bands at ±24 dB (stock VLC: 10 bands, ±20 dB), plus 12 fully parametric bands (peak, low/high shelf, low/high pass, notch, band-pass with frequency, gain and Q), preamp, balance, stereo width, a clip-guard limiter, a live response curve, 17 presets and your own saved presets. Changes are heard while you drag. |
| **Converter** | Batch convert with VLC's own engine while you keep watching: MP4/MKV (H.264 + AAC), WebM, MPEG-TS, MP3, AAC, Ogg Vorbis, Opus, FLAC, WAV; size, bitrate, frame rate, sample rate, channels; pull the audio out of videos; advanced custom transcode options; several files at once. Never overwrites: a taken name becomes "name (2)". |
| **Bulk Renamer** | Rule stack with live preview: replace (plain or regex), remove characters, remove between brackets, strip leading numbers, add text, clean up, change case, number in order, name from tags (`{track:2} - {title}` …), extension case. All-or-nothing, never overwrites, every batch can be undone (history survives restarts). |
| **LAN server** | Any phone, tablet or computer on your network can open WLC's address, enter the 6-digit PIN and get a remote control, your Media Center, the EQ, and playback right in its browser (with seeking). File tools and settings stay on the computer running WLC. |
| **WilkCast** | WLCs on the same network find each other automatically. Cast a file, a whole folder (as a queue) or what is playing, or *Move* playback to another WLC and carry on from the same second; pause, seek, skip and stop it from where you are. |
| **Browser screens** | Turn any smart TV, phone, tablet or old laptop into a cast target with nothing installed: open `http://<wlc-address>:48620/screen` in its browser and enter the PIN. Every WLC on the network can then cast to it, and its remote controls work. Browsers play MP4 (H.264), WebM, MP3, AAC, FLAC and WAV; anything else is reported back and skipped. |
| **Open protocol** | WilkCast is tiny and documented in `doc/wilkcast.md` (public domain). `tools-wlc/wilkcast.py` is a second implementation in one Python file with no dependencies: `discover`, `cast` (serves your files itself), `ctl`, `status`, and `receive` (turns any computer with mpv, ffplay or VLC into a receiver). |

Also changed: own name, icons and bundle identifier
(`io.github.joshwilk.wlc`), its own settings folder (it never touches a real
VLC's preferences), and VideoLAN's updater and crash reporter are switched off.

## Where the code is

| path | what |
|---|---|
| `modules/control/wlc/` | The Control Center interface module: HTTP server and access control (`wlc_http.c`), player/library/renamer/EQ API (`wlc_api.c`), converter (`wlc_jobs.c`), WilkCast and browser screens (`wlc_cast.c`), and the web pages (`ui/index.html`, `ui/screen.html`, embedded as `wlc_ui.h` by `tools-wlc/gen_ui.py`). |
| `modules/audio_filter/wlceq.c` | Power EQ audio filter. |
| `src/libvlc.c`, `src/libvlc-module.c` | Start the Control Center with the player (`--no-wlc-control-center` turns it off). |
| `modules/gui/macosx/` | Renamed strings, the Control Center menu item, About text. |
| `share/Info.plist.in`, `src/darwin/dirs.c`, `extras/package/macosx/configure.sh` | Mac identity, settings folder, no updater. |
| `tools-wlc/` | `make_icons.py` (draws the barrel and writes every Mac icon), `wilkcast.py` (stand-alone WilkCast sender/receiver), `e2e.py`, `ui_e2e.py` and `screen_e2e.py` (tests), `gen_ui.py`. |
| `.github/workflows/wlc-macos.yml` | Builds `WLC.app` and a DMG on GitHub's Mac machines. |
| `doc/wilkcast.md` | The casting protocol. |

Options: `--wlc-port` (default 48620; the next nine are tried if it is taken),
`--wlc-open` (open the Control Center at every start), `--no-wlc-control-center`.
Settings live in `wlc.json` in the configuration folder.

## Building

**Mac (the supported target for 0.1):** push this tree to a GitHub repository,
open *Actions ▸ WLC for macOS ▸ Run workflow*, and download the DMG artifact.
Or on a Mac with Xcode:

    mkdir build && cd build
    ../extras/package/macosx/build.sh -a aarch64     # or x86_64
    mv VLC.app WLC.app && codesign --force --deep --sign - WLC.app

The app is not notarized, so the first launch needs right-click ▸ Open (or
System Settings ▸ Privacy & Security ▸ Open Anyway).

**Linux (used for development and testing):** install VLC's build dependencies,
then `./bootstrap && mkdir build && cd build && ../configure && make`.
Run `./vlc` from the build folder.

## Tests

    python3 tools-wlc/e2e.py build/vlc            # 124 checks, two live instances
    python3 tools-wlc/ui_e2e.py http://127.0.0.1:48620 /folder/with/media
    python3 tools-wlc/screen_e2e.py http://<lan-ip>:48620 /folder/with/media   # 16 checks in Chrome

`e2e.py` covers the library, player, access control, byte ranges, renamer,
converter, WilkCast between two instances (queues, browser screens, and
`wilkcast.py` as both sender and receiver), and the EQ measured on real audio
output. `screen_e2e.py` signs a real Chrome in as a screen, casts a queue to it
and checks it really plays, pauses, seeks, reports an unplayable file and moves
on. `ui_e2e.py` drives the Control Center in Chrome, including a second
"phone" session over the LAN address.

## Status and known gaps (0.2.0)

* The new modules and tests run on Linux, and the new C code is compile-checked
  for Apple Silicon and Intel macOS. The Mac app itself is produced by the
  GitHub workflow; the Objective-C menu change is small but has only been
  built there, not on the development machine.
* Linux and Windows still carry VLC's name, icons and settings folder outside
  the Control Center. Android is a separate code base and has not been started.
* No DLNA/UPnP server yet: TVs cannot browse the library on their own. A TV
  with a web browser can be a WilkCast screen instead, or browse the Media
  Center over the LAN address.
* Browser screens only play what the browser has codecs for; WLC does not yet
  convert on the fly for them. A screen that closes its page stays listed for
  up to 15 seconds.
* FLAC output has no length in its header (a limitation of the stream writer
  inherited from VLC); it plays fine but some players show no duration.
* LAN traffic is plain HTTP protected by a PIN: meant for networks you trust.
* The translations of the renamed Mac strings fall back to English.

## License

GPL-2.0-or-later, like VLC (`COPYING`); the Power EQ module is
LGPL-2.1-or-later like VLC's other audio filters. Third-party components keep
their own licenses. The WilkCast specification is public domain.
