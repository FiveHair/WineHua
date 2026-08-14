#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERATIONS 32u

static uint64_t now_ns(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
            (uint64_t)value.tv_nsec;
}

static double ms_between(uint64_t begin, uint64_t end)
{
    return (double)(end - begin) / 1000000.0;
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

static uint32_t find_graphics_queue(VkPhysicalDevice physical)
{
    uint32_t count = 0;
    VkQueueFamilyProperties families[16];
    uint32_t i;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (count > 16)
        count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            return i;
    }
    return UINT32_MAX;
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

/* Render one triangle into a transient offscreen color target. Maleoon defers
 * shader compilation until the first draw, so creating a pipeline alone never
 * populates the pipeline cache. Submitting and waiting here forces the lazy
 * compile so vkGetPipelineCacheData can return real bytes. */
static VkResult render_pipeline(VkPhysicalDevice physical, VkDevice device,
        VkQueue queue, uint32_t queue_family, VkRenderPass render_pass,
        VkPipeline pipeline)
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;

    VkImageCreateInfo image_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    VkImageViewCreateInfo view_info;
    VkFramebufferCreateInfo framebuffer_info;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo command_info;
    VkFenceCreateInfo fence_info;
    uint32_t memory_type = 0;

    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = 64u;
    image_info.extent.height = 64u;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
    framebuffer_info.width = 64u;
    framebuffer_info.height = 64u;
    framebuffer_info.layers = 1;
    result = vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer);
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
        VkSubmitInfo submit;
        memset(&begin, 0, sizeof(begin));
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        memset(&clear, 0, sizeof(clear));
        clear.color.float32[3] = 1.0f;
        memset(&render, 0, sizeof(render));
        render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass = render_pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent.width = 64u;
        render.renderArea.extent.height = 64u;
        render.clearValueCount = 1;
        render.pClearValues = &clear;
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
        result = vkEndCommandBuffer(command);
        if (result == VK_SUCCESS)
            result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result == VK_SUCCESS)
            result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                    10000000000ull);
        if (result != VK_SUCCESS)
            goto done;
    }

    result = VK_SUCCESS;
done:
    if (fence)
        vkDestroyFence(device, fence, NULL);
    if (pool)
        vkDestroyCommandPool(device, pool, NULL);
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

static VkResult create_pipeline(VkDevice device, VkPipelineLayout layout,
        VkRenderPass render_pass, VkShaderModule vertex,
        VkShaderModule fragment, VkPipelineCache cache, VkPipeline *pipeline)
{
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
    VkSpecializationMapEntry vertex_entry;
    VkSpecializationMapEntry fragment_entry;
    VkSpecializationInfo vertex_specialization;
    VkSpecializationInfo fragment_specialization;
    float x_offset = 0.0f;
    uint32_t variant = 0u;

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
    viewport.width = 64.0f;
    viewport.height = 64.0f;
    viewport.maxDepth = 1.0f;
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent.width = 64u;
    scissor.extent.height = 64u;
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
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;
    return vkCreateGraphicsPipelines(device, cache, 1, &info, NULL, pipeline);
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

static void emit_result(const char *path, uint32_t iterations,
        double cold_ms, double warm_ms, size_t cache_bytes, int warm_ok,
        int result_code, const char *stage)
{
    FILE *file = path ? fopen(path, "w") : stdout;
    if (!file)
        file = stdout;
    fprintf(file,
            "{\"status\":\"%s\",\"iterations\":%u,\"coldMs\":%.3f,\"warmMs\":%.3f,"
            "\"cacheBytes\":%zu,\"warmOk\":%s,"
            "\"speedup\":%.3f,\"vkResult\":%d,\"stage\":\"%s\"}\n",
            result_code == VK_SUCCESS ? "PASS" : "FAIL",
            iterations, cold_ms, warm_ms, cache_bytes,
            warm_ok ? "true" : "false",
            cold_ms > 0.0 ? cold_ms / warm_ms : 0.0, result_code, stage);
    if (file != stdout)
        fclose(file);
}

int main(int argc, char **argv)
{
    uint32_t iterations = DEFAULT_ITERATIONS;
    const char *vertex_path = "pso_storm.vert.spv";
    const char *fragment_path = "pso_storm.frag.spv";
    const char *result_path = NULL;
    VkApplicationInfo application;
    VkInstanceCreateInfo instance_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t physical_count = 1;
    uint32_t queue_family = UINT32_MAX;
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo layout_info;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkAttachmentDescription attachment;
    VkAttachmentReference attachment_reference;
    VkSubpassDescription subpass;
    VkRenderPassCreateInfo render_pass_info;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo cache_info;
    VkPipelineCache cold_cache = VK_NULL_HANDLE;
    VkPipelineCache warm_cache = VK_NULL_HANDLE;
    const uint32_t *vertex_code;
    const uint32_t *fragment_code;
    size_t vertex_size = 0;
    size_t fragment_size = 0;
    uint8_t *cache_data = NULL;
    size_t cache_bytes = 0;
    VkPipeline *pipelines = NULL;
    uint64_t begin;
    double cold_ms = 0.0;
    double warm_ms = 0.0;
    int warm_ok = 0;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    const char *stage = "start";
    uint32_t i;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--iterations") && i + 1u < (uint32_t)argc) {
            if (!parse_u32(argv[++i], &iterations))
                return 2;
        } else if (!strcmp(argv[i], "--vertex") && i + 1u < (uint32_t)argc) {
            vertex_path = argv[++i];
        } else if (!strcmp(argv[i], "--fragment") && i + 1u < (uint32_t)argc) {
            fragment_path = argv[++i];
        } else if (!strcmp(argv[i], "--result") && i + 1u < (uint32_t)argc) {
            result_path = argv[++i];
        } else {
            return 2;
        }
    }

    vertex_code = read_file(vertex_path, &vertex_size);
    fragment_code = read_file(fragment_path, &fragment_size);
    if (!vertex_code || !fragment_code || vertex_size % 4 || fragment_size % 4)
        goto done;

    memset(&application, 0, sizeof(application));
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "WineHua CacheReuse";
    application.apiVersion = VK_API_VERSION_1_1;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS)
        goto done;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
    if (result != VK_SUCCESS || !physical_count) {
        if (result == VK_SUCCESS)
            result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    queue_family = find_graphics_queue(physical);
    if (queue_family == UINT32_MAX)
        goto done;

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
    if (result != VK_SUCCESS)
        goto done;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    memset(&layout_info, 0, sizeof(layout_info));
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    result = vkCreatePipelineLayout(device, &layout_info, NULL, &layout);
    if (result != VK_SUCCESS)
        goto done;

    memset(&attachment, 0, sizeof(attachment));
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    if (result != VK_SUCCESS)
        goto done;

    result = create_shader(device, vertex_code, vertex_size, &vertex);
    if (result != VK_SUCCESS)
        goto done;
    result = create_shader(device, fragment_code, fragment_size, &fragment);
    if (result != VK_SUCCESS)
        goto done;

    memset(&cache_info, 0, sizeof(cache_info));
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    result = vkCreatePipelineCache(device, &cache_info, NULL, &cold_cache);
    if (result != VK_SUCCESS)
        goto done;

    pipelines = calloc(iterations, sizeof(*pipelines));
    if (!pipelines) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }

    /* Cold: compile the same pipeline repeatedly into the cold cache. */
    stage = "cold-loop";
    fprintf(stderr, "CACHEREUSE COLD START\n");
    fflush(stderr);
    begin = now_ns();
    for (i = 0; i < iterations; i++) {
        result = create_pipeline(device, layout, render_pass, vertex, fragment,
                cold_cache, &pipelines[i]);
        if (i == 0) {
            fprintf(stderr, "CACHEREUSE COLD0: result=%d\n", result);
            fflush(stderr);
        }
        if (result != VK_SUCCESS)
            goto done;
    }
    cold_ms = ms_between(begin, now_ns());

    /* Maleoon compiles shaders lazily at first draw, so submit one draw to
     * force the pipeline cache to be populated before serializing it. */
    stage = "render1";
    result = render_pipeline(physical, device, queue, queue_family,
            render_pass, pipelines[0]);
    fprintf(stderr, "CACHEREUSE RENDER1: result=%d\n", result);
    fflush(stderr);
    if (result != VK_SUCCESS)
        goto done;

    /* Serialize the cold cache. */
    stage = "get1";
    result = vkGetPipelineCacheData(device, cold_cache, &cache_bytes, NULL);
    fprintf(stderr, "CACHEREUSE GET1: result=%d cacheBytes=%zu\n", result, cache_bytes);
    fflush(stderr);
    if (result != VK_SUCCESS || !cache_bytes)
        goto done;
    cache_data = malloc(cache_bytes);
    if (!cache_data) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    result = vkGetPipelineCacheData(device, cold_cache, &cache_bytes, cache_data);
    fprintf(stderr, "CACHEREUSE GET2: result=%d cacheBytes=%zu\n", result, cache_bytes);
    fflush(stderr);
    if (result != VK_SUCCESS)
        goto done;

    /* Build a warm cache from the serialized blob. */
    stage = "get2-done";
    memset(&cache_info, 0, sizeof(cache_info));
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache_info.initialDataSize = cache_bytes;
    cache_info.pInitialData = cache_data;
    result = vkCreatePipelineCache(device, &cache_info, NULL, &warm_cache);
    if (result != VK_SUCCESS)
        goto done;

    /* Destroy cold pipelines so warm creation is the only consumer. */
    stage = "warm-cache";
    for (i = 0; i < iterations; i++) {
        vkDestroyPipeline(device, pipelines[i], NULL);
        pipelines[i] = VK_NULL_HANDLE;
    }

    /* Warm: compile the same pipeline again into the warm cache. */
    stage = "warm-loop";
    begin = now_ns();
    for (i = 0; i < iterations; i++) {
        result = create_pipeline(device, layout, render_pass, vertex, fragment,
                warm_cache, &pipelines[i]);
        if (result != VK_SUCCESS)
            goto done;
    }
    warm_ms = ms_between(begin, now_ns());

    result = render_pipeline(physical, device, queue, queue_family,
            render_pass, pipelines[0]);
    fprintf(stderr, "CACHEREUSE RENDER2: result=%d\n", result);
    fflush(stderr);
    if (result != VK_SUCCESS)
        goto done;

    warm_ok = 1;
    result = VK_SUCCESS;

done:
    fprintf(stderr, "CACHEREUSE DIAG: result=%d coldMs=%.3f cacheBytes=%zu\n",
            result, cold_ms, cache_bytes);
    if (pipelines) {
        for (i = 0; i < iterations; i++)
            vkDestroyPipeline(device, pipelines[i], NULL);
        free(pipelines);
    }
    if (cache_data)
        free(cache_data);
    if (cold_cache)
        vkDestroyPipelineCache(device, cold_cache, NULL);
    if (warm_cache)
        vkDestroyPipelineCache(device, warm_cache, NULL);
    if (vertex)
        vkDestroyShaderModule(device, vertex, NULL);
    if (fragment)
        vkDestroyShaderModule(device, fragment, NULL);
    if (render_pass)
        vkDestroyRenderPass(device, render_pass, NULL);
    if (layout)
        vkDestroyPipelineLayout(device, layout, NULL);
    if (device)
        vkDestroyDevice(device, NULL);
    if (instance)
        vkDestroyInstance(instance, NULL);
    free((void *)vertex_code);
    free((void *)fragment_code);

    if (result == VK_SUCCESS)
        stage = "complete";
    emit_result(result_path, iterations, cold_ms, warm_ms, cache_bytes, warm_ok,
            result, stage);
    return result == VK_SUCCESS ? 0 : 1;
}
