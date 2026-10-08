# midi1_loopback

A small macOS utility that creates **MIDI 1.0 virtual loopback buses** as a
drop-in replacement for the Core MIDI **IAC Driver** — with one crucial
difference: it passes a **Note On with velocity 0** (`90 nn 00`) through
**unchanged**, instead of silently rewriting it to a Note Off.

Built on [RtMidi](https://github.com/thestk/rtmidi).

## The problem it solves

On macOS, routing MIDI through the IAC Driver mangles a very specific message:

```
You send:      90 3C 00     (Note On, note 60, velocity 0)
IAC delivers:  80 3C 40     (Note Off, note 60, velocity 64)
```

This breaks protocols — most notably **Mackie Control (MCU)** — that use *Note
On with velocity 0* to signal a button release and deliberately **ignore** Note
Off messages. On such a setup the control surface's buttons appear stuck.

This is the issue reported upstream in RtMidi:
**[thestk/rtmidi#320 — Note On velocity 0 is translated to Note Off on macOS](https://github.com/thestk/rtmidi/issues/320)**.

### Why Core MIDI does this

It is **not** a bug in RtMidi — it happens inside Core MIDI itself.

Since macOS 11, Core MIDI is a **MIDI 2.0** stack internally (Universal MIDI
Packet / UMP). In the **MIDI 2.0** specification, Note On and Note Off are two
fully distinct messages, each with 16-bit velocity, and the old **MIDI 1.0**
convention that *"a Note On with velocity 0 means Note Off"* **no longer
applies** — a MIDI 2.0 Note On simply isn't allowed to carry that implied
meaning.

To bridge the two worlds, Core MIDI applies the MIDI Association's standard
**MIDI 1.0 ↔ 2.0 default translation** rules. One of those rules says: a
legacy MIDI 1.0 *Note On, velocity 0* must be translated to a MIDI 2.0
*Note Off*. The modern IAC Driver is a MIDI 2.0 endpoint, so any 1.0 byte
stream that passes through it is round-tripped `1.0 → 2.0 → 1.0`, and the
velocity-0 Note On comes back out as a Note Off (`80 nn 40`).

### Why this tool avoids it

RtMidi creates its virtual endpoints using Core MIDI's **legacy
`MIDIPacketList` (MIDI 1.0) API**. When a MIDI 1.0 source is connected directly
to a MIDI 1.0 destination, Core MIDI performs **no protocol conversion**, so the
translation rule never fires and the bytes are preserved exactly.

`midi1_loopback` turns that into a usable loopback "bus": for each bus it
creates two virtual endpoints that share one name —

- a virtual **destination** (apps send to it), and
- a virtual **source** (apps read from it) —

and forwards everything from the destination to the source, verbatim. Other
apps see e.g. `M1 Bus 1` as both a MIDI input and a MIDI output, exactly like
`IAC Driver Bus 1`, but without the Note On/velocity-0 rewrite.

> **Note:** these virtual ports exist only while the program is running. Unlike
> the IAC Driver they are not persistent across reboots; run the tool (or set it
> up as a `launchd` agent) whenever you need the buses.

## Requirements

- macOS (uses the Core MIDI backend)
- A C++11 compiler (Apple Clang)
- CMake ≥ 3.14
- Network access for the first configure (RtMidi is fetched automatically)

## Build

```sh
cmake -S . -B build
cmake --build build
```

The executable is written to `build/midi1_loopback`.

By default CMake downloads a pinned RtMidi release via `FetchContent`. To build
against a local/installed RtMidi instead:

```sh
cmake -S . -B build -DRTMIDI_FETCH=OFF
cmake --build build
```

## Usage

```sh
./build/midi1_loopback [numBuses] [options]
```

Run with `--help` for the full list of flags.

| Form | Effect |
| --- | --- |
| `midi1_loopback` | Load `port_config.json` (see below); otherwise 2 default buses |
| `midi1_loopback 4` | 4 buses named `RtMidi Loopback Bus 1…4` |
| `midi1_loopback --names "MCU,DAW Sync,Click"` | One bus per comma-separated name |
| `midi1_loopback --config path/to/ports.json` | Load buses from a specific JSON file |
| `midi1_loopback -v` / `--verbose` | Print every forwarded message as hex |
| `midi1_loopback --iac-test` | Diagnostic: check whether IAC still translates Note On/vel 0, then exit |
| `midi1_loopback -h` / `--help` | Show help and exit |

The program runs until `Ctrl-C`.

### Port-selection precedence

1. `--names "..."` — explicit names win and set the bus count.
2. `numBuses` positional — that many buses with default names.
3. JSON config — used when neither of the above is given.
4. Built-in default — 2 buses, if no args and no config file is found.

## Configuration file

When started with no port-selecting argument, `midi1_loopback` loads a JSON
config. It looks for `./port_config.json`, then `../port_config.json`; override
the path with `--config <path>`.

```json
{
  "verbose": false,
  "ports": [
    { "name": "M1 Bus 1" },
    { "name": "M1 Bus 2" }
  ]
}
```

- `ports` — the list of buses to create, in order; each `name` becomes a
  bus that appears as both a MIDI input and output.
- `verbose` — optional; a command-line `--verbose` always overrides it.

Up to 16 buses are supported.

## Checking whether IAC still has the bug (`--iac-test`)

`--iac-test` is a self-contained diagnostic. It sends a Note On/velocity 0
through a real **IAC Driver** bus, reads it back, and reports whether CoreMIDI
translated it — and it prints the **macOS version** so the result can be
recorded against a specific OS release. This is intended to confirm, on future
macOS versions, whether Apple has changed the behavior.

It requires the IAC Driver to be enabled (Audio MIDI Setup → *Show MIDI Studio*
→ **IAC Driver** → *Device is online*, with at least one bus).

```
$ midi1_loopback --iac-test
midi1_loopback --iac-test
OS: macOS 26.3 (build 25D125), Darwin 25.3.0
RtMidi: 6.0.0

Purpose: verify whether the macOS IAC Driver rewrites a
MIDI 1.0 "Note On, velocity 0" (90 3C 00) into a "Note Off"
(80 3C 40) as it passes through.

Using IAC output port [0] IAC Driver Bus 1
Using IAC input  port [0] IAC Driver Bus 1

Sent:     90 3C 00   (Note On, note 60, velocity 0)
Received: 80 3C 40

RESULT: TRANSLATED. The IAC Driver converted Note On/velocity 0
Use midi1_loopback's virtual buses instead of IAC to avoid this.
```

- **TRANSLATED** (received `80 3C 40`) — the bug is still present on this macOS.
- **NOT translated** (received `90 3C 00`) — IAC preserved the message; if it
  used to translate, this macOS version appears to have fixed it.
- **INCONCLUSIVE** — no IAC port was found, or nothing came back (see the
  on-screen hint).

## Running persistently at login (launchd)

The virtual buses exist only while `midi1_loopback` is running. To keep them
available automatically, install it as a per-user **LaunchAgent**. (A user
session is required — CoreMIDI virtual ports live in the per-user MIDIServer —
so this is a LaunchAgent, not a system-wide LaunchDaemon.)

1. Install the binary and example config (default prefix `/usr/local`):

   ```sh
   sudo cmake --install build
   ```

   This installs `/usr/local/bin/midi1_loopback` and
   `/usr/local/share/midi1_loopback/port_config.json`. Edit that config (or
   point the plist at a different one) to define your buses.

2. Copy the provided LaunchAgent into place:

   ```sh
   cp launchd/com.musekinetics.midi1_loopback.plist ~/Library/LaunchAgents/
   ```

   The plist's paths match the install locations above; adjust them if you used
   a different prefix or config location.

3. Load it (it will also start automatically at every login):

   ```sh
   launchctl load ~/Library/LaunchAgents/com.musekinetics.midi1_loopback.plist
   ```

To stop and remove it:

```sh
launchctl unload ~/Library/LaunchAgents/com.musekinetics.midi1_loopback.plist
```

Logs are written to `/tmp/midi1_loopback.out.log` and
`/tmp/midi1_loopback.err.log` (configurable in the plist).

## License

Released under the [MIT License](LICENSE) © KMI Music, Inc.

This project links against RtMidi, which is distributed under its own MIT-style
license. See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
