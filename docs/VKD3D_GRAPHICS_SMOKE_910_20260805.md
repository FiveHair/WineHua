# VKD3D Guest Swapchain Smoke Evidence — 910 — 2026-08-05

## Scope

This is isolated evidence for the `feature/vkd3d-capability-probe` branch. It
does not install VKD3D into the default Wine runtime, does not change the DXVK
Legacy/Modern selections, and does not enable D3D12 for ordinary launches.

Device: `62T0225B10005882` (910)

## Source and artifacts

- VKD3D-Proton runtime source: `f74c040a29688cd1e437bf089fd6548b89b00504`
- Graphics smoke source commit: `a302f9d4` (`test(vkd3d): add deterministic graphics present smoke`)
- `d3d12.dll` SHA-256: `01d74e215a3a7fb2172749f6cbd08bb746706cef3ba63c841b1444572ada8003`
- `vkd3d-graphics-smoketest.exe` SHA-256: `5632b68ae47e90b27a82400eb81d179d9ad1df23cabfdd14f8c0e0ed1d6d11ad`
- Signed HAP SHA-256: `688cf87035714bd0a9413ce829c98363b2357e9bbbc97a5c37a0b49eece68443`

The Docker build exited zero. Both payload files are x86-64 PE files. The
smoke executable import table contains only `GDI32.dll`, `KERNEL32.dll`,
`msvcrt.dll`, and `USER32.dll`; D3D12 and DXGI are loaded after `main()` with
hash-verified experiment-local payload resolution.

## Root-cause corrections

The first graphics smoke statically imported D3D12/DXGI, so VKD3D work could
begin before the first program checkpoint. The test now emits its configuration
and creates the Win32 window before dynamically resolving
`D3D12CreateDevice`, `D3D12SerializeRootSignature`, and
`CreateDXGIFactory1`.

The remaining apparent initialization hang was not a failed D3D12 device. The
stage trace proved successful creation of the device, direct queue, swapchain,
root signature, graphics PSO, command list, upload vertex buffer, RTV heap,
back buffers, fence, and event. The original demo loop rendered only when the
Win32 message queue became completely empty; continuous WineHua window traffic
starved that idle callback. The smoke now handles at most 64 messages per
iteration and then renders one frame deterministically.

No `vkDeviceWaitIdle`, sleep, global flush, or equivalent completion
compensation was introduced.

## Three clean-device results

Each run used a new experiment ID, full bundle uninstall/reinstall, a clean
Wine prefix, and the `vkd3d-500k-*` isolated runtime profile.

| Experiment | Result | Frames | Elapsed | FPS |
| --- | --- | ---: | ---: | ---: |
| `vkd3d-500k-f74c040a-graphics-r4` | PASS | 3/3 | 62687 ms | 0.048 |
| `vkd3d-500k-f74c040a-graphics-r5` | PASS | 3/3 | 62843 ms | 0.048 |
| `vkd3d-500k-f74c040a-graphics-r6` | PASS | 3/3 | 62825 ms | 0.048 |

All runs used a 640x480 `DXGI_FORMAT_B8G8R8A8_UNORM` two-buffer swapchain,
`Present(0)`, a direct D3D12 queue, per-frame fence completion, and returned
exit code zero with `status=PASS` JSON.

## Retained log hashes

| Log | SHA-256 |
| --- | --- |
| `vkd3d-graphics-r4-pass-stderr.log` | `96ab2cb6555cc0cfa66b4e65339bc76213ed719d7bc378eb323f2c8baac4f25a` |
| `vkd3d-graphics-r4-pass-virgl.log` | `fad357f25bcf2b3b8ae72cc39baac746d394914d3480235510ba8d9a9845a35a` |
| `vkd3d-graphics-r5-pass-stderr.log` | `cf37a28f5c2652be17213884e406d50f434ad3251921cd81db223a8d15812dd8` |
| `vkd3d-graphics-r5-pass-virgl.log` | `98999d5e098812dbb2df0d18333b6d36b33b618779c12c844cf9d4e0610b8b8c` |
| `vkd3d-graphics-r6-pass-stderr.log` | `44768007ee8e9e816f03c24dedae92e29c67eaa49ca60a38d25db52e425d6eab` |
| `vkd3d-graphics-r6-pass-virgl.log` | `260c0dc888e52222aa8b19035942ec00bd74a6590c282393e225d9034800758f` |

The retained logs are stored outside the repository at `D:\MyProject`.

## Gate status

The three-frame graphics construction, command submission, fence, and DXGI
swapchain API smoke is reproducible on the 910. This qualifies only the Guest
D3D12 execution path. It does not qualify physical presentation, games, or the
1000-frame physical-display gate.

A post-run Host log audit found that the Venus presenter did not attach to the
Wine window. It waited for key `182205397598275` (`surface=67`) and timed out
with `target missing` / `result=-11`, while the actual Wayland window later
created `surface=71` (`key=182205397598279`). Wine maps the Host `-EAGAIN` to
`VK_SUBOPTIMAL_KHR`, so the DXGI microtest can report `PASS` even when the Host
did not publish its frames. The earlier interpretation of `-EAGAIN` as buffer
release pacing was therefore incorrect.

The staged VKD3D experiment selected `winehua.d3d_backend=wined3d`. The UI
previously derived the Host presentation backend only from that D3D11 setting,
selecting `virgl_compositor`, although the experiment-local `d3d12.dll`
presented through Vulkan/Venus. The r7 correction routes only `vkd3d-*`
experiments to `venus_broker_present`; ordinary WineD3D, DXVK Legacy/Modern,
and default desktop launches remain unchanged.

## r7 presenter-routing result

The r7 run used a full uninstall/reinstall, a clean prefix, and the newly built
HAP (`68a94c74855c0c5c5a8f8e7657a9f14017f98a251a842fbd154f46144753422d`).
The launch log records `d3d=wined3d present=venus_broker_present`. The Host
initially observed the expected startup race, then attached the same requested
key after 42,364 microseconds and created a 640x480 FIFO swapchain successfully.

The SurfaceQueue consumer published frame 1 with one signal and zero failures.
The Guest completed 3/3 frames in 1,971 ms (`fps=1.522`) and exited with code
zero. This proves that the corrected route reaches a real Host target; it does
not yet prove three independently displayed frames or the continuous 1000-frame
gate.

Retained r7 logs outside the repository have SHA-256 hashes
`9b2871443667a2b984941903a188ee946f7932ac1012cf99c474c391b501bf0e`
(Host) and `3e91e901decba8c747a72be99d54268ac2e6fccf1838b0aeda139ecc55605be3`
(Guest stderr).

Physical presentation requires a matching Host target, successful presenter
attachment/swapchain setup, `vk_present ret=0`, and observable frame publication.
The 1000-frame gate remains blocked until those conditions pass without sleep,
`vkDeviceWaitIdle`, or global-flush compensation.
