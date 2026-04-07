# Decaflash

Monorepo for the Decaflash cube system.

## Current Status

V1 is intentionally small:

- `brain` can already broadcast demo commands and a separate beat clock over ESP-NOW
- `brain` can now read the Unit Mini PDM on raw-signal level and print live stats over serial
- `brain` can now display short serial-triggered text on the Matrix
- `brain` can now use stored Wi-Fi credentials for an initial HTTPS API test command
- `node` is the active V1 firmware for an ATOM Lite with Flashlight Unit
- microphone and RGB strip nodes come later
- the current node demo is driven directly with the ATOM button

## Project Structure

- `apps/brain/src` contains the future controller firmware
- `apps/node/src` contains the shared node app plus the current flashlight renderer
- `shared/include` contains types shared across apps
- `docs` contains scope notes and project documentation

## PlatformIO Environments

- `brain`: controller firmware for the ATOM Matrix
- `node`: current node firmware for the ATOM Lite + Flashlight Unit

The environment name `node` describes the device type, not a visual role.
Right now the only implemented node hardware is a flashlight node.
The node firmware is already structured so hardware-specific renderers can branch underneath the shared node logic.
The current flashlight demo programs are also shaped like future brain commands, so local demos and remote control can share the same data model.

## Build

Build brain firmware:

```bash
pio run -e brain
```

Build node firmware:

```bash
pio run -e node
```

## Flash

Flash brain firmware:

```bash
pio run -e brain -t upload
```

Flash node firmware:

```bash
pio run -e node -t upload
```

If PlatformIO does not pick the correct serial device automatically, specify the port explicitly:

```bash
pio run -e node -t upload --upload-port /dev/cu.usbserial-XXXX
```

Current local example mapping in this setup:

```bash
# Flashlight node
/dev/cu.usbserial-B956E80C38

# Brain
/dev/cu.usbserial-2D52E72138
```

Example uploads with the currently connected devices:

```bash
pio run -e node -t upload --upload-port /dev/cu.usbserial-B956E80C38
pio run -e brain -t upload --upload-port /dev/cu.usbserial-2D52E72138
```

## Serial Monitor

Open a serial monitor for the brain:

```bash
pio device monitor -e brain
```

Open a serial monitor for the node:

```bash
pio device monitor -e node
```

Example monitor commands with the current ports:

```bash
pio device monitor -e node --port /dev/cu.usbserial-B956E80C38
pio device monitor -e brain --port /dev/cu.usbserial-2D52E72138
```

Current `brain` serial commands:

- `text HALLO`
- `text HALLO, WELT`
- `text clear`
- `wifi status`
- `wifi scan`
- `wifi connect`
- `wifi disconnect`
- `api zen`
- `api relay`

## Brain Controls

Current `brain` button behavior on the ATOM Matrix:

- single short tap: start the brain if idle, otherwise switch immediately to the next scene

## Brain Microphone Input

The current `brain` firmware also initializes a Unit Mini PDM on the ATOM Matrix Grove port and prints raw input statistics to serial.

Current wiring for this setup:

- Unit Mini PDM `DATA` -> ATOM Matrix `G26` (yellow Grove wire)
- Unit Mini PDM `CLK` -> ATOM Matrix `G32` (white Grove wire)
- `5V` and `GND` as usual on the Grove port

Current capture settings:

- `16 kHz`
- `16-bit`
- mono PDM RX on `I2S_NUM_0`

Expected serial output after boot:

```text
mic=ready data_pin=26 clock_pin=32 sample_rate=16000 dma=8x128
mic=report fields=env avg peak dc raw_p2p samples
mic=level env=412 avg=537 peak=1732 dc=-228 raw_p2p=2890 samples=4096
mic=frame_centered 4 -3 7 12 -8 -6 3 9
```

Current preprocessing is intentionally still small:

- slow DC estimate for offset removal
- block-based loudness and transient tracking for live analysis
- 5x5 matrix VU meter when no scene UI is active
- prototype onset detection and BPM estimate on serial debug
- beat indicator dot over the UI: white on every beat, red on beat 1
- audio analysis can softly steer the live master beat clock after a stable lock

## Current Node Demo

The current `node` firmware is a simple standalone flashlight demo with five local programs:

- `Beat Drive`
- `Heavy Half`
- `Double Tap 3Hz`
- `Quad Skip`
- `Riser 5x`

Controls:

- short button press on the ATOM cycles to the next program
- long button press turns the flashlight output off

Timing:

- internal local clock at `120 BPM`
- `Beat Drive` hits every beat with a short, punchy flash
- `Heavy Half` hits only on beat 1 with a longer, heavier flash
- `Double Tap 3Hz` fires a 2-hit burst on beat 1 of every bar with `333 ms` spacing
- `Quad Skip` fires a 4-hit burst on beat 1 every second bar and tightens slightly inside the burst
- `Riser 5x` fires a 5-hit burst on beat 1 of every bar and accelerates inside the burst

## Command Model

The node now runs against an active command instead of hardcoded behavior branches. That is the same shape the brain can later send over radio.

See [`decaflash_types.h`](/Users/tiefencode/Projekte/decaflash/shared/include/decaflash_types.h) for the current shared shape.

Example for the current `Quad Skip` style command:

```cpp
NodeCommand quadSkip = {
  "Quad Skip",
  EffectType::BarBurst,
  255,
  2,
  1,
  4,
  260,
  -20,
  70
};
```

The node keeps one `activeCommand` in memory and renders that command locally. The important part now is that command data is separate from timing data.

## Protocol Model

On top of `NodeCommand`, the shared protocol now defines message envelopes in [`protocol.h`](/Users/tiefencode/Projekte/decaflash/shared/include/protocol.h):

```cpp
struct NodeCommandMessage {
  MessageHeader header;
  uint32_t commandRevision;
  NodeKind targetNodeKind;
  NodeCommand command;
};

struct ClockSyncMessage {
  MessageHeader header;
  uint32_t clockRevision;
  uint32_t beatSerial;
  uint16_t bpm;
  uint8_t beatsPerBar;
  uint8_t beatInBar;
  uint32_t currentBar;
};
```

That lets the brain send two different things:

- `NodeCommandMessage`: what a node should do
- `ClockSyncMessage`: when the shared musical clock currently is

Example command send:

```cpp
auto message = makeNodeCommandMessage(
  NodeKind::Flashlight,
  kFlashCommands[3],  // Quad Skip
  1
);
```

Example clock send:

```cpp
auto sync = makeClockSyncMessage(
  1,    // clock revision
  42,   // beat serial
  120,  // bpm
  4,    // beats per bar
  1,    // beat in bar
  8     // current bar
);
```

## ESP-NOW Step

The first transport step is now wired in and split cleanly:

- `brain` sends scene commands on scene changes and occasional refresh
- `brain` sends `ClockSyncMessage` once per bar
- `node` applies commands only when the revision changes
- `node` keeps its local clock running, but regularly re-locks to the brain clock
- small phase errors are trimmed softly on the next beat instead of always hard-resetting
- if clock sync disappears for a few seconds, the node falls back to local holdover instead of stopping

This is still intentionally simple:

- broadcast only
- no pairing
- no acknowledgements
- hard resync only when the phase is clearly off
- basic soft trim is in place, smarter smoothing can come later

## V1 Scope

- standalone flashlight node first
- local test patterns on an internal beat clock
- initial ESP-NOW master/slave transport
- raw microphone input plus prototype onset/BPM analysis on the brain
- audio BPM can softly guide the live beat clock when confidence stays high

## Next Steps

1. Test the new clock lock between brain matrix and flashlight node on hardware.
2. Refine the soft-sync strategy with measured latency and tighter phase heuristics.
3. Add persistent default preset selection on the node.
4. Refine the audio lock heuristics and phase trim so `currentBpm` stays stable across full songs.
