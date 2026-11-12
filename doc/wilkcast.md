# WilkCast 1 — a small casting protocol for home and office networks

WilkCast lets one program tell another on the same local network "play this,
from here". It is deliberately tiny: one UDP beacon for finding devices and
four HTTP calls for control. Media travels as a plain HTTP download with byte
ranges, so the receiver can seek. Nothing leaves the local network and there is
no cloud service, account or pairing server.

WLC (Wilkinson LAN Client) is the reference implementation
(`modules/control/wlc/wlc_cast.c`). `tools-wlc/wilkcast.py` is a second,
stand-alone one in a single Python file: a command-line sender and a receiver
that drives mpv, ffplay or VLC. Anything that can send UDP and speak HTTP can
implement either side. This document is the whole specification.

Revision 1.1 (WLC 0.2) added queues, browser screens and the `base` and
`kind` fields. Every addition is optional, and a 1.0 device keeps working
with a 1.1 device in both directions.

## Roles

* **Receiver** — plays media when asked. Runs an HTTP server.
* **Sender** — finds receivers, serves the media file over HTTP, and sends
  commands. A WLC instance is both at once.
* **Host** — a receiver may also stand in for devices that cannot run a server
  themselves. WLC does this for *browser screens* (section 4).

## 1. Discovery

Every receiver announces itself every 4 seconds with one UDP datagram sent to

* multicast group `239.87.76.67`, port `48621` (TTL 1), and
* the broadcast address of each network interface, port `48621`
  (for networks that drop multicast).

The datagram is ASCII/UTF-8 text:

    WILKCAST/1 {"id":"<32 hex chars>","name":"Living room Mac","port":48620,"auth":"pin"}

| field  | meaning |
|--------|---------|
| `id`   | Stable identifier of the device. Minimum 8 characters. |
| `name` | What to show to people. |
| `port` | TCP port of the receiver's HTTP server. The IP address is the datagram's source address. |
| `auth` | `"pin"`: commands need the PIN. `"open"`: anyone on the network may cast. `"off"`: not accepting casts. |
| `bye`  | Optional, `true` in a final datagram when the device shuts down. |
| `base` | Optional (1.1). Path prefix of the device's HTTP API. Default `/cast/v1/`. Must start with `/cast/v1/` and end with `/`. |
| `kind` | Optional (1.1). What the device is, for display: `"wlc"`, `"browser"`, `"script"`, … |

A device that has just started may send `WILKCAST/1 ?`; everyone who hears it
announces immediately. A device not heard from for 15 seconds is forgotten.
Unknown JSON fields must be ignored. Datagrams are at most 1023 bytes.

If discovery is blocked on a network, a sender can be given the address by hand
and call `GET /cast/v1/info` instead.

## 2. Receiver HTTP API

Paths below are written with the default prefix `/cast/v1/`; a device that
announced another `base` is called at `<base>info`, `<base>play` and so on.

All bodies are JSON (`Content-Type: application/json`). When `auth` is `"pin"`,
every call except `info` carries the PIN in a header:

    X-WilkCast-Pin: 123456

Senders should also send `X-WilkCast-From: <their id>`.

| call | body | result |
|------|------|--------|
| `GET /cast/v1/info` | – | `{"wilkcast":1,"app":"WLC","version":"0.1.0","id":"…","name":"…","auth":"pin"}` — never needs a PIN. |
| `POST /cast/v1/play` | `{"url":"http://…","title":"…","position":0,"from":"…","queue":[{"url":"…","title":"…"}]}` | Start playing `url` now, from `position` seconds. `url` must be `http` or `https`. `queue` (1.1, optional, at most 500 entries) is played in order after `url` and replaces anything already lined up; `next` and `prev` move through it. A 1.0 receiver ignores it and plays only `url`. |
| `POST /cast/v1/control` | `{"cmd":"…","value":…}` | `cmd` is one of `play`, `resume`, `pause`, `toggle`, `stop`, `next`, `prev`, `seek` (value = seconds), `seekrel` (value = ± seconds), `volume` (value 0–2, 1 = 100 %). Returns the status object. |
| `GET /cast/v1/status` | – | `{"state":"playing"|"paused"|"stopped"|"opening","time":12.3,"length":180.0,"volume":1.0,"title":"…","artist":"…","album":"…","name":"…", …}` |

Status codes: `200` done, `400` bad request, `401` PIN missing or wrong (also
returned for 60 seconds after five wrong PINs in a row), `403` this receiver
does not accept casts, `404` unknown call.

## 3. Serving the media

The sender makes the file available at an HTTP URL the receiver can reach and
puts that URL in `play`. Requirements on that URL:

* `GET` and `HEAD`, with `Range: bytes=a-b`, `bytes=a-` and `bytes=-n`
  answered by `206` + `Content-Range` (and `416` past the end);
* `Accept-Ranges: bytes` and a correct `Content-Length`;
* it should end in the file's name so the receiver can guess the type.

WLC serves `http://<sender-ip>:<port>/media/<key>/<absolute path>` where `<key>`
is a random 128-bit value made at start-up. Only files inside the folders the
user added to the Media Center, or files the user explicitly cast, can be
fetched with it. The sender learns which of its own addresses to use by looking
at the local address of its TCP connection to the receiver.

A sender may also pass along a URL it does not serve itself (an internet radio
stream, for instance).

## 4. Browser screens (1.1)

A smart TV, phone or old laptop can be a receiver with nothing installed:
its web browser opens `http://<wlc-address>:48620/screen` and signs in with
that WLC's PIN. From then on the hosting WLC

* announces the screen with its own beacon: its own `id` and `name`, the
  host's `port` and `auth`, `"kind":"browser"` and
  `"base":"/cast/v1/s/<screen id>/"`;
* answers `info`, `play`, `control` and `status` under that `base`, with the
  same PIN rules as for itself, by queueing the command for the screen and
  reporting the status the screen last sent;
* lists its screens in its own `info` answer as
  `"screens":[{"id":"…","name":"…","base":"…"}]`.

Senders need nothing special: they just use the `base` from the beacon. The
link between a screen page and its host is internal to WLC (the page posts its
status and collects commands twice a second), so another implementation may
host screens differently.

A browser can only play what it has codecs for: MP4 (H.264/AAC), WebM, MP3,
AAC, WAV and usually FLAC. When it cannot play a file the screen reports
`"state":"error"` with an `"error"` text, then goes on to the next queued item.

## 5. "Move" (hand-off)

To continue on another device, the sender reads its own position, sends `play`
with that `position`, and stops its own playback once the receiver answers 200.

## 6. Security notes

* WilkCast is for networks you trust: traffic is not encrypted, and a PIN sent
  in a header can be seen by someone who can sniff the network.
* Receivers only accept `http`/`https` URLs, never local files or other
  schemes, and only the commands in the table above.
* The PIN is six digits with a lock-out; receivers should let the user turn
  casting off and make a new PIN.
* A receiver must not treat a cast as a login: a correct PIN on `/cast/v1/*`
  gives access to those calls only.
* A browser screen signs in with the PIN once and keeps a session cookie; a new
  PIN on the host signs every screen out.
* `base` is only ever used as a path on the same address and port as the
  beacon came from, so a beacon cannot point a sender at another machine.

## 7. Minimal examples

Find receivers (bash + socat):

    socat -u UDP4-RECV:48621,ip-add-membership=239.87.76.67:0.0.0.0,reuseaddr -

Cast a file you serve yourself:

    curl -H 'X-WilkCast-Pin: 123456' -H 'Content-Type: application/json' \
         -d '{"url":"http://192.168.1.10:8000/movie.mp4","title":"Movie night"}' \
         http://192.168.1.20:48620/cast/v1/play

Pause it:

    curl -H 'X-WilkCast-Pin: 123456' -H 'Content-Type: application/json' \
         -d '{"cmd":"pause"}' http://192.168.1.20:48620/cast/v1/control

Or with the reference script:

    tools-wlc/wilkcast.py discover
    tools-wlc/wilkcast.py cast "Living room TV" episode1.mp4 episode2.mp4 --pin 123456
    tools-wlc/wilkcast.py ctl "Living room TV" seekrel 30 --pin 123456
    tools-wlc/wilkcast.py receive --name "Garage PC" --player mpv

WilkCast 1 is placed in the public domain; implement it freely.
