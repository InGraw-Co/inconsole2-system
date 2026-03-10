#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DATA_ROOT="${INCONSOLE_DATA_ROOT:-$ROOT_DIR/userdata-x86}"

"$ROOT_DIR/build.sh"

mkdir -p "$DATA_ROOT/apps" "$DATA_ROOT/system/logs"

if [ ! -f "$DATA_ROOT/system/settings.json" ]; then
    cat > "$DATA_ROOT/system/settings.json" <<'JSON'
{
  "language": "pl",
  "theme_id": "tech_noir",
  "volume": 70,
  "brightness": 80,
  "animations": true
}
JSON
fi

if [ ! -d "$DATA_ROOT/apps/demo-app" ]; then
    mkdir -p "$DATA_ROOT/apps/demo-app"
    cat > "$DATA_ROOT/apps/demo-app/app.json" <<'JSON'
{
  "id": "demo.app",
  "name": "Demo App",
  "type": "tool",
  "description": "Desktop test app for launcher visuals",
  "category": "Demo",
  "order": 10,
  "exec": "launch.sh",
  "icon": "icon.png"
}
JSON
    cat > "$DATA_ROOT/apps/demo-app/launch.sh" <<'SH'
#!/bin/sh
echo "Demo app launched at $(date)" >> "${INCONSOLE_DATA_ROOT:-./userdata-x86}/system/logs/demo-app.log"
SH
    chmod +x "$DATA_ROOT/apps/demo-app/launch.sh"
fi

if [ -f "$ROOT_DIR/test.png" ]; then
    cp -f "$ROOT_DIR/test.png" "$DATA_ROOT/apps/demo-app/icon.png"
fi

export INCONSOLE_DATA_ROOT="$DATA_ROOT"
exec "$ROOT_DIR/build/inconsole-runtime-x86"
