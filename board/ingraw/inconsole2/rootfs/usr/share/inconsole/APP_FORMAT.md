# InConsole App Manifest (V1)

Each app/game is discovered from:

- `/userdata/apps/<app_id>/app.json`
- `/userdata/apps/<app_id>/icon.png` (recommended 64x64 PNG)
- `/userdata/apps/<app_id>/launch.sh` (or another executable defined in `exec`)

## Required fields (`app.json`)

```json
{
  "id": "retro.tetris",
  "type": "game",
  "name": "Tetris",
  "description": "Classic falling blocks"
}
```

## Full example

```json
{
  "id": "retro.tetris",
  "type": "game",
  "name": "Tetris",
  "description": "Classic falling blocks",
  "icon": "icon.png",
  "exec": "launch.sh",
  "args": ["--fullscreen"],
  "category": "Games",
  "order": 10
}
```

## Notes

- `type`: `game`, `app`, or `tool`
- `category`: usually `Games`, `Apps`, `Settings`, `Diagnostics`
- `icon`: relative path inside app dir (defaults to `icon.png`)
- `exec`: relative path or absolute executable (defaults to `launch.sh`)
- `args`: optional array of string arguments
- `order`: optional integer sort key (smaller first)
- Invalid manifests are skipped and logged into `/userdata/system/logs/runtime-core.log`.
