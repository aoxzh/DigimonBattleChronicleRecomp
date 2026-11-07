# Desktop disc images

The desktop runner reads the disc directly for DI/FST/DVD traffic. The
generated CPU code is still built from the exact `main.dol` that was lifted, so
the image must be the Japanese `G2DJB2` revision 0.

## Formats

| Format | Status |
|---|---|
| `.iso`, `.gcm` | Direct; no import step |
| `.rvz` | Import once with `tools/prepare_disc.py` (needs `zstandard`); the runner reads the cached ISO |
| `.gcz`, `.ciso` | Import once with `tools/prepare_disc.py`; pure-Python decoders, no extra dependency |
| `.wia` | Not decoded yet; convert to ISO with Dolphin (`right-click game > Convert File`) |

The launcher still accepts the compressed path so it can show an actionable
message instead of "compressed images are not supported".

## RVZ import

RVZ needs Python 3 and the `zstandard` module:

```sh
python -m pip install zstandard
python tools/prepare_disc.py "/path/to/Digimon Battle Chronicle (Japan).rvz"
```

On Windows, `tools\prepare-disc.bat` is a drag-and-drop wrapper. On Linux/macOS
use `tools/prepare-disc.sh`. `--check` validates an already-raw ISO without
writing; `--force` replaces a stale import; `--out` and `--cache-dir` override
the default paths.

GCZ and CISO are decoded by the helper's own small decoders (new in this
revision; synthetic round-trips are covered by `tests/test_prepare_disc.py`).
WIA still needs Dolphin; its codecs are not implemented here.

The cache name is `<data dir>/cache/<image stem>.iso`, matching
`settings_resolve_disc()` in `platform/desktop/upstream_gx/settings.c`:

- Windows: `%APPDATA%\DigimonBattleGC\cache\`
- Linux: `$XDG_DATA_HOME/DigimonBattleGC/cache/` (default `~/.local/share/...`)
- macOS: `~/Library/Application Support/DigimonBattleGC/cache/`

Portable builds (`portable.txt` beside the executable) use `<exe>/user/cache/`
on the C side; pass `--cache-dir` if you use that layout.

The helper verifies the DVD magic, game ID `G2DJB2`, revision 0, the lifted
DOL size (3,591,200 bytes) and its FNV-1a hash before declaring the image
usable. A partial/failed extraction is removed.

## What this does not do

- It does not build the generated C from the disc. The repository still owns
  the generated layer and its versioned inputs; see
  `architecture.md` and `../../docs/NGC_RECOMP_ACCELERATION.md`.
- It does not include or download game data. Bring your own disc image.
- It does not support WIA yet; that remains on the gap list in
  `../../docs/GITHUB_RECOMP_GAP_2026-11-07.md`.
