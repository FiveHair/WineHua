# VKD3D PSOStorm specification

PSOStorm is a guest x86-64 Vulkan benchmark for isolating the Venus object
dependency and pipeline creation path used by vkd3d-proton. It is not a
general D3D12 sample and its result must not be treated as proof that a game is
compatible.

## Default workload

The default command is equivalent to:

```text
winehua_vulkan_pso_storm.exe \
  --pipelines 5120 \
  --workers 8 \
  --cache per-pso \
  --lifetime immediate \
  --result pso-storm-result.json
```

Each worker repeatedly performs the following operations:

1. Create a temporary vertex shader module.
2. Create a temporary fragment shader module.
3. Create a temporary pipeline cache when `--cache per-pso` is selected.
4. Create one graphics pipeline with deterministic specialization constants.
5. Return from `vkCreateGraphicsPipelines`.
6. Immediately destroy the temporary cache and shader modules when
   `--lifetime immediate` is selected.

Pipeline layout and render pass objects are created on the main thread before
the workers start. This intentionally reproduces the primary-ring object to
TLS-ring pipeline dependency that the benchmark is designed to measure.

## Options

```text
--pipelines N
--workers N                  (alias: --threads)
--cache none|per-pso|per-thread|shared
--shared-cache-lock on|off
--lifetime immediate|deferred
--vertex PATH
--fragment PATH
--result PATH
```

`shared` plus `--shared-cache-lock on` serializes access to the shared
`VkPipelineCache` as required by Vulkan external synchronization rules.
`shared` plus `--shared-cache-lock off` is diagnostic-only. An unlocked result
must never qualify a Mesa change for integration or be used as a product
default.

## Required matrix

Run the following on the known-safe Mesa baseline and on every candidate
object-dependency implementation:

- Workers: 1, 2, 4, 8.
- Cache modes: `none`, `per-pso`, `per-thread`, `shared` locked.
- Lifetimes: `immediate`, `deferred`.
- `shared` unlocked may be run only to diagnose external synchronization.

The `per-pso` plus `immediate` result is the primary VKD3D-like score.

## Correctness validation

After the workers finish, PSOStorm keeps the final pipeline and renders a
64 by 64 full-screen triangle to an `R8G8B8A8_UNORM` offscreen image. It copies
the image to a host-visible buffer and compares all 16,384 bytes with the
deterministic expected value. It records actual and expected FNV-1a 64-bit
hashes.

The render path uses a fence with a bounded timeout. The test contains no
`vkDeviceWaitIdle`, sleep, or global flush compensation.

## JSON result

Schema version 2 includes at least:

- Device name, vendor ID, device ID, driver version, and Vulkan API version.
- Cache, lifetime, shared-lock, and unlocked-diagnostic modes.
- Requested, attempted, completed, and failed pipeline counts.
- First failure index and `VkResult`.
- Object creation, pipeline workload, render validation, and destruction time.
- Per-PSO p50, p95, p99, and maximum latency.
- Pipelines per second and per-worker results.
- Full-buffer render result plus actual and expected hashes.

A progress heartbeat is written to standard error after every 256 attempted
pipelines so the device runner can distinguish slow progress from a deadlock.

## Pass criteria

A single run passes only when all requested pipelines are successfully
created, the bounded fence completes, all readback bytes match, and the two
hashes agree. A candidate Mesa implementation qualifies only when:

1. The required matrix completes without Venus object lookup failures,
   command-stream errors, assertions, or watchdog termination.
2. The primary workload passes three consecutive times with the same render
   hash.
3. The result does not depend on shared unlocked cache access.
4. Guest workload time approaches the corresponding host measurement. Use a
   guest-to-host ratio target of approximately 1.5 to 2 times if the simple
   shaders make an absolute 15 to 20 second target inappropriate.

A PSOStorm pass authorizes further dependency work; it does not by itself
authorize enabling D3D12 by default or merging the isolated VKD3D branch.
