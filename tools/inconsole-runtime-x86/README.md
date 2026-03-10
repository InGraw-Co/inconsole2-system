# inconsole-runtime-x86

Desktop test harness for `inconsole-runtime`, intended for Linux x86 visual iteration.

## Goals
- Same UI flow as console runtime.
- Same logical resolution: `480x272`.
- Fast iteration outside Buildroot.

## Dependencies (Ubuntu/Debian example)
```sh
sudo apt-get install -y build-essential cmake pkg-config libsdl1.2-dev libfreetype-dev libpng-dev
```

`cmake` is optional. If missing, `build.sh` automatically uses direct `g++` build fallback.

## Run
```sh
cd tools/inconsole-runtime-x86
./run.sh
```

## Data location
- Default: `tools/inconsole-runtime-x86/userdata-x86`
- Override with env:
```sh
INCONSOLE_DATA_ROOT=/path/to/data ./run.sh
```

## Controls (keyboard)
- `Arrow keys`: navigation
- `Enter` / `Space` / `Z`: `A` (accept)
- `Esc` / `Backspace` / `X`: `B` (back)
- `Tab` / `P`: `START` (menu/settings)
- `Q` / `E`: `L` / `R`

## Notes
- This folder is the x86 test variant only.
- Console-specific hardware paths are intentionally redirected to local data root.
