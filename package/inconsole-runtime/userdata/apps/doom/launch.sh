#!/bin/sh
set -eu

PRBOOM_BIN="${INCONSOLE_PRBOOM_BIN:-}"
BRIDGE_BIN="/usr/bin/inconsole-input-bridge"
RUNTIME_BIN="/usr/bin/inconsole-runtime"
IWAD_DIR="/usr/share/games/doom"
CFG_DIR="/userdata/system/doom"
CFG_FILE="${CFG_DIR}/prboom.cfg"
LOG_DIR="/userdata/system/logs"
DOOM_LOG="${INCONSOLE_DOOM_LOG:-${LOG_DIR}/doom-run.log}"
VIDEO_W="${INCONSOLE_DOOM_WIDTH:-480}"
VIDEO_H="${INCONSOLE_DOOM_HEIGHT:-272}"
STARTUP_TIMEOUT="${INCONSOLE_DOOM_STARTUP_TIMEOUT:-8}"
PRBOOM_EXTRA_ARGS="${INCONSOLE_DOOM_EXTRA_ARGS:-}"
BRIDGE_PID=""

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-fbcon}"
export SDL_FBDEV="${SDL_FBDEV:-/dev/fb0}"
export SDL_NOMOUSE=1
export HOME="${HOME:-/userdata}"

mkdir -p "${CFG_DIR}"
mkdir -p "${LOG_DIR}"

if [ ! -f "${CFG_FILE}" ]; then
	cat > "${CFG_FILE}" << 'CFG'
use_mouse               0
use_joystick            1
joyb_fire               0
joyb_use                1
joyb_speed              4
joyb_strafe             5
screenblocks            10
CFG
fi

IWAD=""
for c in "${IWAD_DIR}/doom1.wad" "${IWAD_DIR}/DOOM1.WAD" "${IWAD_DIR}/doom.wad" "${IWAD_DIR}/DOOM.WAD"; do
	if [ -f "$c" ]; then
		IWAD="$c"
		break
	fi
done

ts() {
	date '+%Y-%m-%d %H:%M:%S'
}

log_line() {
	printf '%s doom-launcher: %s\n' "$(ts)" "$*" >> "${DOOM_LOG}"
}

stop_bridge() {
	if [ -n "${BRIDGE_PID}" ] && kill -0 "${BRIDGE_PID}" 2>/dev/null; then
		kill "${BRIDGE_PID}" 2>/dev/null || true
		sleep 1
		kill -9 "${BRIDGE_PID}" 2>/dev/null || true
	fi
}

start_bridge() {
	if [ ! -e /dev/uinput ] && command -v modprobe >/dev/null 2>&1; then
		modprobe uinput >/dev/null 2>&1 || true
	fi
	if [ ! -e /dev/uinput ] && [ ! -e /dev/input/uinput ]; then
		log_line "input bridge disabled: missing /dev/uinput"
		return
	fi
	if [ ! -x "${BRIDGE_BIN}" ]; then
		log_line "input bridge missing: ${BRIDGE_BIN}"
		return
	fi
	"${BRIDGE_BIN}" >> "${DOOM_LOG}" 2>&1 &
	BRIDGE_PID=$!
	log_line "input bridge started pid=${BRIDGE_PID}"
}

if [ -z "${PRBOOM_BIN}" ]; then
	for c in /usr/games/prboom /usr/bin/prboom; do
		if [ -x "$c" ]; then
			PRBOOM_BIN="$c"
			break
		fi
	done
fi

rm -f "${DOOM_LOG}"
printf '%s\n' "---- doom run ---- $(ts)" >> "${DOOM_LOG}"
start_bridge
trap 'stop_bridge' EXIT INT TERM

start_prboom_try() {
	w="$1"
	h="$2"
	log_line "try start prboom bin=${PRBOOM_BIN} iwad=${IWAD} geom=${w}x${h}"

	"${PRBOOM_BIN}" \
		-iwad "${IWAD}" \
		-config "${CFG_FILE}" \
		-nomouse \
		-nowindow \
		-geom "${w}x${h}" \
		-width "${w}" \
		-height "${h}" \
		-nosound \
		-nomusic \
		${PRBOOM_EXTRA_ARGS} >> "${DOOM_LOG}" 2>&1 &
	DOOM_PID=$!

	started=0
	t=0
	while [ "$t" -lt "$STARTUP_TIMEOUT" ]; do
		if ! kill -0 "$DOOM_PID" 2>/dev/null; then
			break
		fi
		if grep -q "I_InitGraphics:" "${DOOM_LOG}" 2>/dev/null || grep -q "I_UpdateVideoMode:" "${DOOM_LOG}" 2>/dev/null; then
			started=1
			break
		fi
		sleep 1
		t=$((t + 1))
	done

	if [ "$started" -eq 0 ] && kill -0 "$DOOM_PID" 2>/dev/null; then
		log_line "startup timeout (${STARTUP_TIMEOUT}s), killing pid=${DOOM_PID}"
		kill "$DOOM_PID" 2>/dev/null || true
		sleep 1
		kill -9 "$DOOM_PID" 2>/dev/null || true
	fi

	set +e
	wait "$DOOM_PID"
	rc=$?
	set -e
	log_line "prboom finished rc=${rc} started=${started} geom=${w}x${h}"

	if [ "$started" -eq 1 ]; then
		return 0
	fi
	return 1
}

if [ -x "${PRBOOM_BIN}" ] && [ -n "${IWAD}" ]; then
	TRIES="${VIDEO_W}x${VIDEO_H} 480x272 400x240 320x240 320x200 640x480"
	for mode in ${TRIES}; do
		w="${mode%x*}"
		h="${mode#*x}"
		if start_prboom_try "$w" "$h"; then
			log_line "doom session completed successfully"
			break
		fi
	done
else
	log_line "prboom not started (binary missing or iwad missing) bin=${PRBOOM_BIN} iwad=${IWAD}"
fi

log_line "returning to runtime"
stop_bridge
exec "${RUNTIME_BIN}"
