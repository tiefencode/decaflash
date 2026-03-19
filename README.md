# Decaflash

Monorepo for the Decaflash cube system.

## Current Status

V1 is intentionally small:

- `brain` can already broadcast demo commands and a separate beat clock over ESP-NOW
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

- `brain` sends `NodeCommandMessage` on mode changes and occasional refresh
- `brain` sends `ClockSyncMessage` on every beat
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
- no microphone yet
- no beat detection from audio yet

## Next Steps

1. Test the new clock lock between brain matrix and flashlight node on hardware.
2. Refine the soft-sync strategy with measured latency and tighter phase heuristics.
3. Add persistent default preset selection on the node.
4. Add microphone input and beat detection on the brain.
