# VKD3D Limited 500K Adaptation Plan

> Updated: 2026-08-03

> Status: qualification only. No vkd3d-proton DLL is packaged, loaded, or
> selected by the product runtime.

## Objective

Investigate a default-off vkd3d-proton 2.6 profile for devices whose Vulkan
driver supports a 500,000-entry bindless resource view array, without claiming
upstream vkd3d-proton compatibility or changing the validated DXVK paths.

The upstream-compatible profile remains unchanged: resource-view descriptor
arrays need 1,000,000 entries. The experimental profile must never fabricate a
Vulkan feature or limit, and it must never make a device eligible for Gate B.

## Current 920 Evidence

The four-layer audit from `regression-20260803-161509` observes these values
on Maleoon 920:

| Layer | Descriptor indexing | Resource UAB per-stage / per-set | Sampler UAB per-stage / per-set |
| --- | --- | --- | --- |
| Host Vulkan | yes | 500,000 / 500,000 | 500,000 / 500,000 |
| Guest Venus | no | 500,000 / 500,000 | 500,000 / 500,000 |
| Wine Vulkan x64 | evidence incomplete | 500,000 / 500,000 | 500,000 / 500,000 |
| Wine PE x64 | no | 500,000 / 500,000 | 500,000 / 500,000 |

`maxPerStageUpdateAfterBindResources` is 2,000,016 and
`maxUpdateAfterBindDescriptorsInAllPools` is `UINT32_MAX`. The numeric limit
is therefore potentially useful, but the Guest and Wine PE
`descriptorIndexing=false` results block any runtime experiment today. This is
a Venus capability path problem; modifying vkd3d-proton alone cannot bypass
it.

## Profile Definitions

| Profile | Resource view limits | Sampler limits | Input attachments | Gate status |
| --- | --- | --- | --- | --- |
| Upstream 2.6 / 2.8 / 2.9 | >= 1,000,000 per-stage and per-set | >= 2,048 | Informational | May qualify for Gate B only after all normal gates pass. |
| Experimental 2.6 limited 500K | >= 500,000 per-stage and per-set | >= 2,048 | Informational | Default off; evidence candidate only; never directly qualifies Gate B. |

The resource limits are Sampled Images, Storage Images, and Storage Buffers.
The vkd3d 2.6 bindless path creates a 1,000,000-entry resource layout and a
2,048-entry sampler layout. `InputAttachments=8` is not a D3D12 shader-visible
resource heap limit. A title that requests a 1,000,000-entry resource heap
cannot be silently truncated by the experimental profile.

## Implementation Sequence

1. Make Guest Venus and Wine PE expose the required descriptor-indexing feature
   bits truthfully. Re-run the four-layer capability audit and require a stable
   capability hash before building a runtime candidate.
2. Create `feature/vkd3d-capability-probe` in an isolated vkd3d-proton checkout
   from the audited 2.6 revision. If it is introduced as a submodule, keep the
   main repository gitlink pointed only at a pushed submodule commit.
3. Add a separately named, default-off 500K build profile. Reduce every
   descriptor-layout, variable-count allocation, descriptor-heap maximum,
   host mapping, and reported D3D12 view-heap limit coherently. Keep sampler
   heaps at 2,048.
4. Reject a `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` shader-visible heap over
   500,000 with a defined D3D12 error. Do not clamp its size or rewrite its GPU
   descriptor addresses.
5. Build only the isolated x64 vkd3d DLLs. Record upstream tag, fork commit,
   DLL version, SHA-256, loader path, and default-disabled activation setting.
6. Pass a real x64 D3D12 microtest: device, queue, fence, upload/default/
   readback copy, byte-for-byte verification, descriptor writes, and a 500K
   shader-visible resource heap. Run it three times without `vkDeviceWaitIdle`,
   sleeps, or global flushes.
7. Add descriptor-heap telemetry for each game: requested capacity and highest
   descriptor index written. A game that creates a 1M heap remains unsupported
   even if its observed writes are sparse, unless a future vkd3d design can
   preserve its GPU descriptor-handle semantics.

## Safety Boundaries

- Do not modify Host, Venus, Wine, or PE probes to report larger limits or
  enabled features than they expose.
- Do not change `master`, the validated DXVK Legacy path, or the Modern 2.6
  profile.
- Do not enable D3D12 by default and do not treat a built DLL as a functional
  integration result.
- Capability hash, Host driver, Mesa/Venus, or Wine Vulkan changes invalidate
  the audit and require Gate A again.

## Current Next Gate

The immediate engineering task is a Guest Venus descriptor-indexing transport
audit. Until the Guest and Wine PE feature chains report the required bits as
enabled, the experimental 500K profile remains `UNSUPPORTED` and no vkd3d DLL
is introduced.
