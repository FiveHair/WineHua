/*
 * WineHua DXVK 2.6.2 Vulkan requirements probe.
 *
 * This is a Win32 program on purpose. It validates the exact path DXVK sees:
 * Windows Vulkan -> winevulkan -> x86_64 Vulkan loader -> Venus -> Host Vulkan.
 * It does not load DXVK and therefore separates transport qualification from
 * DXVK compatibility-layer work.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct probe_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    ULONGLONG started_ms;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures core;
    uint32_t queue_family;
    BOOL fallback_detected;

    BOOL api13;
    BOOL robust_buffer_access2;
    BOOL robust_image_access2;
    BOOL null_descriptor;
    BOOL synchronization2;
    BOOL dynamic_rendering;
    BOOL maintenance4;
    BOOL timeline_semaphore;
    BOOL buffer_device_address;
    BOOL descriptor_indexing;
    BOOL transform_feedback;
    BOOL geometry_streams;

    BOOL dual_src_blend;
    BOOL multi_viewport;
    BOOL texture_compression_bc;
    BOOL rgba8_snorm_color_attachment;
    BOOL d24s8_sampled;
    BOOL d24s8_depth_stencil_attachment;
    BOOL bc1;
    BOOL bc2;
    BOOL bc3;
    BOOL bc4;
    BOOL bc5;
    BOOL bc6;
    BOOL bc7;

    BOOL transport_features_ready;
    BOOL transport_device_create_ok;
    VkResult transport_device_create_result;

    BOOL timeline_round_trip_ok;
    VkResult timeline_semaphore_create_result;
    VkResult timeline_submit_result;
    VkResult timeline_wait_result;
    VkResult timeline_counter_result;
    uint64_t timeline_observed_value;
};

static ULONGLONG now_ms(void)
{
    FILETIME file_time;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart / 10000ULL - 11644473600000ULL;
}

static const char *argument_value(int argc, char **argv, const char *name, const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!lstrcmpiA(argv[i], name)) return argv[i + 1];
    return fallback;
}

static void json_safe_copy(char *output, size_t output_size, const char *input)
{
    size_t written = 0;
    if (!output_size) return;
    while (input && *input && written + 1 < output_size) {
        unsigned char ch = (unsigned char)*input++;
        if (ch == '"' || ch == '\\' || ch < 0x20 || ch > 0x7e) ch = '_';
        output[written++] = (char)ch;
    }
    output[written] = 0;
}

static void version_text(uint32_t version, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

static BOOL ensure_parent(const char *path)
{
    char copy[MAX_PATH];
    char *cursor;
    lstrcpynA(copy, path ? path : "", sizeof(copy));
    for (cursor = copy; *cursor; ++cursor) {
        if ((*cursor == '\\' || *cursor == '/') && cursor > copy + 2) {
            char saved = *cursor;
            *cursor = 0;
            if (!CreateDirectoryA(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return FALSE;
            *cursor = saved;
        }
    }
    return TRUE;
}

static const char *bool_text(BOOL value)
{
    return value ? "true" : "false";
}

static void write_progress(const struct probe_state *state, const char *stage)
{
    char progress[512];
    char temporary[1024];
    const char *vn_perf = getenv("VN_PERF");
    const BOOL no_semaphore_feedback =
        vn_perf && strstr(vn_perf, "no_semaphore_feedback");
    FILE *file;

    if (!state->result_path || !state->result_path[0]) return;
    snprintf(progress, sizeof(progress), "%s.progress", state->result_path);
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", progress,
             (unsigned long)GetCurrentProcessId());
    if (!ensure_parent(progress)) return;
    file = fopen(temporary, "wb");
    if (!file) return;
    fprintf(file, "{\"pid\":%lu,\"stage\":\"%s\",\"timestampMs\":%llu,"
            "\"vnPerfNoSemaphoreFeedback\":%s}\n",
            (unsigned long)GetCurrentProcessId(), stage ? stage : "unknown",
            (unsigned long long)now_ms(), bool_text(no_semaphore_feedback));
    fflush(file);
    fclose(file);
    MoveFileExA(temporary, progress,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static const char *policy_text(BOOL native_supported, const char *missing_policy)
{
    return native_supported ? "native" : missing_policy;
}

static void write_result(const struct probe_state *state, const char *status,
                         const char *message)
{
    char temporary[MAX_PATH];
    char loader_version[32];
    char device_version[32];
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char safe_message[256];
    FILE *file;

    if (!state->result_path || !state->result_path[0]) return;
    if (!ensure_parent(state->result_path)) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", state->result_path,
             (unsigned long)GetCurrentProcessId());
    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    json_safe_copy(device_name, sizeof(device_name), state->properties.deviceName);
    json_safe_copy(safe_message, sizeof(safe_message), message ? message : "");
    file = fopen(temporary, "wb");
    if (!file) return;

    fprintf(file,
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"runId\": \"%s\",\n"
            "  \"testId\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"stage\": \"dxvk26-requirements\",\n"
            "  \"message\": \"%s\",\n"
            "  \"pid\": %lu,\n"
            "  \"architecture\": {\"peArchitecture\": \"%s\","
            "\"wineUnixArchitecture\": \"%s\","
            "\"vulkanLoaderArchitecture\": \"%s\","
            "\"venusIcdArchitecture\": \"%s\","
            "\"hostArchitecture\": \"%s\","
            "\"wow64ThunkEnabled\": %s,\"box64Enabled\": %s},\n"
            "  \"capabilities\": {\"loaderApiVersion\": \"%s\","
            "\"deviceApiVersion\": \"%s\",\"deviceName\": \"%s\","
            "\"vendorId\":%u,\"deviceId\":%u,\"driverVersion\":%u,"
            "\"graphicsQueueFamily\":%u,\"pushConstantBytes\":%u},\n"
            "  \"dxvk262\": {\n"
            "    \"transport\": {\"api13\":%s,\"coreRobustBufferAccess\":%s,"
            "\"robustBufferAccess2\":%s,\"robustImageAccess2\":%s,"
            "\"nullDescriptor\":%s,\"synchronization2\":%s,"
            "\"dynamicRendering\":%s,\"maintenance4\":%s,"
            "\"deviceCreateAttempted\":%s,\"deviceCreateResult\":%d,"
            "\"passed\":%s},\n"
            "    \"timelineRoundTrip\": {\"feature\":%s,"
            "\"semaphoreCreateResult\":%d,\"queueSubmitResult\":%d,"
            "\"waitResult\":%d,\"counterResult\":%d,"
            "\"observedValue\":%llu,\"passed\":%s},\n"
            "    \"vulkan12\": {\"timelineSemaphore\":%s,"
            "\"bufferDeviceAddress\":%s,\"descriptorIndexing\":%s},\n"
            "    \"d3d11Features\": {\"geometryShader\":%s,"
            "\"tessellationShader\":%s,\"multiDrawIndirect\":%s,"
            "\"dualSrcBlend\":\"%s\",\"multiViewport\":\"%s\","
            "\"transformFeedback\":\"%s\",\"geometryStreams\":\"%s\"},\n"
            "    \"formats\": {\"bc1\":%s,\"bc2\":%s,\"bc3\":%s,"
            "\"bc4\":%s,\"bc5\":%s,\"bc6\":%s,\"bc7\":%s,"
            "\"rgba8SnormColorAttachment\":%s,\"d24s8Sampled\":%s,"
            "\"d24s8DepthStencilAttachment\":%s},\n"
            "    \"compatibility\": {\"bc\":\"%s\","
            "\"dualSrcBlend\":\"%s\",\"multiViewport\":\"%s\","
            "\"geometryStreams\":\"%s\"},\n"
            "    \"eligibility\": {\"transport\":\"%s\","
            "\"bringup\":\"%s\"}\n"
            "  },\n"
            "  \"metrics\": {\"cpuReadBytes\":0,\"cpuUploadBytes\":0,"
            "\"gpuCopyCount\":0,\"queueSubmitCount\":0,"
            "\"fallbackDetected\":%s,\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, safe_message,
            (unsigned long)GetCurrentProcessId(),
#ifdef _WIN64
            "x86_64", "x86_64",
#else
            "x86", "x86_64",
#endif
            getenv("WINEHUA_VULKAN_LOADER_ARCH") ? getenv("WINEHUA_VULKAN_LOADER_ARCH") : "unknown",
            getenv("WINEHUA_VENUS_ICD_ARCH") ? getenv("WINEHUA_VENUS_ICD_ARCH") : "unknown",
            getenv("WINEHUA_HOST_ARCH") ? getenv("WINEHUA_HOST_ARCH") : "unknown",
#ifdef _WIN64
            "false",
#else
            "true",
#endif
            getenv("USE_LIBBOX64") && getenv("USE_LIBBOX64")[0] == '1' ? "true" : "false",
            loader_version, device_version, device_name, state->properties.vendorID,
            state->properties.deviceID, state->properties.driverVersion, state->queue_family,
            state->properties.limits.maxPushConstantsSize,
            bool_text(state->api13), bool_text(state->core.robustBufferAccess),
            bool_text(state->robust_buffer_access2), bool_text(state->robust_image_access2),
            bool_text(state->null_descriptor), bool_text(state->synchronization2),
            bool_text(state->dynamic_rendering), bool_text(state->maintenance4),
            bool_text(state->transport_features_ready), (int)state->transport_device_create_result,
            bool_text(state->transport_device_create_ok), bool_text(state->timeline_semaphore),
            (int)state->timeline_semaphore_create_result, (int)state->timeline_submit_result,
            (int)state->timeline_wait_result, (int)state->timeline_counter_result,
            (unsigned long long)state->timeline_observed_value,
            bool_text(state->timeline_round_trip_ok), bool_text(state->timeline_semaphore),
            bool_text(state->buffer_device_address), bool_text(state->descriptor_indexing),
            bool_text(state->core.geometryShader), bool_text(state->core.tessellationShader),
            bool_text(state->core.multiDrawIndirect),
            policy_text(state->dual_src_blend, "emulated"),
            policy_text(state->multi_viewport, "withheld"),
            policy_text(state->transform_feedback, "native-unavailable"),
            policy_text(state->geometry_streams, "withheld"),
            bool_text(state->bc1), bool_text(state->bc2), bool_text(state->bc3),
            bool_text(state->bc4), bool_text(state->bc5), bool_text(state->bc6),
            bool_text(state->bc7), bool_text(state->rgba8_snorm_color_attachment),
            bool_text(state->d24s8_sampled), bool_text(state->d24s8_depth_stencil_attachment),
            state->texture_compression_bc ? "native" : "decode-required",
            policy_text(state->dual_src_blend, "fallback-required"),
            policy_text(state->multi_viewport, "withhold-required"),
            policy_text(state->geometry_streams, "withhold-required"),
            state->transport_device_create_ok && state->timeline_round_trip_ok ? "PASS" : "FAIL",
            state->transport_device_create_ok && state->timeline_round_trip_ok
                ? "D3D11_FEATURE_PROBE_PENDING" : "BLOCKED",
            bool_text(state->fallback_detected),
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fclose(file);
    MoveFileExA(temporary, state->result_path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static BOOL has_extension(const VkExtensionProperties *extensions, uint32_t count,
                          const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return TRUE;
    return FALSE;
}

static BOOL format_supports(VkPhysicalDevice physical, VkFormat format,
                            VkFormatFeatureFlags features)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

static BOOL query_requirements(VkPhysicalDevice physical, struct probe_state *state)
{
    uint32_t count = 0;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures2 features2 = { 0 };
    VkPhysicalDeviceVulkan12Features vk12 = { 0 };
    VkPhysicalDeviceVulkan13Features vk13 = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = { 0 };
    void **tail = &features2.pNext;
    BOOL api12 = state->properties.apiVersion >= VK_API_VERSION_1_2;
    BOOL has_robustness2;
    BOOL has_transform_feedback;

    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    transform_feedback.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;

    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS)
        return FALSE;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return FALSE;
    if (count && vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) != VK_SUCCESS) {
        free(extensions);
        return FALSE;
    }
    has_robustness2 = has_extension(extensions, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    has_transform_feedback = has_extension(extensions, count,
                                            VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
#define APPEND_FEATURE(feature, enabled) do { \
    if (enabled) { *tail = &(feature); tail = &(feature).pNext; } \
} while (0)
    APPEND_FEATURE(vk12, api12);
    APPEND_FEATURE(vk13, state->api13);
    APPEND_FEATURE(robustness2, has_robustness2);
    APPEND_FEATURE(transform_feedback, has_transform_feedback);
#undef APPEND_FEATURE
    vkGetPhysicalDeviceFeatures2(physical, &features2);

    state->robust_buffer_access2 = has_robustness2 && robustness2.robustBufferAccess2;
    state->robust_image_access2 = has_robustness2 && robustness2.robustImageAccess2;
    state->null_descriptor = has_robustness2 && robustness2.nullDescriptor;
    state->synchronization2 = state->api13 && vk13.synchronization2;
    state->dynamic_rendering = state->api13 && vk13.dynamicRendering;
    state->maintenance4 = state->api13 && vk13.maintenance4;
    state->timeline_semaphore = api12 && vk12.timelineSemaphore;
    state->buffer_device_address = api12 && vk12.bufferDeviceAddress;
    state->descriptor_indexing = api12 && vk12.descriptorIndexing;
    state->transform_feedback = has_transform_feedback && transform_feedback.transformFeedback;
    state->geometry_streams = has_transform_feedback && transform_feedback.geometryStreams;
    state->dual_src_blend = state->core.dualSrcBlend;
    state->multi_viewport = state->core.multiViewport;
    state->texture_compression_bc = state->core.textureCompressionBC;
    state->bc1 = format_supports(physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc2 = format_supports(physical, VK_FORMAT_BC2_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc3 = format_supports(physical, VK_FORMAT_BC3_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc4 = format_supports(physical, VK_FORMAT_BC4_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc5 = format_supports(physical, VK_FORMAT_BC5_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc6 = format_supports(physical, VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc7 = format_supports(physical, VK_FORMAT_BC7_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->rgba8_snorm_color_attachment = format_supports(
        physical, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    state->d24s8_sampled = format_supports(physical, VK_FORMAT_D24_UNORM_S8_UINT,
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->d24s8_depth_stencil_attachment = format_supports(
        physical, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    state->transport_features_ready = state->api13 && state->core.robustBufferAccess &&
        state->robust_buffer_access2 && state->robust_image_access2 &&
        state->null_descriptor && state->synchronization2 && state->dynamic_rendering &&
        state->maintenance4 && state->timeline_semaphore;
    free(extensions);
    return TRUE;
}

static VkResult create_transport_device(VkPhysicalDevice physical,
                                        struct probe_state *state)
{
    const char *extensions[] = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME };
    float priority = 1.0f;
    VkPhysicalDeviceVulkan12Features vk12 = { 0 };
    VkPhysicalDeviceVulkan13Features vk13 = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkDeviceQueueCreateInfo queue = { 0 };
    VkDeviceCreateInfo create = { 0 };
    VkDevice device = VK_NULL_HANDLE;
    VkQueue device_queue = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo semaphore_type = { 0 };
    VkSemaphoreCreateInfo semaphore_create = { 0 };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_create = { 0 };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_alloc = { 0 };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo command_begin = { 0 };
    VkCommandBufferSubmitInfo command_info = { 0 };
    VkSemaphoreSubmitInfo signal = { 0 };
    VkSubmitInfo2 submit = { 0 };
    VkSemaphoreWaitInfo wait = { 0 };
    uint64_t signal_value = 1;
    VkResult result;

    if (!state->transport_features_ready) return VK_ERROR_FEATURE_NOT_PRESENT;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    vk12.timelineSemaphore = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;
    vk13.dynamicRendering = VK_TRUE;
    vk13.maintenance4 = VK_TRUE;
    robustness2.robustBufferAccess2 = VK_TRUE;
    robustness2.nullDescriptor = VK_TRUE;
    robustness2.robustImageAccess2 = VK_TRUE;
    vk13.pNext = &vk12;
    vk12.pNext = &robustness2;
    queue.queueFamilyIndex = state->queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    create.pNext = &vk13;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = 1;
    create.ppEnabledExtensionNames = extensions;
    write_progress(state, "before-device-create");
    result = vkCreateDevice(physical, &create, NULL, &device);
    state->transport_device_create_result = result;
    if (result != VK_SUCCESS)
        return result;
    write_progress(state, "device-created");

    vkGetDeviceQueue(device, state->queue_family, 0, &device_queue);
    semaphore_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphore_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_create.pNext = &semaphore_type;
    write_progress(state, "before-semaphore-create");
    state->timeline_semaphore_create_result =
        vkCreateSemaphore(device, &semaphore_create, NULL, &semaphore);
    if (state->timeline_semaphore_create_result != VK_SUCCESS) {
        result = state->timeline_semaphore_create_result;
        goto cleanup;
    }
    write_progress(state, "semaphore-created");

    /* Keep one real command in the submission.  DXVK never relies on an empty
     * queue submit, and some Venus implementations do not retire an empty
     * Submit2 timeline signal through the same feedback path. */
    pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_create.queueFamilyIndex = state->queue_family;
    result = vkCreateCommandPool(device, &pool_create, NULL, &command_pool);
    if (result != VK_SUCCESS)
        goto cleanup;
    command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_alloc.commandPool = command_pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &command_alloc, &command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    result = vkBeginCommandBuffer(command_buffer, &command_begin);
    if (result != VK_SUCCESS)
        goto cleanup;
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;

    /* DXVK 2.6 submits through Vulkan 1.3 synchronization2.  Qualify the
     * exact QueueSubmit2 timeline path instead of the legacy submit pNext
     * encoding, which DXVK does not use. */
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = semaphore;
    signal.value = signal_value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = command_buffer;
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    write_progress(state, "before-queue-submit");
    state->timeline_submit_result = vkQueueSubmit2(device_queue, 1, &submit, VK_NULL_HANDLE);
    if (state->timeline_submit_result != VK_SUCCESS) {
        result = state->timeline_submit_result;
        goto cleanup;
    }
    write_progress(state, "queue-submitted");

    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore;
    wait.pValues = &signal_value;
    write_progress(state, "before-semaphore-wait");
    state->timeline_wait_result = vkWaitSemaphores(device, &wait, 5000000000ULL);
    if (state->timeline_wait_result != VK_SUCCESS) {
        result = state->timeline_wait_result;
        goto cleanup;
    }
    write_progress(state, "semaphore-wait-complete");

    write_progress(state, "before-counter-query");
    state->timeline_counter_result =
        vkGetSemaphoreCounterValue(device, semaphore, &state->timeline_observed_value);
    state->timeline_round_trip_ok =
        state->timeline_counter_result == VK_SUCCESS &&
        state->timeline_observed_value >= signal_value;
    result = state->timeline_round_trip_ok ? VK_SUCCESS : VK_NOT_READY;

cleanup:
    if (command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, command_pool, NULL);
    if (semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(device, semaphore, NULL);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, NULL);
    write_progress(state, "device-destroyed");
    return result;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    VkApplicationInfo application = { 0 };
    VkInstanceCreateInfo instance_info = { 0 };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 0;
    uint32_t i;
    VkResult result;
    const char *failure = "unknown failure";
    int exit_code = 1;

    memset(&state, 0, sizeof(state));
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id", "dxvk26-requirements");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.started_ms = now_ms();
    state.queue_family = UINT32_MAX;
    state.loader_api = VK_API_VERSION_1_0;
    state.transport_device_create_result = VK_NOT_READY;
    state.timeline_semaphore_create_result = VK_NOT_READY;
    state.timeline_submit_result = VK_NOT_READY;
    state.timeline_wait_result = VK_NOT_READY;
    state.timeline_counter_result = VK_NOT_READY;
    {
        PFN_vkEnumerateInstanceVersion enumerate_version =
            (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                    "vkEnumerateInstanceVersion");
        if (enumerate_version) enumerate_version(&state.loader_api);
    }
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    application.pApplicationName = "winehua_dxvk26_requirements";
    application.applicationVersion = 1;
    application.pEngineName = "WineHua";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_3;
    instance_info.pApplicationInfo = &application;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        write_result(&state, "UNSUPPORTED", "vkCreateInstance Vulkan 1.3 failed");
        return 3;
    }
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) { failure = "no Vulkan physical device"; goto cleanup; }
    {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) { failure = "physical device allocation failed"; goto cleanup; }
        result = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (result == VK_SUCCESS) physical = devices[0];
        free(devices);
        if (result != VK_SUCCESS || !physical) { failure = "physical device enumeration failed"; goto cleanup; }
    }
    vkGetPhysicalDeviceProperties(physical, &state.properties);
    vkGetPhysicalDeviceFeatures(physical, &state.core);
    state.api13 = state.properties.apiVersion >= VK_API_VERSION_1_3;
    state.fallback_detected = strstr(state.properties.deviceName, "llvmpipe") != NULL ||
        strstr(state.properties.deviceName, "softpipe") != NULL;
    if (state.fallback_detected) { failure = "software Vulkan fallback detected"; goto cleanup; }
    if (!query_requirements(physical, &state)) { failure = "capability query failed"; goto cleanup; }
    if (!state.api13) {
        write_result(&state, "UNSUPPORTED", "Guest adapter exposes Vulkan below 1.3");
        vkDestroyInstance(instance, NULL);
        return 3;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    {
        VkQueueFamilyProperties *queues = calloc(count ? count : 1, sizeof(*queues));
        if (!queues) { failure = "queue family allocation failed"; goto cleanup; }
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
        for (i = 0; i < count; ++i) {
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                state.queue_family = i;
                break;
            }
        }
        free(queues);
    }
    if (state.queue_family == UINT32_MAX) { failure = "no graphics queue family"; goto cleanup; }
    result = create_transport_device(physical, &state);
    state.transport_device_create_ok = state.transport_device_create_result == VK_SUCCESS;
    if (!state.transport_device_create_ok) {
        write_result(&state, "UNSUPPORTED", "DXVK 2.6 transport device requirements are unavailable");
        vkDestroyInstance(instance, NULL);
        return 3;
    }
    if (!state.timeline_round_trip_ok) {
        write_result(&state, "FAIL", "DXVK 2.6 timeline queue-submit/wait round trip failed");
        vkDestroyInstance(instance, NULL);
        return 2;
    }
    write_result(&state, "PASS", "DXVK 2.6 transport requirements passed; D3D11 baseline is evaluated by the unmodified runtime");
    exit_code = 0;

cleanup:
    if (exit_code) write_result(&state, "FAIL", failure);
    if (instance) vkDestroyInstance(instance, NULL);
    return exit_code;
}
