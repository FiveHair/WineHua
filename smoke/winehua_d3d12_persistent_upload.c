#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FRAMES 600u
#define DEFAULT_WRITE_BYTES (64u * 1024u)
#define DEFAULT_HEAP_BYTES (64u * 1024u * 1024u)
#define MAX_IN_FLIGHT 16u

struct frame_slot {
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    UINT64 fence_value;
};

struct options {
    UINT frames;
    UINT write_bytes;
    UINT64 heap_bytes;
    UINT64 range_stride;
    int rotate_ranges;
    DWORD fence_timeout_ms;
    const char *result_path;
};

struct diagnostics {
    const char *stage;
    UINT frame;
    UINT submitted;
    UINT64 requested_fence;
    UINT64 completed_fence;
};

static void release_unknown(IUnknown **object)
{
    if (*object) {
        IUnknown_Release(*object);
        *object = NULL;
    }
}

static uint64_t qpc_now(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static double qpc_ms(uint64_t begin, uint64_t end)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (double)(end - begin) * 1000.0 / (double)frequency.QuadPart;
}

static uint8_t pattern_byte(UINT frame, UINT byte_index)
{
    uint32_t x = frame * 0x9e3779b9u + byte_index * 0x85ebca6bu;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return (uint8_t)x;
}

static int parse_u32(const char *text, UINT *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (!text[0] || !end || *end || parsed == 0 || parsed > UINT32_MAX)
        return 0;
    *value = (UINT)parsed;
    return 1;
}

static int parse_u64(const char *text, UINT64 *value)
{
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (!text[0] || !end || *end || parsed == 0)
        return 0;
    *value = (UINT64)parsed;
    return 1;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int i;
    options->frames = DEFAULT_FRAMES;
    options->write_bytes = DEFAULT_WRITE_BYTES;
    options->heap_bytes = DEFAULT_HEAP_BYTES;
    options->range_stride = DEFAULT_WRITE_BYTES;
    options->rotate_ranges = 1;
    options->fence_timeout_ms = 120000;
    options->result_path = "persistent-upload-result.json";

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &options->frames))
                return 0;
        } else if (!strcmp(argv[i], "--write-bytes") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &options->write_bytes))
                return 0;
        } else if (!strcmp(argv[i], "--heap-bytes") && i + 1 < argc) {
            if (!parse_u64(argv[++i], &options->heap_bytes))
                return 0;
        } else if (!strcmp(argv[i], "--range-stride") && i + 1 < argc) {
            if (!parse_u64(argv[++i], &options->range_stride))
                return 0;
        } else if (!strcmp(argv[i], "--range-mode") && i + 1 < argc) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "same"))
                options->rotate_ranges = 0;
            else if (!strcmp(mode, "rotate"))
                options->rotate_ranges = 1;
            else
                return 0;
        } else if (!strcmp(argv[i], "--fence-timeout-ms") && i + 1 < argc) {
            UINT timeout;
            if (!parse_u32(argv[++i], &timeout))
                return 0;
            options->fence_timeout_ms = timeout;
        } else if (!strcmp(argv[i], "--result") && i + 1 < argc) {
            options->result_path = argv[++i];
        } else {
            return 0;
        }
    }

    if (options->write_bytes > options->heap_bytes ||
        !options->range_stride || options->range_stride > options->heap_bytes ||
        options->range_stride < options->write_bytes)
        return 0;
    return 1;
}

static HRESULT wait_for_fence(ID3D12Fence *fence, UINT64 value, HANDLE event,
        DWORD timeout_ms, struct diagnostics *diagnostics)
{
    HRESULT hr;
    diagnostics->requested_fence = value;
    diagnostics->completed_fence = ID3D12Fence_GetCompletedValue(fence);
    if (diagnostics->completed_fence >= value)
        return S_OK;
    hr = ID3D12Fence_SetEventOnCompletion(fence, value, event);
    if (FAILED(hr))
        return hr;
    if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
        diagnostics->completed_fence = ID3D12Fence_GetCompletedValue(fence);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    diagnostics->completed_fence = ID3D12Fence_GetCompletedValue(fence);
    return S_OK;
}

static HRESULT create_buffer(ID3D12Device *device, UINT64 size,
        D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state,
        ID3D12Resource **resource)
{
    D3D12_HEAP_PROPERTIES heap;
    D3D12_RESOURCE_DESC desc;
    memset(&heap, 0, sizeof(heap));
    memset(&desc, 0, sizeof(desc));

    heap.Type = heap_type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    return ID3D12Device_CreateCommittedResource(device, &heap,
            D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)resource);
}

static void write_result(const struct options *options, const char *status,
        HRESULT hr, double submit_ms, UINT64 mismatches, UINT64 first_mismatch,
        const struct diagnostics *diagnostics)
{
    FILE *file = fopen(options->result_path, "wb");
    const double submits_per_second = submit_ms > 0.0
        ? (double)options->frames * 1000.0 / submit_ms : 0.0;
    const UINT64 logical_bytes = (UINT64)options->frames * options->write_bytes;
    if (!file)
        return;
    fprintf(file,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"test\": \"d3d12-persistent-upload\",\n"
        "  \"status\": \"%s\",\n"
        "  \"hresult\": \"0x%08lx\",\n"
        "  \"frames\": %u,\n"
        "  \"inFlight\": %u,\n"
        "  \"heapBytes\": %llu,\n"
        "  \"writeBytesPerFrame\": %u,\n"
        "  \"rangeStride\": %llu,\n"
        "  \"rangeMode\": \"%s\",\n"
        "  \"logicalBytesWritten\": %llu,\n"
        "  \"submitMilliseconds\": %.3f,\n"
        "  \"submitsPerSecond\": %.3f,\n"
        "  \"mismatchCount\": %llu,\n"
        "  \"firstMismatch\": %lld,\n"
        "  \"failure\": {\"stage\": \"%s\", \"frame\": %u, "
        "\"submitted\": %u, \"requestedFence\": %llu, "
        "\"completedFence\": %llu, \"timeoutMilliseconds\": %lu}\n"
        "}\n",
        status, (unsigned long)hr, options->frames, MAX_IN_FLIGHT,
        (unsigned long long)options->heap_bytes, options->write_bytes,
        (unsigned long long)options->range_stride,
        options->rotate_ranges ? "rotate" : "same",
        (unsigned long long)logical_bytes, submit_ms, submits_per_second,
        (unsigned long long)mismatches,
        mismatches ? (long long)first_mismatch : -1ll,
        diagnostics->stage, diagnostics->frame, diagnostics->submitted,
        (unsigned long long)diagnostics->requested_fence,
        (unsigned long long)diagnostics->completed_fence,
        (unsigned long)options->fence_timeout_ms);
    fclose(file);
}

int main(int argc, char **argv)
{
    struct options options;
    struct frame_slot slots[MAX_IN_FLIGHT];
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Fence *fence = NULL;
    ID3D12Resource *upload = NULL;
    ID3D12Resource *gpu = NULL;
    ID3D12Resource *readback = NULL;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    D3D12_RANGE empty_range = {0, 0};
    D3D12_RANGE read_range;
    HANDLE fence_event = NULL;
    uint8_t *upload_map = NULL;
    uint8_t *readback_map = NULL;
    UINT64 next_fence = 1;
    UINT64 mismatches = 0;
    UINT64 first_mismatch = 0;
    uint64_t submit_begin = 0;
    uint64_t submit_end = 0;
    HRESULT hr = E_FAIL;
    struct diagnostics diagnostics = {"initialization", 0, 0, 0, 0};
    UINT frame;
    UINT i;

    memset(slots, 0, sizeof(slots));
    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr, "usage: %s [--frames N] [--write-bytes N] "
                "[--heap-bytes N] [--range-stride N] [--range-mode same|rotate] "
                "[--fence-timeout-ms N] [--result PATH]\n", argv[0]);
        return 2;
    }

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
            &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr))
        goto done;

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = ID3D12Device_CreateCommandQueue(device, &queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    if (FAILED(hr))
        goto done;

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    if (FAILED(hr))
        goto done;
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!fence_event) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    hr = create_buffer(device, options.heap_bytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
    if (FAILED(hr))
        goto done;
    hr = create_buffer(device, options.write_bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, &gpu);
    if (FAILED(hr))
        goto done;
    hr = create_buffer(device, options.write_bytes, D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (FAILED(hr))
        goto done;

    hr = ID3D12Resource_Map(upload, 0, &empty_range, (void **)&upload_map);
    if (FAILED(hr))
        goto done;

    for (i = 0; i < MAX_IN_FLIGHT; i++) {
        hr = ID3D12Device_CreateCommandAllocator(device,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&slots[i].allocator);
        if (FAILED(hr))
            goto done;
        hr = ID3D12Device_CreateCommandList(device, 0,
                D3D12_COMMAND_LIST_TYPE_DIRECT, slots[i].allocator, NULL,
                &IID_ID3D12GraphicsCommandList, (void **)&slots[i].list);
        if (FAILED(hr))
            goto done;
        hr = ID3D12GraphicsCommandList_Close(slots[i].list);
        if (FAILED(hr))
            goto done;
    }

    submit_begin = qpc_now();
    for (frame = 0; frame < options.frames; frame++) {
        struct frame_slot *slot = &slots[frame % MAX_IN_FLIGHT];
        const UINT64 range_count = options.heap_bytes / options.range_stride;
        const UINT64 range_index = options.rotate_ranges ?
            ((UINT64)frame % range_count) : 0;
        const UINT64 source_offset = range_index * options.range_stride;
        ID3D12CommandList *lists[1];
        UINT byte_index;

        if (slot->fence_value) {
            diagnostics.stage = "slot-reuse-wait";
            diagnostics.frame = frame;
            hr = wait_for_fence(fence, slot->fence_value, fence_event,
                    options.fence_timeout_ms, &diagnostics);
            if (FAILED(hr))
                goto done;
        }

        for (byte_index = 0; byte_index < options.write_bytes; byte_index++)
            upload_map[source_offset + byte_index] = pattern_byte(frame, byte_index);

        hr = ID3D12CommandAllocator_Reset(slot->allocator);
        if (FAILED(hr))
            goto done;
        hr = ID3D12GraphicsCommandList_Reset(slot->list, slot->allocator, NULL);
        if (FAILED(hr))
            goto done;
        ID3D12GraphicsCommandList_CopyBufferRegion(slot->list, gpu, 0,
                upload, source_offset, options.write_bytes);
        hr = ID3D12GraphicsCommandList_Close(slot->list);
        if (FAILED(hr))
            goto done;
        lists[0] = (ID3D12CommandList *)slot->list;
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        slot->fence_value = next_fence++;
        hr = ID3D12CommandQueue_Signal(queue, fence, slot->fence_value);
        if (FAILED(hr))
            goto done;
        diagnostics.submitted = frame + 1;
    }

    diagnostics.stage = "final-submit-wait";
    diagnostics.frame = options.frames;
    hr = wait_for_fence(fence, next_fence - 1, fence_event,
            options.fence_timeout_ms, &diagnostics);
    if (FAILED(hr))
        goto done;
    submit_end = qpc_now();

    {
        struct frame_slot *slot = &slots[0];
        D3D12_RESOURCE_BARRIER barrier;
        ID3D12CommandList *lists[1];
        diagnostics.stage = "readback-slot-wait";
        hr = wait_for_fence(fence, slot->fence_value, fence_event,
                options.fence_timeout_ms, &diagnostics);
        if (FAILED(hr))
            goto done;
        hr = ID3D12CommandAllocator_Reset(slot->allocator);
        if (FAILED(hr))
            goto done;
        hr = ID3D12GraphicsCommandList_Reset(slot->list, slot->allocator, NULL);
        if (FAILED(hr))
            goto done;
        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = gpu;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(slot->list, 1, &barrier);
        ID3D12GraphicsCommandList_CopyBufferRegion(slot->list, readback, 0,
                gpu, 0, options.write_bytes);
        hr = ID3D12GraphicsCommandList_Close(slot->list);
        if (FAILED(hr))
            goto done;
        lists[0] = (ID3D12CommandList *)slot->list;
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        hr = ID3D12CommandQueue_Signal(queue, fence, next_fence);
        if (FAILED(hr))
            goto done;
        diagnostics.stage = "readback-wait";
        hr = wait_for_fence(fence, next_fence, fence_event,
                options.fence_timeout_ms, &diagnostics);
        if (FAILED(hr))
            goto done;
    }

    read_range.Begin = 0;
    read_range.End = options.write_bytes;
    hr = ID3D12Resource_Map(readback, 0, &read_range, (void **)&readback_map);
    if (FAILED(hr))
        goto done;
    for (i = 0; i < options.write_bytes; i++) {
        if (readback_map[i] != pattern_byte(options.frames - 1, i)) {
            if (!mismatches)
                first_mismatch = i;
            mismatches++;
        }
    }
    ID3D12Resource_Unmap(readback, 0, &empty_range);
    readback_map = NULL;
    hr = mismatches ? E_FAIL : S_OK;
    diagnostics.stage = SUCCEEDED(hr) ? "complete" : "readback-validate";

done:
    if (upload_map)
        ID3D12Resource_Unmap(upload, 0, NULL);
    write_result(&options, SUCCEEDED(hr) ? "PASS" : "FAIL", hr,
            submit_end > submit_begin ? qpc_ms(submit_begin, submit_end) : 0.0,
            mismatches, first_mismatch, &diagnostics);
    fprintf(stdout,
            "persistent-upload status=%s hr=0x%08lx frames=%u "
            "write_bytes=%u heap_bytes=%llu submit_ms=%.3f mismatches=%llu "
            "stage=%s frame=%u submitted=%u requested_fence=%llu completed_fence=%llu\n",
            SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr,
            options.frames, options.write_bytes,
            (unsigned long long)options.heap_bytes,
            submit_end > submit_begin ? qpc_ms(submit_begin, submit_end) : 0.0,
            (unsigned long long)mismatches, diagnostics.stage,
            diagnostics.frame, diagnostics.submitted,
            (unsigned long long)diagnostics.requested_fence,
            (unsigned long long)diagnostics.completed_fence);

    for (i = 0; i < MAX_IN_FLIGHT; i++) {
        release_unknown((IUnknown **)&slots[i].list);
        release_unknown((IUnknown **)&slots[i].allocator);
    }
    release_unknown((IUnknown **)&readback);
    release_unknown((IUnknown **)&gpu);
    release_unknown((IUnknown **)&upload);
    release_unknown((IUnknown **)&fence);
    release_unknown((IUnknown **)&queue);
    release_unknown((IUnknown **)&device);
    if (fence_event)
        CloseHandle(fence_event);
    return SUCCEEDED(hr) ? 0 : 1;
}
