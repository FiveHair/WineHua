# Pad GL reapply plan

Date: 2026-07-09

This note is the recovery checklist after backing up the GL work-in-progress to
`backup/gl-wip-20260709` and resetting the main tree back to clean `dev`.

## Baseline gate

- Source baseline: `dev` at `92adcff`.
- Submodules are reset to the `dev` pointers.
- Build target is ARM64 Pad through the WSL + Docker flow.
- Before reapplying GL, confirm first-run startup reaches the Wine desktop and
  audio still works.

## Do not merge wholesale

The backup branch contains mixed changes. Do not merge it as a branch.

Skip these unless a later log proves they are needed:

- root-level temporary reports and PowerShell diagnostic scripts
- large `Index.ets` UI rewrite
- broad Wine `ntdll` experiments
- broad `win32u/opengl.c` rewrite until reviewed separately
- SDL/audio environment changes mixed into GL setup
- host `LD_LIBRARY_PATH` changes that expose x86_64 guest Mesa to ARM64 native code

## Candidate GL pieces

Apply in small commits after the baseline gate passes:

1. Guest Mesa/virpipe packaging:
   - `scripts/build_guest_gfx.sh`
   - narrowly needed parts of `scripts/build_ohos_guest_gfx.sh`
   - narrowly needed `scripts/assemble.sh` packaging for `guest_gfx`
   - `Makefile` target wiring, but keep startup unrelated rules unchanged

2. Host VirGL server support:
   - `entry/src/main/cpp/virgl_child.cpp`
   - `entry/src/main/cpp/CMakeLists.txt` only for `libvirgl_child.so`
   - Pad host side must load ARM64 native libraries only

3. Runtime broker integration:
   - `GraphicsBroker` NCP launch path for `virgl_child`
   - keep base desktop startup on SHM
   - prepare GL env only after desktop readiness, not during `wineboot`

4. Wine WGL/Wayland pieces:
   - small `opengl32` fixes and diagnostics
   - `winewayland.drv` GLES readback path
   - review `win32u/opengl.c` separately before taking it

5. Box64 Mesa wrapper support:
   - `rint` / `rintf` libc wrapper additions

6. Tests:
   - graphics smoke/test executables after the normal WGL path works
   - test programs should call WGL normally; no project-specific function
     lookup requirement for ordinary applications

## Device notes

Use Windows hdc for real-device work:

`C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe`

If USB shows `Connected` but commands fail with `handshake is not ready` or
`Device not found or connected`, recover hdc/USB authorization before judging
the app build.
