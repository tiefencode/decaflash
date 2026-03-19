# Decaflash

Monorepo for the Decaflash cube system.

## Current Status

V1 is intentionally small:

- `brain` exists as a placeholder firmware for the ATOM Matrix controller
- `node` is the active V1 firmware for an ATOM Lite with Flashlight Unit
- microphone, ESP-NOW, and RGB strip nodes come later
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

## Serial Monitor

Open a serial monitor for the brain:

```bash
pio device monitor -e brain
```

Open a serial monitor for the node:

```bash
pio device monitor -e node
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

```cpp
struct NodeCommand {
  const char* name;
  EffectType effect;
  uint8_t intensity;
  uint8_t triggerEveryBars;
  uint8_t triggerBeat;
  uint8_t burstCount;
  uint16_t burstIntervalMs;
  int16_t burstIntervalStepMs;
  uint16_t flashDurationMs;
};
```

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

The node keeps one `activeCommand` in memory and renders that command locally. Later the brain can replace that same structure with live scene data.

## V1 Scope

- standalone flashlight node first
- local test patterns on an internal beat clock
- no radio yet
- no microphone yet
- no beat detection from audio yet

## Next Steps

1. Add persistent default preset selection on the node.
2. Decide whether long button press should store default, blackout, or both.
3. Add ESP-NOW communication between brain and node.
4. Add microphone input and beat detection on the brain.
