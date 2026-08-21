# cnc-ddraw DirectDraw overlay

WineHua ships [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) v7.1.0.0 as a
**PE overlay**, not as a Wine fork change. Classic DirectDraw games (C&C, StarCraft,
Age of Empires) keep using `ddraw.dll`; when `WINEHUA_DDRAW_BACKEND=cnc` is set,
WineHua prepends the overlay directory to `WINEDLLDIR` and sets
`WINEDLLOVERRIDES=ddraw=n`.

This is the same family of path injection used for DXVK, but it does **not** go
through `ntdll` `search_winehua_dxvk_overlay` (that hook only remaps
`d3d11.dll` / `dxgi.dll` / `d3d12.dll`).

## Runtime layout

```
wine-data/cnc-ddraw/
  ddraw.ini          # WineHua defaults (renderer=opengl, windowed, hook=4)
  manifest.json
  x86/ddraw.dll      # required (Win32; x64 is not shipped)
```

`AppendDdrawBackendEnv` also sets `CNC_DDRAW_CONFIG_FILE` to the overlay
`ddraw.ini` so the game working directory does not need a local copy.

## Enable per game

Pass in the launch environment (game profile `d3d_env` or smoke `environment`):

```
WINEHUA_DDRAW_BACKEND=cnc
```

Smoke suite: `ddraw` (x86 probe `winehua_ddraw_smoke.exe`). The probe requires
`DDGetProcAddress` when the overlay is requested, proving cnc-ddraw loaded
instead of Wine's builtin `ddraw`.

## Build

```
make cnc-ddraw     # MinGW i686, isolated source copy
make assemble      # stages overlay + smoke
```

The submodule `thirdparty/cnc-ddraw` stays pinned at tag `v7.1.0.0`. Do not
edit it in place; WineHua defaults live in `scripts/build_cnc_ddraw.sh`.
