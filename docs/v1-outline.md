# Decaflash V1

## Scope

V1 focuses on the flashlight node as a standalone device.

- no radio yet
- no microphone yet
- no beat detection from audio yet
- no RGB node yet

## Device split

- `brain`: placeholder firmware for the future controller cube
- `node`: standalone flashlight firmware for the Atom Lite + Flashlight Unit

## Node V1 goals

- boot reliably
- run three clear flashlight base programs locally
- allow local mode switching with the ATOM button
- keep hardware-specific logic inside the node firmware
- prepare shared types for later radio commands

## Next steps

1. add local default preset selection
2. save the selected default preset to persistent storage
3. add ESP-NOW transport between brain and node
4. add microphone input on the brain
