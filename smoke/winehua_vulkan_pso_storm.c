#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PIPELINES 5120u
#define DEFAULT_THREADS 8u
#define MAX_THREADS 32u
#define RENDER_WIDTH 64u
#define RENDER_HEIGHT 64u
#define HEARTBEAT_INTERVAL 256u

enum cache_mode {
    CACHE_MODE_NONE,
    CACHE_MODE_PER_PSO,
    CACHE_MODE_PER_THREAD,
    CACHE_MODE_SHARED
};

enum lifetime_mode {
    LIFETIME_MODE_IMMEDIATE,
    LIFETIME_MODE_DEFERRED
};

struct options {
    uint32_t pipelines;
    uint32_t threads;
    enum cache_mode cache_mode;
    enum lifetime_mode lifetime_mode;
    int shared_cache_lock;
    const char *vertex_path;
    const char *fragment_path;
    const char *result_path;
};

struct worker {
    VkDevice device;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    const uint32_t *vertex_code;
    size_t vertex_size;
    const uint32_t *fragment_code;
    size_t fragment_size;
    VkPipelineCache cache;
    enum cache_mode cache_mode;
    enum lifetime_mode lifetime_mode;
    pthread_mutex_t *shared_cache_mutex;
    VkPipeline *pipelines;
    VkShaderModule *deferred_vertices;
    VkShaderModule *deferred_fragments;
    VkPipelineCache *deferred_caches;
    double *latencies;
    atomic_uint *global_attempted;
    uint32_t total_pipelines;
    uint32_t first;
    uint32_t count;
    uint32_t thread_index;
    uint32_t attempted;
    uint32_t completed;
    uint32_t immediate_destroyed;
    double immediate_destroy_ms;
    uint32_t failures;
    uint32_t first_failure_index;
    VkResult first_error;
    double milliseconds;
};

struct render_diagnostics {
    uint64_t first_mismatch;
    uint64_t channel_mismatches[4];
    uint8_t actual_first_rgba[4];
    uint8_t expected_rgba[4];
    uint8_t actual_center_rgba[4];
    uint8_t actual_last_rgba[4];
};

static const char *cache_mode_name(enum cache_mode mode)
{
    switch (mode) {
    case CACHE_MODE_NONE: return "none";
    case CACHE_MODE_PER_PSO: return "per-pso";
    case CACHE_MODE_PER_THREAD: return "per-thread";
    case CACHE_MODE_SHARED: return "shared";
    default: return "invalid";
    }
}

static const char *lifetime_mode_name(enum lifetime_mode mode)
{
    return mode == LIFETIME_MODE_DEFERRED ? "deferred" : "immediate";
}

static uint64_t qpc_now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
            (uint64_t)value.tv_nsec;
}

static double qpc_ms(uint64_t begin, uint64_t end)
{
    return (double)(end - begin) / 1000000.0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (!text[0] || !end || *end || !parsed || parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int i;
    options->pipelines = DEFAULT_PIPELINES;
    options->threads = DEFAULT_THREADS;
    options->cache_mode = CACHE_MODE_PER_PSO;
    options->lifetime_mode = LIFETIME_MODE_IMMEDIATE;
    options->shared_cache_lock = 1;
    options->vertex_path = "pso_storm.vert.spv";
    options->fragment_path = "pso_storm.frag.spv";
    options->result_path = "pso-storm-result.json";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pipelines") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &options->pipelines))
                return 0;
        } else if ((!strcmp(argv[i], "--threads") ||
                !strcmp(argv[i], "--workers")) && i + 1 < argc) {
            if (!parse_u32(argv[++i], &options->threads))
                return 0;
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "none")) options->cache_mode = CACHE_MODE_NONE;
            else if (!strcmp(value, "per-pso")) options->cache_mode = CACHE_MODE_PER_PSO;
            else if (!strcmp(value, "per-thread")) options->cache_mode = CACHE_MODE_PER_THREAD;
            else if (!strcmp(value, "shared")) options->cache_mode = CACHE_MODE_SHARED;
            else return 0;
        } else if (!strcmp(argv[i], "--lifetime") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "immediate")) options->lifetime_mode = LIFETIME_MODE_IMMEDIATE;
            else if (!strcmp(value, "deferred")) options->lifetime_mode = LIFETIME_MODE_DEFERRED;
            else return 0;
        } else if (!strcmp(argv[i], "--shared-cache-lock") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "on")) options->shared_cache_lock = 1;
            else if (!strcmp(value, "off")) options->shared_cache_lock = 0;
            else return 0;
        } else if (!strcmp(argv[i], "--vertex") && i + 1 < argc) {
            options->vertex_path = argv[++i];
        } else if (!strcmp(argv[i], "--fragment") && i + 1 < argc) {
            options->fragment_path = argv[++i];
        } else if (!strcmp(argv[i], "--result") && i + 1 < argc) {
            options->result_path = argv[++i];
        } else {
            return 0;
        }
    }
    return options->threads <= MAX_THREADS && options->threads <= options->pipelines;
}

static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double percentile(const double *sorted, uint32_t count, uint32_t percent)
{
    uint64_t index;
    if (!count) return 0.0;
    index = ((uint64_t)(count - 1u) * percent + 50u) / 100u;
    return sorted[index];
}

static void *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    void *data;
    long length;
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
            fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static VkResult create_shader(VkDevice device, const uint32_t *code,
        size_t size, VkShaderModule *module)
{
    VkShaderModuleCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    return vkCreateShaderModule(device, &info, NULL, module);
}

static VkResult create_pipeline(struct worker *worker, uint32_t index,
        VkShaderModule vertex, VkShaderModule fragment, VkPipelineCache cache,
        VkPipeline *pipeline)
{
    VkSpecializationMapEntry vertex_entry;
    VkSpecializationMapEntry fragment_entry;
    VkSpecializationInfo vertex_specialization;
    VkSpecializationInfo fragment_specialization;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterization;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkGraphicsPipelineCreateInfo info;
    float x_offset = ((int32_t)(index % 17u) - 8) / 128.0f;
    uint32_t variant = 0xff000000u | (index * 2654435761u & 0x00ffffffu);

    memset(&vertex_entry, 0, sizeof(vertex_entry));
    vertex_entry.constantID = 0;
    vertex_entry.size = sizeof(x_offset);
    vertex_specialization.mapEntryCount = 1;
    vertex_specialization.pMapEntries = &vertex_entry;
    vertex_specialization.dataSize = sizeof(x_offset);
    vertex_specialization.pData = &x_offset;

    memset(&fragment_entry, 0, sizeof(fragment_entry));
    fragment_entry.constantID = 1;
    fragment_entry.size = sizeof(variant);
    fragment_specialization.mapEntryCount = 1;
    fragment_specialization.pMapEntries = &fragment_entry;
    fragment_specialization.dataSize = sizeof(variant);
    fragment_specialization.pData = &variant;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = &vertex_specialization;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &fragment_specialization;

    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&assembly, 0, sizeof(assembly));
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    memset(&viewport, 0, sizeof(viewport));
    viewport.width = (float)RENDER_WIDTH;
    viewport.height = (float)RENDER_HEIGHT;
    viewport.maxDepth = 1.0f;
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent.width = RENDER_WIDTH;
    scissor.extent.height = RENDER_HEIGHT;
    memset(&viewport_state, 0, sizeof(viewport_state));
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    memset(&rasterization, 0, sizeof(rasterization));
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.lineWidth = 1.0f;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    memset(&multisample, 0, sizeof(multisample));
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&blend_attachment, 0, sizeof(blend_attachment));
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
    memset(&blend, 0, sizeof(blend));
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = worker->layout;
    info.renderPass = worker->render_pass;
    info.subpass = 0;
    return vkCreateGraphicsPipelines(worker->device, cache, 1, &info,
            NULL, pipeline);
}

static void *worker_main(void *opaque)
{
    struct worker *worker = opaque;
    VkPipelineCacheCreateInfo cache_info;
    uint64_t begin = qpc_now();
    uint32_t i;
    memset(&cache_info, 0, sizeof(cache_info));
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    for (i = 0; i < worker->count; i++) {
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        VkPipelineCache cache = worker->cache;
        VkResult result = VK_SUCCESS;
        uint64_t latency_begin = qpc_now();
        const uint32_t index = worker->first + i;

        result = create_shader(worker->device, worker->vertex_code,
                worker->vertex_size, &vertex);
        if (result == VK_SUCCESS)
            result = create_shader(worker->device, worker->fragment_code,
                    worker->fragment_size, &fragment);
        if (result == VK_SUCCESS && worker->cache_mode == CACHE_MODE_PER_PSO)
            result = vkCreatePipelineCache(worker->device, &cache_info, NULL, &cache);
        if (result == VK_SUCCESS) {
            if (worker->shared_cache_mutex)
                pthread_mutex_lock(worker->shared_cache_mutex);
            result = create_pipeline(worker, index, vertex, fragment, cache,
                    &worker->pipelines[index]);
            if (worker->shared_cache_mutex)
                pthread_mutex_unlock(worker->shared_cache_mutex);
        }
        if (worker->lifetime_mode == LIFETIME_MODE_DEFERRED) {
            worker->deferred_vertices[index] = vertex;
            worker->deferred_fragments[index] = fragment;
            if (worker->cache_mode == CACHE_MODE_PER_PSO)
                worker->deferred_caches[index] = cache;
        } else {
            /* Keep only the final pipeline for the deterministic render check.
             * All other successful objects exercise real pipeline churn. */
            if (result == VK_SUCCESS && index + 1u != worker->total_pipelines) {
                const uint64_t destroy_begin = qpc_now();
                vkDestroyPipeline(worker->device, worker->pipelines[index], NULL);
                worker->immediate_destroy_ms += qpc_ms(destroy_begin, qpc_now());
                worker->pipelines[index] = VK_NULL_HANDLE;
                worker->immediate_destroyed++;
            }
            if (worker->cache_mode == CACHE_MODE_PER_PSO && cache)
                vkDestroyPipelineCache(worker->device, cache, NULL);
            if (fragment) vkDestroyShaderModule(worker->device, fragment, NULL);
            if (vertex) vkDestroyShaderModule(worker->device, vertex, NULL);
        }
        worker->latencies[index] = qpc_ms(latency_begin, qpc_now());
        worker->attempted++;
        if (result != VK_SUCCESS) {
            worker->failures++;
            if (worker->first_error == VK_SUCCESS) {
                worker->first_error = result;
                worker->first_failure_index = index;
            }
        } else {
            worker->completed++;
        }
        {
            unsigned int attempted = atomic_fetch_add(worker->global_attempted, 1u) + 1u;
            if (!(attempted % HEARTBEAT_INTERVAL)) {
                fprintf(stderr, "pso-storm heartbeat attempted=%u total=%u worker=%u\n",
                        attempted, worker->total_pipelines, worker->thread_index);
                fflush(stderr);
            }
        }
    }
    worker->milliseconds = qpc_ms(begin, qpc_now());
    return NULL;
}

static uint32_t find_graphics_queue(VkPhysicalDevice physical)
{
    VkQueueFamilyProperties *properties;
    uint32_t count = 0;
    uint32_t index;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count)
        return UINT32_MAX;
    properties = calloc(count, sizeof(*properties));
    if (!properties)
        return UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties);
    for (index = 0; index < count; index++) {
        if (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    }
    free(properties);
    return index < count ? index : UINT32_MAX;
}

static int find_memory_type(VkPhysicalDevice physical, uint32_t bits,
        VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (i = 0; i < properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static VkResult create_bound_buffer(VkPhysicalDevice physical, VkDevice device,
        VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties, VkBuffer *buffer,
        VkDeviceMemory *memory, VkMemoryPropertyFlags *actual_properties)
{
    VkBufferCreateInfo buffer_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t memory_type;
    VkResult result;

    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(device, &buffer_info, NULL, buffer);
    if (result != VK_SUCCESS)
        return result;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    if (!find_memory_type(physical, requirements.memoryTypeBits, properties,
            &memory_type)) {
        if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                find_memory_type(physical, requirements.memoryTypeBits,
                properties & ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &memory_type)) {
            properties &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        } else {
            vkDestroyBuffer(device, *buffer, NULL);
            *buffer = VK_NULL_HANDLE;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    *actual_properties = memory_properties.memoryTypes[memory_type].propertyFlags;
    memset(&allocation, 0, sizeof(allocation));
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return result;
    }
    result = vkBindBufferMemory(device, *buffer, *memory, 0);
    if (result != VK_SUCCESS) {
        vkFreeMemory(device, *memory, NULL);
        vkDestroyBuffer(device, *buffer, NULL);
        *memory = VK_NULL_HANDLE;
        *buffer = VK_NULL_HANDLE;
    }
    return result;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static VkResult render_and_validate(VkPhysicalDevice physical, VkDevice device,
        VkQueue queue, uint32_t queue_family, VkRenderPass render_pass,
        VkPipeline pipeline, uint32_t pipeline_index, int *readback_ok,
        uint64_t *actual_hash, uint64_t *expected_hash,
        struct render_diagnostics *diagnostics)
{
    const VkDeviceSize readback_size = RENDER_WIDTH * RENDER_HEIGHT * 4u;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult result;
    void *mapped = NULL;

    VkImageCreateInfo image_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    VkImageViewCreateInfo view_info;
    VkFramebufferCreateInfo framebuffer_info;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo command_info;
    VkFenceCreateInfo fence_info;
    VkMemoryPropertyFlags readback_properties = 0;
    uint32_t memory_type;

    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = RENDER_WIDTH;
    image_info.extent.height = RENDER_HEIGHT;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateImage(device, &image_info, NULL, &image);
    if (result != VK_SUCCESS)
        goto done;
    vkGetImageMemoryRequirements(device, image, &requirements);
    if (!find_memory_type(physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) &&
            !find_memory_type(physical, requirements.memoryTypeBits, 0,
            &memory_type)) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(device, &allocation, NULL, &image_memory);
    if (result != VK_SUCCESS ||
            (result = vkBindImageMemory(device, image, image_memory, 0)) != VK_SUCCESS)
        goto done;

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &view_info, NULL, &image_view);
    if (result != VK_SUCCESS)
        goto done;

    memset(&framebuffer_info, 0, sizeof(framebuffer_info));
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &image_view;
    framebuffer_info.width = RENDER_WIDTH;
    framebuffer_info.height = RENDER_HEIGHT;
    framebuffer_info.layers = 1;
    result = vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer);
    if (result != VK_SUCCESS)
        goto done;
    result = create_bound_buffer(physical, device, readback_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &readback, &readback_memory,
            &readback_properties);
    if (result != VK_SUCCESS)
        goto done;

    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    result = vkCreateCommandPool(device, &pool_info, NULL, &pool);
    if (result != VK_SUCCESS)
        goto done;
    memset(&command_info, 0, sizeof(command_info));
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &command_info, &command);
    if (result != VK_SUCCESS)
        goto done;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS)
        goto done;

    {
        VkCommandBufferBeginInfo begin;
        VkClearValue clear;
        VkRenderPassBeginInfo render;
        VkImageMemoryBarrier barrier;
        VkBufferImageCopy copy;
        VkSubmitInfo submit;
        memset(&begin, 0, sizeof(begin));
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        memset(&clear, 0, sizeof(clear));
        clear.color.float32[0] = 1.0f;
        clear.color.float32[2] = 1.0f;
        clear.color.float32[3] = 1.0f;
        memset(&render, 0, sizeof(render));
        render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass = render_pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent.width = RENDER_WIDTH;
        render.renderArea.extent.height = RENDER_HEIGHT;
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        memset(&barrier, 0, sizeof(barrier));
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        memset(&copy, 0, sizeof(copy));
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = RENDER_WIDTH;
        copy.imageExtent.height = RENDER_HEIGHT;
        copy.imageExtent.depth = 1;
        memset(&submit, 0, sizeof(submit));
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;

        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS)
            goto done;
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        vkCmdPipelineBarrier(command,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                &barrier);
        vkCmdCopyImageToBuffer(command, image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        result = vkEndCommandBuffer(command);
        if (result == VK_SUCCESS)
            result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result == VK_SUCCESS)
            result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                    10000000000ull);
        if (result != VK_SUCCESS)
            goto done;
    }

    result = vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result == VK_SUCCESS) {
        const uint32_t variant = UINT32_C(0xff000000) |
                (pipeline_index * UINT32_C(2654435761) & UINT32_C(0x00ffffff));
        const uint8_t expected[4] = {
            (uint8_t)variant, (uint8_t)(variant >> 8),
            (uint8_t)(variant >> 16), UINT8_C(255)
        };
        const uint8_t *bytes = mapped;
        uint64_t expected_value = UINT64_C(14695981039346656037);
        size_t i;
        diagnostics->first_mismatch = UINT64_MAX;
        memcpy(diagnostics->expected_rgba, expected, sizeof(expected));
        if (!(readback_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range;
            memset(&range, 0, sizeof(range));
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readback_memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            result = vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        *readback_ok = result == VK_SUCCESS;
        for (i = 0; i < (size_t)readback_size; i++) {
            if (bytes[i] != expected[i & 3u]) {
                if (diagnostics->first_mismatch == UINT64_MAX)
                    diagnostics->first_mismatch = i;
                diagnostics->channel_mismatches[i & 3u]++;
                *readback_ok = 0;
            }
        }
        memcpy(diagnostics->actual_first_rgba, bytes, 4);
        memcpy(diagnostics->actual_center_rgba,
                bytes + ((RENDER_HEIGHT / 2u * RENDER_WIDTH +
                RENDER_WIDTH / 2u) * 4u), 4);
        memcpy(diagnostics->actual_last_rgba, bytes + readback_size - 4u, 4);
        *actual_hash = fnv1a64(bytes, (size_t)readback_size);
        for (i = 0; i < (size_t)readback_size; i++) {
            expected_value ^= expected[i & 3u];
            expected_value *= UINT64_C(1099511628211);
        }
        *expected_hash = expected_value;
        vkUnmapMemory(device, readback_memory);
        mapped = NULL;
        if (result == VK_SUCCESS && !*readback_ok)
            result = VK_ERROR_VALIDATION_FAILED_EXT;
    }

done:
    if (mapped)
        vkUnmapMemory(device, readback_memory);
    if (fence)
        vkDestroyFence(device, fence, NULL);
    if (pool)
        vkDestroyCommandPool(device, pool, NULL);
    if (readback)
        vkDestroyBuffer(device, readback, NULL);
    if (readback_memory)
        vkFreeMemory(device, readback_memory, NULL);
    if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, NULL);
    if (image_view)
        vkDestroyImageView(device, image_view, NULL);
    if (image)
        vkDestroyImage(device, image, NULL);
    if (image_memory)
        vkFreeMemory(device, image_memory, NULL);
    return result;
}

static void write_result(const struct options *options,
        const VkPhysicalDeviceProperties *properties, const struct worker *workers,
        uint32_t attempted, uint32_t completed, uint32_t failures,
        int64_t first_failure_index, VkResult result,
        double object_create_ms, double pipeline_create_ms,
        double render_validate_ms, double object_destroy_ms,
        const double *sorted_latencies, int readback_ok,
        uint64_t actual_hash, uint64_t expected_hash,
        const struct render_diagnostics *diagnostics)
{
    FILE *file = fopen(options->result_path, "wb");
    const char *ring_mode = getenv("WINEHUA_PSO_RING_MODE");
    uint32_t immediate_destroyed = 0;
    double immediate_destroy_ms = 0.0;
    uint32_t i;
    if (!file)
        return;
    for (i = 0; i < options->threads; i++) {
        immediate_destroyed += workers[i].immediate_destroyed;
        immediate_destroy_ms += workers[i].immediate_destroy_ms;
    }
    fprintf(file,
            "{\n  \"schemaVersion\": 2,\n"
            "  \"test\": \"vulkan-pso-storm\",\n"
            "  \"status\": \"%s\",\n"
            "  \"vkResult\": %d,\n"
            "  \"device\": {\"name\": \"%s\", \"vendorId\": %u, "
            "\"deviceId\": %u, \"driverVersion\": %u, \"apiVersion\": %u},\n"
            "  \"mode\": {\"cache\": \"%s\", \"lifetime\": \"%s\", \"ring\": \"%s\", "
            "\"sharedCacheLock\": %s, \"sharedUnlockedDiagnostic\": %s},\n"
            "  \"requestedPipelines\": %u,\n"
            "  \"attemptedPipelines\": %u,\n"
            "  \"completedPipelines\": %u,\n"
            "  \"retainedPipelineCount\": %u,\n"
            "  \"immediateDestroyedPipelines\": %u,\n"
            "  \"failures\": %u,\n"
            "  \"firstFailureIndex\": %" PRId64 ",\n"
            "  \"workers\": %u,\n"
            "  \"heartbeatInterval\": %u,\n"
            "  \"timings\": {\"objectCreateMilliseconds\": %.3f, "
            "\"pipelineWorkloadMilliseconds\": %.3f, "
            "\"renderValidateMilliseconds\": %.3f, "
            "\"objectDestroyMilliseconds\": %.3f, "
            "\"immediatePipelineDestroyMilliseconds\": %.3f},\n"
            "  \"psoLatencyMilliseconds\": {\"p50\": %.3f, \"p95\": %.3f, "
            "\"p99\": %.3f, \"max\": %.3f},\n"
            "  \"pipelinesPerSecond\": %.3f,\n",
            failures || result != VK_SUCCESS ? "FAIL" : "PASS", result,
            properties ? properties->deviceName : "unavailable",
            properties ? properties->vendorID : 0,
            properties ? properties->deviceID : 0,
            properties ? properties->driverVersion : 0,
            properties ? properties->apiVersion : 0,
            cache_mode_name(options->cache_mode),
            lifetime_mode_name(options->lifetime_mode),
            ring_mode && ring_mode[0] ? ring_mode : "unspecified",
            options->shared_cache_lock ? "true" : "false",
            options->cache_mode == CACHE_MODE_SHARED &&
                    !options->shared_cache_lock ? "true" : "false",
            options->pipelines, attempted, completed,
            completed ? (options->lifetime_mode == LIFETIME_MODE_IMMEDIATE ? 1u : completed) : 0u,
            immediate_destroyed, failures, first_failure_index,
            options->threads, HEARTBEAT_INTERVAL,
            object_create_ms, pipeline_create_ms, render_validate_ms,
            object_destroy_ms, immediate_destroy_ms,
            percentile(sorted_latencies, attempted, 50),
            percentile(sorted_latencies, attempted, 95),
            percentile(sorted_latencies, attempted, 99),
            attempted ? sorted_latencies[attempted - 1u] : 0.0,
            pipeline_create_ms > 0.0 ? completed * 1000.0 /
                    pipeline_create_ms : 0.0);
    /* Keep argument lists short here. Box64's libc vararg bridge has crashed
     * on this probe's former 70-argument fprintf after all Vulkan work had
     * completed successfully. */
    fprintf(file,
            "  \"renderValidation\": {\"passed\": %s, "
            "\"width\": %u, \"height\": %u, \"bytesCompared\": %u, "
            "\"actualFnv1a64\": \"0x%016" PRIx64 "\", "
            "\"expectedFnv1a64\": \"0x%016" PRIx64 "\", "
            "\"firstMismatchByte\": %" PRIu64 ",\n",
            readback_ok ? "true" : "false", RENDER_WIDTH, RENDER_HEIGHT,
            RENDER_WIDTH * RENDER_HEIGHT * 4u, actual_hash, expected_hash,
            diagnostics->first_mismatch);
    fprintf(file,
            "    \"expectedRgba\": [%u,%u,%u,%u], "
            "\"actualFirstRgba\": [%u,%u,%u,%u],\n",
            diagnostics->expected_rgba[0], diagnostics->expected_rgba[1],
            diagnostics->expected_rgba[2], diagnostics->expected_rgba[3],
            diagnostics->actual_first_rgba[0], diagnostics->actual_first_rgba[1],
            diagnostics->actual_first_rgba[2], diagnostics->actual_first_rgba[3]);
    fprintf(file,
            "    \"actualCenterRgba\": [%u,%u,%u,%u], "
            "\"actualLastRgba\": [%u,%u,%u,%u],\n",
            diagnostics->actual_center_rgba[0], diagnostics->actual_center_rgba[1],
            diagnostics->actual_center_rgba[2], diagnostics->actual_center_rgba[3],
            diagnostics->actual_last_rgba[0], diagnostics->actual_last_rgba[1],
            diagnostics->actual_last_rgba[2], diagnostics->actual_last_rgba[3]);
    fprintf(file,
            "    \"channelMismatchCounts\": [%llu,%llu,%llu,%llu]},\n"
            "  \"workerResults\": [\n",
            (unsigned long long)diagnostics->channel_mismatches[0],
            (unsigned long long)diagnostics->channel_mismatches[1],
            (unsigned long long)diagnostics->channel_mismatches[2],
            (unsigned long long)diagnostics->channel_mismatches[3]);
    for (i = 0; i < options->threads; i++) {
        fprintf(file,
                "    {\"index\": %u, \"requested\": %u, "
                "\"attempted\": %u, \"completed\": %u, \"failures\": %u, "
                "\"firstFailureIndex\": %d, \"firstError\": %d, "
                "\"milliseconds\": %.3f}%s\n",
                i, workers[i].count, workers[i].attempted, workers[i].completed,
                workers[i].failures,
                workers[i].first_error == VK_SUCCESS ? -1 :
                        (int)workers[i].first_failure_index,
                workers[i].first_error, workers[i].milliseconds,
                i + 1 == options->threads ? "" : ",");
    }
    fprintf(file, "  ]\n}\n");
    fclose(file);
}

int main(int argc, char **argv)
{
    struct options options;
    struct worker workers[MAX_THREADS];
    pthread_t threads[MAX_THREADS];
    VkApplicationInfo application;
    VkInstanceCreateInfo instance_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo layout_info;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineCache caches[MAX_THREADS];
    VkAttachmentDescription attachment;
    VkAttachmentReference attachment_reference;
    VkSubpassDescription subpass;
    VkRenderPassCreateInfo render_pass_info;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPipeline *pipelines = NULL;
    VkShaderModule *deferred_vertices = NULL;
    VkShaderModule *deferred_fragments = NULL;
    VkPipelineCache *deferred_caches = NULL;
    double *latencies = NULL;
    double *sorted_latencies = NULL;
    uint32_t *vertex_code = NULL;
    uint32_t *fragment_code = NULL;
    size_t vertex_size = 0;
    size_t fragment_size = 0;
    uint32_t physical_count = 1;
    uint32_t queue_family;
    uint32_t attempted = 0;
    uint32_t completed = 0;
    uint32_t failures = 0;
    uint32_t threads_created = 0;
    int64_t first_failure_index = -1;
    atomic_uint global_attempted = 0;
    uint64_t timing_begin = 0;
    double object_create_ms = 0.0;
    double pipeline_create_ms = 0.0;
    double render_validate_ms = 0.0;
    double object_destroy_ms = 0.0;
    int readback_ok = 0;
    uint64_t actual_hash = 0;
    uint64_t expected_hash = 0;
    struct render_diagnostics render_diagnostics;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_t shared_cache_mutex;
    int shared_cache_mutex_initialized = 0;
    float queue_priority = 1.0f;
    uint32_t i;

    memset(workers, 0, sizeof(workers));
    memset(threads, 0, sizeof(threads));
    memset(caches, 0, sizeof(caches));
    memset(&render_diagnostics, 0, sizeof(render_diagnostics));
    render_diagnostics.first_mismatch = UINT64_MAX;
    memset(&properties, 0, sizeof(properties));
    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr, "usage: %s [--pipelines N] [--workers N] "
                "[--cache none|per-pso|per-thread|shared] "
                "[--shared-cache-lock on|off] "
                "[--lifetime immediate|deferred] [--vertex PATH] "
                "[--fragment PATH] [--result PATH]\n", argv[0]);
        return 2;
    }
    if (options.cache_mode != CACHE_MODE_SHARED)
        options.shared_cache_lock = 0;
    vertex_code = read_file(options.vertex_path, &vertex_size);
    fragment_code = read_file(options.fragment_path, &fragment_size);
    if (!vertex_code || !fragment_code || vertex_size % 4 || fragment_size % 4) {
        fprintf(stderr, "failed to read valid SPIR-V files\n");
        goto done;
    }

    memset(&application, 0, sizeof(application));
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "WineHua PSOStorm";
    application.apiVersion = VK_API_VERSION_1_1;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) goto done;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
    if (result != VK_SUCCESS || !physical_count) {
        if (result == VK_SUCCESS) result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    vkGetPhysicalDeviceProperties(physical, &properties);
    queue_family = find_graphics_queue(physical);
    if (queue_family == UINT32_MAX) goto done;

    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    result = vkCreateDevice(physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS) goto done;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    timing_begin = qpc_now();
    memset(&layout_info, 0, sizeof(layout_info));
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    result = vkCreatePipelineLayout(device, &layout_info, NULL, &layout);
    if (result != VK_SUCCESS) goto done;

    memset(&attachment, 0, sizeof(attachment));
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_reference.attachment = 0;
    attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment_reference;
    memset(&render_pass_info, 0, sizeof(render_pass_info));
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass);
    if (result != VK_SUCCESS) goto done;

    if (options.cache_mode == CACHE_MODE_PER_THREAD ||
            options.cache_mode == CACHE_MODE_SHARED) {
        const uint32_t cache_count = options.cache_mode == CACHE_MODE_SHARED ?
                1u : options.threads;
        for (i = 0; i < cache_count; i++) {
            VkPipelineCacheCreateInfo cache_info;
            memset(&cache_info, 0, sizeof(cache_info));
            cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            result = vkCreatePipelineCache(device, &cache_info, NULL, &caches[i]);
            if (result != VK_SUCCESS) goto done;
        }
    }
    if (options.cache_mode == CACHE_MODE_SHARED && options.shared_cache_lock) {
        if (pthread_mutex_init(&shared_cache_mutex, NULL)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto done;
        }
        shared_cache_mutex_initialized = 1;
    }
    object_create_ms = qpc_ms(timing_begin, qpc_now());

    pipelines = calloc(options.pipelines, sizeof(*pipelines));
    latencies = calloc(options.pipelines, sizeof(*latencies));
    sorted_latencies = calloc(options.pipelines, sizeof(*sorted_latencies));
    if (options.lifetime_mode == LIFETIME_MODE_DEFERRED) {
        deferred_vertices = calloc(options.pipelines, sizeof(*deferred_vertices));
        deferred_fragments = calloc(options.pipelines, sizeof(*deferred_fragments));
        if (options.cache_mode == CACHE_MODE_PER_PSO)
            deferred_caches = calloc(options.pipelines, sizeof(*deferred_caches));
    }
    if (!pipelines || !latencies || !sorted_latencies ||
            (options.lifetime_mode == LIFETIME_MODE_DEFERRED &&
            (!deferred_vertices || !deferred_fragments ||
            (options.cache_mode == CACHE_MODE_PER_PSO && !deferred_caches)))) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }

    timing_begin = qpc_now();
    for (i = 0; i < options.threads; i++) {
        const uint32_t base = options.pipelines / options.threads;
        const uint32_t extra = options.pipelines % options.threads;
        workers[i].device = device;
        workers[i].layout = layout;
        workers[i].render_pass = render_pass;
        workers[i].vertex_code = vertex_code;
        workers[i].vertex_size = vertex_size;
        workers[i].fragment_code = fragment_code;
        workers[i].fragment_size = fragment_size;
        workers[i].cache_mode = options.cache_mode;
        workers[i].lifetime_mode = options.lifetime_mode;
        workers[i].cache = options.cache_mode == CACHE_MODE_PER_THREAD ?
                caches[i] : options.cache_mode == CACHE_MODE_SHARED ?
                caches[0] : VK_NULL_HANDLE;
        workers[i].shared_cache_mutex = shared_cache_mutex_initialized ?
                &shared_cache_mutex : NULL;
        workers[i].pipelines = pipelines;
        workers[i].deferred_vertices = deferred_vertices;
        workers[i].deferred_fragments = deferred_fragments;
        workers[i].deferred_caches = deferred_caches;
        workers[i].latencies = latencies;
        workers[i].global_attempted = &global_attempted;
        workers[i].total_pipelines = options.pipelines;
        workers[i].first = i * base + (i < extra ? i : extra);
        workers[i].count = base + (i < extra ? 1u : 0u);
        workers[i].thread_index = i;
        workers[i].first_error = VK_SUCCESS;
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i])) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            failures++;
            break;
        }
        threads_created++;
    }
    for (i = 0; i < threads_created; i++)
        pthread_join(threads[i], NULL);
    pipeline_create_ms = qpc_ms(timing_begin, qpc_now());
    for (i = 0; i < threads_created; i++) {
        attempted += workers[i].attempted;
        completed += workers[i].completed;
        failures += workers[i].failures;
        if (workers[i].first_error != VK_SUCCESS &&
                (first_failure_index < 0 ||
                workers[i].first_failure_index < (uint32_t)first_failure_index))
            first_failure_index = workers[i].first_failure_index;
        if (result == VK_SUCCESS && workers[i].first_error != VK_SUCCESS)
            result = workers[i].first_error;
    }
    memcpy(sorted_latencies, latencies, attempted * sizeof(*sorted_latencies));
    qsort(sorted_latencies, attempted, sizeof(*sorted_latencies), compare_double);
    if (!failures && completed == options.pipelines) result = VK_SUCCESS;

    if (result == VK_SUCCESS) {
        timing_begin = qpc_now();
        result = render_and_validate(physical, device, queue, queue_family,
                render_pass, pipelines[options.pipelines - 1u],
                options.pipelines - 1u, &readback_ok, &actual_hash,
                &expected_hash, &render_diagnostics);
        render_validate_ms = qpc_ms(timing_begin, qpc_now());
        if (result != VK_SUCCESS) failures++;
    }

done:
    timing_begin = qpc_now();
    if (device && pipelines) {
        for (i = 0; i < options.pipelines; i++)
            if (pipelines[i]) vkDestroyPipeline(device, pipelines[i], NULL);
    }
    if (device && deferred_caches) {
        for (i = 0; i < options.pipelines; i++)
            if (deferred_caches[i])
                vkDestroyPipelineCache(device, deferred_caches[i], NULL);
    }
    if (device && deferred_fragments) {
        for (i = 0; i < options.pipelines; i++)
            if (deferred_fragments[i])
                vkDestroyShaderModule(device, deferred_fragments[i], NULL);
    }
    if (device && deferred_vertices) {
        for (i = 0; i < options.pipelines; i++)
            if (deferred_vertices[i])
                vkDestroyShaderModule(device, deferred_vertices[i], NULL);
    }
    if (device) {
        for (i = 0; i < MAX_THREADS; i++)
            if (caches[i]) vkDestroyPipelineCache(device, caches[i], NULL);
    }
    if (device && render_pass) vkDestroyRenderPass(device, render_pass, NULL);
    if (device && layout) vkDestroyPipelineLayout(device, layout, NULL);
    object_destroy_ms = qpc_ms(timing_begin, qpc_now());
    write_result(&options, physical ? &properties : NULL, workers, attempted,
            completed, failures, first_failure_index, result, object_create_ms,
            pipeline_create_ms, render_validate_ms, object_destroy_ms,
            sorted_latencies, readback_ok, actual_hash, expected_hash,
            &render_diagnostics);
    fprintf(stdout,
            "pso-storm status=%s result=%d requested=%u attempted=%u "
            "completed=%u failures=%u workers=%u cache=%s lifetime=%s "
            "pso_ms=%.3f render_ok=%d hash=0x%016" PRIx64
            " expected=0x%016" PRIx64 "\n",
            result == VK_SUCCESS && !failures ? "PASS" : "FAIL", result,
            options.pipelines, attempted, completed, failures, options.threads,
            cache_mode_name(options.cache_mode),
            lifetime_mode_name(options.lifetime_mode), pipeline_create_ms,
            readback_ok, actual_hash, expected_hash);
    if (shared_cache_mutex_initialized)
        pthread_mutex_destroy(&shared_cache_mutex);
    free(sorted_latencies);
    free(latencies);
    free(deferred_caches);
    free(deferred_fragments);
    free(deferred_vertices);
    free(pipelines);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    free(fragment_code);
    free(vertex_code);
    return result == VK_SUCCESS && !failures ? 0 : 1;
}
