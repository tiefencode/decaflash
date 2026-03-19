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
- `apps/node/src` contains the current standalone node firmware
- `shared/include` contains types shared across apps
- `docs` contains scope notes and project documentation

## PlatformIO Environments

- `brain`: controller firmware for the ATOM Matrix
- `node`: current node firmware for the ATOM Lite + Flashlight Unit

The environment name `node` describes the device type, not a visual role.
Right now the only implemented node hardware is a flashlight node.
If RGB nodes are added later, we can either keep one generic `node` firmware or split into hardware-specific environments when needed.

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

The current `node` firmware is a simple standalone flashlight demo with three local programs:

- `Flash 3Hz`
- `Pulse Slow`
- `Strobe`

Controls:

- short button press on the ATOM cycles to the next program
- long button press turns the flashlight output off

## V1 Scope

- standalone flashlight node first
- local test patterns over serial
- no radio yet
- no microphone yet
- no beat detection from audio yet

## Next Steps

1. Add persistent default preset selection on the node.
2. Decide whether long button press should store default, blackout, or both.
3. Add ESP-NOW communication between brain and node.
4. Add microphone input and beat detection on the brain.
