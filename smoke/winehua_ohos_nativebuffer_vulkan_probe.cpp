#define VK_USE_PLATFORM_OHOS 1

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr VkExternalMemoryHandleTypeFlagBits kNativeBufferHandle =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
constexpr VkDeviceSize kProbeBytes = 1024 * 1024;

uint8_t Pattern(size_t index, uint8_t salt)
{
    uint32_t value = static_cast<uint32_t>(index) * 1664525u + 1013904223u + salt * 977u;
    value ^= value >> 13;
    return static_cast<uint8_t>(value >> 17);
}

const char* JsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string JsonEscape(const char* value)
{
    std::string out;
    for (const unsigned char c : std::string(value ? value : "")) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out += static_cast<char>(c);
            break;
        }
    }
    return out;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags memoryFlags = 0;
};

struct Result {
    std::string testId = "ohos-nativebuffer-vulkan";
    std::string windowSource = "none";
    VkResult instanceResult = VK_NOT_READY;
    VkResult deviceResult = VK_NOT_READY;
    VkResult imageFormatQueryResult = VK_NOT_READY;
    VkResult nativePropertyResult = VK_NOT_READY;
    VkResult externalBufferCreateResult = VK_NOT_READY;
    VkResult importAllocateResult = VK_NOT_READY;
    VkResult importBindResult = VK_NOT_READY;
    VkResult importedVkMapResult = VK_NOT_READY;
    VkResult persistentVkMapResult = VK_NOT_READY;
    VkResult persistentVkFlushResult = VK_NOT_READY;
    VkResult persistentVkInvalidateResult = VK_NOT_READY;
    VkPhysicalDeviceProperties deviceProperties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    uint32_t queueFamily = UINT32_MAX;
    bool externalMemoryExtension = false;
    bool legacyNativeBufferExtension = false;
    bool foreignQueueExtension = false;
    bool getNativeBufferPropertiesSymbol = false;
    bool getMemoryNativeBufferSymbol = false;
    bool acquireImageSymbol = false;
    bool releaseImageSymbol = false;
    VkExternalMemoryFeatureFlags transferBufferExternalFeatures = 0;
    VkExternalMemoryFeatureFlags fullBufferExternalFeatures = 0;
    VkBufferUsageFlags importedBufferUsage = 0;
    VkExternalMemoryFeatureFlags imageExternalFeatures = 0;
    uint64_t imageNativeUsage = 0;
    bool nativeAllocated = false;
    uint32_t nativeAllocationAttempts = 0;
    bool consumerSurfaceCreated = false;
    int32_t consumerDefaultSizeResult = -1;
    int32_t consumerDefaultUsageResult = -1;
    int32_t consumerListenerResult = -1;
    bool producerWindowAcquired = false;
    int32_t producerGeometryResult = -1;
    int32_t producerFormatResult = -1;
    int32_t producerUsageResult = -1;
    int32_t producerTimeoutResult = -1;
    int32_t producerQueueSizeResult = -1;
    int32_t producerQueueSize = -1;
    int32_t producerSurfaceIdResult = -1;
    uint64_t producerSurfaceId = 0;
    int32_t producerPreallocateResult = -1;
    int32_t producerRequestResult = -1;
    int32_t nativeFromWindowResult = -1;
    int32_t producerAbortResult = -1;
    int32_t eglGetDisplayResult = -1;
    int32_t eglInitializeResult = -1;
    int32_t eglMajor = 0;
    int32_t eglMinor = 0;
    int32_t eglBindApiResult = -1;
    int32_t eglChooseConfigResult = -1;
    int32_t eglCreateContextResult = -1;
    int32_t eglCreateSurfaceResult = -1;
    int32_t eglMakeCurrentResult = -1;
    int32_t eglSwapIntervalResult = -1;
    uint32_t glClearError = GL_NO_ERROR;
    int32_t eglSwapResult = -1;
    int32_t eglSwapError = EGL_SUCCESS;
    int32_t lastFlushedBufferResult = -1;
    bool lastFlushedBufferPresent = false;
    int32_t flushedFromWindowResult = -1;
    int32_t lastFlushedBufferReleaseResult = -1;
    bool eglProducerConnected = false;
    uint32_t consumerFrameSignals = 0;
    std::string nativeSource = "none";
    uint32_t nativeSequence = 0;
    int nativeMapResult = -1;
    int nativeUnmapResult = -1;
    bool nativeRemapPassed = false;
    int32_t nativeWidth = 0;
    int32_t nativeHeight = 0;
    int32_t nativeFormat = 0;
    int32_t nativeUsage = 0;
    int32_t nativeStride = 0;
    VkDeviceSize nativeAllocationSize = 0;
    uint32_t nativeMemoryTypeBits = 0;
    uint32_t bufferMemoryTypeBits = 0;
    uint32_t selectedMemoryType = UINT32_MAX;
    VkMemoryPropertyFlags selectedMemoryFlags = 0;
    bool synchronizedCpuToGpuPassed = false;
    bool synchronizedGpuToCpuPassed = false;
    bool persistentCpuToGpuPassed = false;
    bool persistentGpuToCpuPassed = false;
    bool persistentVkCpuToGpuPassed = false;
    bool persistentVkGpuToCpuPassed = false;
    bool explicitCacheMaintenanceRequired = false;
    uint64_t synchronizedCpuToGpuMismatches = UINT64_MAX;
    uint64_t synchronizedGpuToCpuMismatches = UINT64_MAX;
    uint64_t persistentCpuToGpuMismatches = UINT64_MAX;
    uint64_t persistentGpuToCpuMismatches = UINT64_MAX;
    uint64_t persistentVkCpuToGpuMismatches = UINT64_MAX;
    uint64_t persistentVkGpuToCpuMismatches = UINT64_MAX;
    std::string fatal;
};

class NativeBufferVulkanProbe {
public:
    NativeBufferVulkanProbe(OHNativeWindow* producerWindow = nullptr,
                            bool ownsProducerWindow = false)
        : producerWindow_(producerWindow),
          ownsProducerWindow_(ownsProducerWindow),
          externalProducerWindow_(producerWindow != nullptr)
    {
        if (externalProducerWindow_) {
            result_.testId = "host-nativebuffer";
            result_.windowSource = "xcomponent-surface";
            result_.producerWindowAcquired = true;
        }
    }

    ~NativeBufferVulkanProbe()
    {
        Cleanup();
    }

    int Run(const char* outputPath)
    {
        if (InitVulkan()) {
            QueryExternalCapabilities();
            RunNativeBufferMapping();
            RunBufferImport();
        }
        ReleaseProbeBacking();
        Emit(outputPath);
        const bool persistentVk = result_.persistentVkCpuToGpuPassed &&
                                  result_.persistentVkGpuToCpuPassed;
        const bool synchronized = result_.synchronizedCpuToGpuPassed &&
                                  result_.synchronizedGpuToCpuPassed;
        const bool fullUpload = result_.persistentVkCpuToGpuPassed &&
            (result_.fullBufferExternalFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
        if (!result_.fatal.empty()) return 1;
        if (fullUpload && result_.persistentVkGpuToCpuPassed) return 0;
        return (persistentVk || result_.persistentVkCpuToGpuPassed ||
                result_.persistentVkGpuToCpuPassed || synchronized) ? 2 : 3;
    }

private:
    static void OnConsumerFrameAvailable(void* context)
    {
        auto* probe = static_cast<NativeBufferVulkanProbe*>(context);
        probe->consumerFrameSignals_.fetch_add(1, std::memory_order_relaxed);
    }

    bool InitVulkan()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WineHua OHOS NativeBuffer Vulkan Probe";
        app.applicationVersion = 1;
        app.pEngineName = "none";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        result_.instanceResult = vkCreateInstance(&instanceInfo, nullptr, &instance_);
        if (result_.instanceResult != VK_SUCCESS) {
            result_.fatal = "vkCreateInstance failed";
            return false;
        }

        uint32_t physicalCount = 0;
        if (vkEnumeratePhysicalDevices(instance_, &physicalCount, nullptr) != VK_SUCCESS ||
            physicalCount == 0) {
            result_.fatal = "no Vulkan physical device";
            return false;
        }
        std::vector<VkPhysicalDevice> devices(physicalCount);
        if (vkEnumeratePhysicalDevices(instance_, &physicalCount, devices.data()) != VK_SUCCESS) {
            result_.fatal = "vkEnumeratePhysicalDevices failed";
            return false;
        }

        for (VkPhysicalDevice candidate : devices) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    physical_ = candidate;
                    result_.queueFamily = i;
                    break;
                }
            }
            if (physical_ != VK_NULL_HANDLE) break;
        }
        if (physical_ == VK_NULL_HANDLE) {
            result_.fatal = "no graphics queue family";
            return false;
        }

        vkGetPhysicalDeviceProperties(physical_, &result_.deviceProperties);
        vkGetPhysicalDeviceMemoryProperties(physical_, &result_.memoryProperties);

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount)
            vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount,
                                                 extensions.data());
        result_.externalMemoryExtension = HasExtension(
            extensions, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME);
        result_.legacyNativeBufferExtension = HasExtension(
            extensions, VK_OHOS_NATIVE_BUFFER_EXTENSION_NAME);
        result_.foreignQueueExtension = HasExtension(
            extensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

        std::vector<const char*> enabledExtensions;
        if (result_.externalMemoryExtension)
            enabledExtensions.push_back(VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME);
        if (result_.foreignQueueExtension)
            enabledExtensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = result_.queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();
        result_.deviceResult = vkCreateDevice(physical_, &deviceInfo, nullptr, &device_);
        if (result_.deviceResult != VK_SUCCESS) {
            result_.fatal = "vkCreateDevice failed";
            return false;
        }
        vkGetDeviceQueue(device_, result_.queueFamily, 0, &queue_);

        getNativeBufferProperties_ = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(
            vkGetDeviceProcAddr(device_, "vkGetNativeBufferPropertiesOHOS"));
        getMemoryNativeBuffer_ = reinterpret_cast<PFN_vkGetMemoryNativeBufferOHOS>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryNativeBufferOHOS"));
        acquireImage_ = reinterpret_cast<PFN_vkAcquireImageOHOS>(
            vkGetDeviceProcAddr(device_, "vkAcquireImageOHOS"));
        releaseImage_ = reinterpret_cast<PFN_vkQueueSignalReleaseImageOHOS>(
            vkGetDeviceProcAddr(device_, "vkQueueSignalReleaseImageOHOS"));
        result_.getNativeBufferPropertiesSymbol = getNativeBufferProperties_ != nullptr;
        result_.getMemoryNativeBufferSymbol = getMemoryNativeBuffer_ != nullptr;
        result_.acquireImageSymbol = acquireImage_ != nullptr;
        result_.releaseImageSymbol = releaseImage_ != nullptr;

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = result_.queueFamily;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
            result_.fatal = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandInfo, &commandBuffer_) != VK_SUCCESS) {
            result_.fatal = "vkAllocateCommandBuffers failed";
            return false;
        }
        return true;
    }

    void QueryExternalCapabilities()
    {
        VkPhysicalDeviceExternalBufferInfo bufferInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        bufferInfo.handleType = kNativeBufferHandle;
        VkExternalBufferProperties bufferProperties{
            VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkGetPhysicalDeviceExternalBufferProperties(physical_, &bufferInfo, &bufferProperties);
        result_.transferBufferExternalFeatures =
            bufferProperties.externalMemoryProperties.externalMemoryFeatures;
        bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        bufferProperties = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(physical_, &bufferInfo, &bufferProperties);
        result_.fullBufferExternalFeatures =
            bufferProperties.externalMemoryProperties.externalMemoryFeatures;

        VkPhysicalDeviceExternalImageFormatInfo externalImageInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalImageInfo.handleType = kNativeBufferHandle;
        VkPhysicalDeviceImageFormatInfo2 imageInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        imageInfo.pNext = &externalImageInfo;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.type = VK_IMAGE_TYPE_2D;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        VkNativeBufferUsageOHOS nativeUsage{VK_STRUCTURE_TYPE_NATIVE_BUFFER_USAGE_OHOS};
        VkExternalImageFormatProperties externalImageProperties{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        externalImageProperties.pNext = &nativeUsage;
        VkImageFormatProperties2 imageProperties{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        imageProperties.pNext = &externalImageProperties;
        result_.imageFormatQueryResult = vkGetPhysicalDeviceImageFormatProperties2(
            physical_, &imageInfo, &imageProperties);
        if (result_.imageFormatQueryResult == VK_SUCCESS) {
            result_.imageExternalFeatures =
                externalImageProperties.externalMemoryProperties.externalMemoryFeatures;
            result_.imageNativeUsage = nativeUsage.OHOSNativeBufferUsage;
        }
    }

    void RunNativeBufferMapping()
    {
        struct AllocationCandidate {
            int32_t width;
            int32_t height;
            int32_t format;
            int32_t usage;
        };
        const int32_t usageCandidates[] = {
            NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_MEM_MMZ_CACHE |
                NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE,
            NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER |
                NATIVEBUFFER_USAGE_HW_TEXTURE,
            NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                NATIVEBUFFER_USAGE_MEM_DMA,
        };
        constexpr size_t usageCandidateCount =
            sizeof(usageCandidates) / sizeof(usageCandidates[0]);
        std::vector<AllocationCandidate> candidates;
        candidates.reserve(2 * usageCandidateCount);
        for (int32_t usage : usageCandidates)
            candidates.push_back({static_cast<int32_t>(kProbeBytes), 1,
                                  NATIVEBUFFER_PIXEL_FMT_BLOB, usage});
        for (int32_t usage : usageCandidates)
            candidates.push_back({512, 512, NATIVEBUFFER_PIXEL_FMT_RGBA_8888, usage});

        constexpr uint64_t surfaceUsage =
            NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
            NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER |
            NATIVEBUFFER_USAGE_HW_TEXTURE;

        if (!producerWindow_) {
            consumerSurface_ = OH_ConsumerSurface_Create();
            result_.consumerSurfaceCreated = consumerSurface_ != nullptr;
            if (consumerSurface_) {
                result_.consumerDefaultSizeResult = OH_ConsumerSurface_SetDefaultSize(
                    consumerSurface_, 512, 512);
                result_.consumerDefaultUsageResult = OH_ConsumerSurface_SetDefaultUsage(
                    consumerSurface_, surfaceUsage);
                OH_OnFrameAvailableListener listener{};
                listener.context = this;
                listener.onFrameAvailable = OnConsumerFrameAvailable;
                result_.consumerListenerResult = OH_NativeImage_SetOnFrameAvailableListener(
                    consumerSurface_, listener);
                consumerListenerSet_ = result_.consumerListenerResult == 0;
                producerWindow_ = OH_NativeImage_AcquireNativeWindow(consumerSurface_);
                result_.producerWindowAcquired = producerWindow_ != nullptr;
                if (producerWindow_) result_.windowSource = "consumer-surface";
            }
        }
        if (producerWindow_) {
            result_.producerGeometryResult = OH_NativeWindow_NativeWindowHandleOpt(
                producerWindow_, SET_BUFFER_GEOMETRY, 512, 512);
            result_.producerFormatResult = OH_NativeWindow_NativeWindowHandleOpt(
                producerWindow_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
            result_.producerUsageResult = OH_NativeWindow_NativeWindowHandleOpt(
                producerWindow_, SET_USAGE, surfaceUsage);
            result_.producerTimeoutResult = OH_NativeWindow_NativeWindowHandleOpt(
                producerWindow_, SET_TIMEOUT, 0);
            result_.producerQueueSizeResult = OH_NativeWindow_NativeWindowHandleOpt(
                producerWindow_, GET_BUFFERQUEUE_SIZE, &result_.producerQueueSize);
            result_.producerSurfaceIdResult = OH_NativeWindow_GetSurfaceId(
                producerWindow_, &result_.producerSurfaceId);
            result_.producerPreallocateResult = OH_NativeWindow_PreAllocBuffers(
                producerWindow_, 3);
            RunEglProducer(surfaceUsage);
            if (!nativeBuffer_) {
                result_.producerRequestResult = OH_NativeWindow_NativeWindowRequestBuffer(
                    producerWindow_, &windowBuffer_, &windowFenceFd_);
                if (result_.producerRequestResult == 0 && windowBuffer_) {
                    if (windowFenceFd_ >= 0) {
                        close(windowFenceFd_);
                        windowFenceFd_ = -1;
                    }
                    result_.nativeFromWindowResult = OH_NativeBuffer_FromNativeWindowBuffer(
                        windowBuffer_, &nativeBuffer_);
                    if (result_.nativeFromWindowResult == 0 && nativeBuffer_)
                        result_.nativeSource = result_.windowSource;
                }
            }
        }
        if (!nativeBuffer_) {
            OH_NativeBuffer_Config config{};
            for (const auto& candidate : candidates) {
                ++result_.nativeAllocationAttempts;
                config = {candidate.width, candidate.height, candidate.format,
                          candidate.usage, 0};
                nativeBuffer_ = OH_NativeBuffer_Alloc(&config);
                if (nativeBuffer_) {
                    result_.nativeSource = "direct-alloc";
                    break;
                }
            }
        }
        result_.nativeAllocated = nativeBuffer_ != nullptr;
        if (!nativeBuffer_) return;

        result_.nativeSequence = OH_NativeBuffer_GetSeqNum(nativeBuffer_);
        OH_NativeBuffer_Config actual{};
        OH_NativeBuffer_GetConfig(nativeBuffer_, &actual);
        result_.nativeWidth = actual.width;
        result_.nativeHeight = actual.height;
        result_.nativeFormat = actual.format;
        result_.nativeUsage = actual.usage;
        result_.nativeStride = actual.stride;

        void* mapped = nullptr;
        result_.nativeMapResult = OH_NativeBuffer_Map(nativeBuffer_, &mapped);
        if (result_.nativeMapResult != 0 || !mapped) return;
        Fill(mapped, 0x11);
        result_.nativeUnmapResult = OH_NativeBuffer_Unmap(nativeBuffer_);
        if (result_.nativeUnmapResult != 0) return;

        mapped = nullptr;
        if (OH_NativeBuffer_Map(nativeBuffer_, &mapped) != 0 || !mapped) return;
        result_.nativeRemapPassed = Verify(mapped, 0x11) == 0;
        OH_NativeBuffer_Unmap(nativeBuffer_);
    }

    void RunEglProducer(uint64_t usage)
    {
        if (!producerWindow_) return;
        if (!externalProducerWindow_) {
            // This native child is not a guest Wayland/VirGL process. Inherit
            // no surfaceless selector from the application graphics helpers.
            unsetenv("EGL_PLATFORM");
            unsetenv("WAYLAND_DISPLAY");
        }
        OH_NativeWindow_NativeWindowHandleOpt(producerWindow_, SET_USAGE, usage);
        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        result_.eglGetDisplayResult = eglDisplay_ != EGL_NO_DISPLAY ? 0 : -1;
        if (eglDisplay_ == EGL_NO_DISPLAY) return;
        result_.eglInitializeResult = eglInitialize(
            eglDisplay_, &result_.eglMajor, &result_.eglMinor)
            == EGL_TRUE ? 0 : eglGetError();
        if (result_.eglInitializeResult != 0) return;
        result_.eglBindApiResult = eglBindAPI(EGL_OPENGL_ES_API)
            == EGL_TRUE ? 0 : eglGetError();
        if (result_.eglBindApiResult != 0) return;
        const EGLint attrs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint count = 0;
        EGLConfig config = nullptr;
        result_.eglChooseConfigResult = eglChooseConfig(
            eglDisplay_, attrs, &config, 1, &count) == EGL_TRUE && count ? 0 : eglGetError();
        if (result_.eglChooseConfigResult != 0) return;
        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        eglContext_ = eglCreateContext(eglDisplay_, config, EGL_NO_CONTEXT, contextAttrs);
        result_.eglCreateContextResult = eglContext_ != EGL_NO_CONTEXT ? 0 : eglGetError();
        if (result_.eglCreateContextResult != 0) return;
        eglSurface_ = eglCreateWindowSurface(
            eglDisplay_, config, reinterpret_cast<EGLNativeWindowType>(producerWindow_), nullptr);
        result_.eglCreateSurfaceResult = eglSurface_ != EGL_NO_SURFACE ? 0 : eglGetError();
        if (result_.eglCreateSurfaceResult != 0) return;
        result_.eglMakeCurrentResult = eglMakeCurrent(
            eglDisplay_, eglSurface_, eglSurface_, eglContext_) == EGL_TRUE ? 0 : eglGetError();
        if (result_.eglMakeCurrentResult != 0) return;
        result_.eglSwapIntervalResult = eglSwapInterval(eglDisplay_, 0)
            == EGL_TRUE ? 0 : eglGetError();
        glClearColor(0.125f, 0.25f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        result_.glClearError = glGetError();
        if (result_.glClearError != GL_NO_ERROR) return;
        result_.eglSwapResult = eglSwapBuffers(eglDisplay_, eglSurface_) == EGL_TRUE ? 0 : -1;
        result_.eglSwapError = result_.eglSwapResult == 0 ? EGL_SUCCESS : eglGetError();
        result_.eglProducerConnected = result_.eglSwapResult == 0;
        if (!result_.eglProducerConnected) return;
        float matrix[16]{};
        int fenceFd = -1;
        OHNativeWindowBuffer* flushed = nullptr;
        result_.lastFlushedBufferResult = OH_NativeWindow_GetLastFlushedBufferV2(
            producerWindow_, &flushed, &fenceFd, matrix);
        if (fenceFd >= 0) close(fenceFd);
        result_.lastFlushedBufferPresent = flushed != nullptr;
        if (result_.lastFlushedBufferResult == 0 && flushed) {
            flushedWindowBuffer_ = flushed;
            result_.flushedFromWindowResult = OH_NativeBuffer_FromNativeWindowBuffer(
                flushedWindowBuffer_, &nativeBuffer_);
            if (result_.flushedFromWindowResult == 0 && nativeBuffer_)
                result_.nativeSource = externalProducerWindow_
                    ? "egl-flushed-xcomponent" : "egl-flushed-surface";
        }
    }

    void ReleaseEglProducer()
    {
        if (eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
            if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
            // The main App shares EGL_DEFAULT_DISPLAY across renderers. Keep
            // it initialized when this probe runs on a real XComponent.
            if (!externalProducerWindow_) eglTerminate(eglDisplay_);
        }
        eglDisplay_ = EGL_NO_DISPLAY;
        eglSurface_ = EGL_NO_SURFACE;
        eglContext_ = EGL_NO_CONTEXT;
    }

    void RunBufferImport()
    {
        if (!nativeBuffer_ || !result_.externalMemoryExtension ||
            !result_.getNativeBufferPropertiesSymbol ||
            !(result_.transferBufferExternalFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            return;
        }

        VkNativeBufferPropertiesOHOS nativeProperties{
            VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
        result_.nativePropertyResult = getNativeBufferProperties_(
            device_, nativeBuffer_, &nativeProperties);
        if (result_.nativePropertyResult != VK_SUCCESS) return;
        result_.nativeAllocationSize = nativeProperties.allocationSize;
        result_.nativeMemoryTypeBits = nativeProperties.memoryTypeBits;

        VkExternalMemoryBufferCreateInfo externalInfo{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        externalInfo.handleTypes = kNativeBufferHandle;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.pNext = &externalInfo;
        bufferInfo.size = kProbeBytes;
        const VkBufferUsageFlags transferUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkBufferUsageFlags fullUsage = transferUsage |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        result_.importedBufferUsage =
            (result_.fullBufferExternalFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
                ? fullUsage : transferUsage;
        bufferInfo.usage = result_.importedBufferUsage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result_.externalBufferCreateResult = vkCreateBuffer(
            device_, &bufferInfo, nullptr, &importedBuffer_);
        if (result_.externalBufferCreateResult != VK_SUCCESS) return;

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, importedBuffer_, &requirements);
        result_.bufferMemoryTypeBits = requirements.memoryTypeBits;
        result_.selectedMemoryType = FindMemoryType(
            requirements.memoryTypeBits & nativeProperties.memoryTypeBits,
            0, 0, &result_.selectedMemoryFlags);
        if (result_.selectedMemoryType == UINT32_MAX) return;

        VkMemoryDedicatedAllocateInfo dedicated{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = importedBuffer_;
        VkImportNativeBufferInfoOHOS importInfo{
            VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
        importInfo.pNext = &dedicated;
        importInfo.buffer = nativeBuffer_;
        VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocationInfo.pNext = &importInfo;
        allocationInfo.allocationSize = nativeProperties.allocationSize;
        allocationInfo.memoryTypeIndex = result_.selectedMemoryType;
        result_.importAllocateResult = vkAllocateMemory(
            device_, &allocationInfo, nullptr, &importedMemory_);
        if (result_.importAllocateResult != VK_SUCCESS) return;
        result_.importBindResult = vkBindBufferMemory(
            device_, importedBuffer_, importedMemory_, 0);
        if (result_.importBindResult != VK_SUCCESS) return;

        void* importedMapping = nullptr;
        result_.importedVkMapResult = vkMapMemory(
            device_, importedMemory_, 0, VK_WHOLE_SIZE, 0, &importedMapping);
        if (result_.importedVkMapResult == VK_SUCCESS)
            vkUnmapMemory(device_, importedMemory_);

        if (!CreateHostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload_) ||
            !CreateHostBuffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_)) {
            return;
        }

        RunSynchronizedVisibility();
        RunPersistentVisibility();
        RunPersistentVulkanVisibility();
    }

    void RunSynchronizedVisibility()
    {
        void* nativeMapping = nullptr;
        if (OH_NativeBuffer_Map(nativeBuffer_, &nativeMapping) == 0 && nativeMapping) {
            Fill(nativeMapping, 0x31);
            if (OH_NativeBuffer_Unmap(nativeBuffer_) == 0 && CopyImportedToReadback()) {
                result_.synchronizedCpuToGpuMismatches = VerifyHostBuffer(readback_, 0x31);
                result_.synchronizedCpuToGpuPassed =
                    result_.synchronizedCpuToGpuMismatches == 0;
            }
        }

        if (FillHostBuffer(upload_, 0x52) && CopyUploadToImported()) {
            nativeMapping = nullptr;
            if (OH_NativeBuffer_Map(nativeBuffer_, &nativeMapping) == 0 && nativeMapping) {
                result_.synchronizedGpuToCpuMismatches = Verify(nativeMapping, 0x52);
                result_.synchronizedGpuToCpuPassed =
                    result_.synchronizedGpuToCpuMismatches == 0;
                OH_NativeBuffer_Unmap(nativeBuffer_);
            }
        }
    }

    void RunPersistentVisibility()
    {
        void* nativeMapping = nullptr;
        if (OH_NativeBuffer_Map(nativeBuffer_, &nativeMapping) != 0 || !nativeMapping) return;

        Fill(nativeMapping, 0x73);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (CopyImportedToReadback()) {
            result_.persistentCpuToGpuMismatches = VerifyHostBuffer(readback_, 0x73);
            result_.persistentCpuToGpuPassed = result_.persistentCpuToGpuMismatches == 0;
        }

        if (FillHostBuffer(upload_, 0x94) && CopyUploadToImported()) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            result_.persistentGpuToCpuMismatches = Verify(nativeMapping, 0x94);
            result_.persistentGpuToCpuPassed = result_.persistentGpuToCpuMismatches == 0;
        }
        OH_NativeBuffer_Unmap(nativeBuffer_);
    }

    void RunPersistentVulkanVisibility()
    {
        void* mapping = nullptr;
        result_.persistentVkMapResult = vkMapMemory(
            device_, importedMemory_, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (result_.persistentVkMapResult != VK_SUCCESS || !mapping) return;

        result_.explicitCacheMaintenanceRequired =
            !(result_.selectedMemoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        Fill(mapping, 0xa5);
        if (result_.explicitCacheMaintenanceRequired) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = importedMemory_;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            result_.persistentVkFlushResult =
                vkFlushMappedMemoryRanges(device_, 1, &range);
        } else {
            result_.persistentVkFlushResult = VK_SUCCESS;
        }
        if (result_.persistentVkFlushResult == VK_SUCCESS && CopyImportedToReadback()) {
            result_.persistentVkCpuToGpuMismatches = VerifyHostBuffer(readback_, 0xa5);
            result_.persistentVkCpuToGpuPassed =
                result_.persistentVkCpuToGpuMismatches == 0;
        }

        if (FillHostBuffer(upload_, 0xc7) && CopyUploadToImported()) {
            if (result_.explicitCacheMaintenanceRequired) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = importedMemory_;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                result_.persistentVkInvalidateResult =
                    vkInvalidateMappedMemoryRanges(device_, 1, &range);
            } else {
                result_.persistentVkInvalidateResult = VK_SUCCESS;
            }
            if (result_.persistentVkInvalidateResult == VK_SUCCESS) {
                result_.persistentVkGpuToCpuMismatches = Verify(mapping, 0xc7);
                result_.persistentVkGpuToCpuPassed =
                    result_.persistentVkGpuToCpuMismatches == 0;
            }
        }
        vkUnmapMemory(device_, importedMemory_);
    }

    uint32_t ExternalQueueFamily() const
    {
        return result_.foreignQueueExtension ? VK_QUEUE_FAMILY_FOREIGN_EXT :
                                               VK_QUEUE_FAMILY_EXTERNAL;
    }

    bool CopyImportedToReadback()
    {
        if (!BeginCommands()) return false;
        BufferOwnershipBarrier(ExternalQueueFamily(), result_.queueFamily,
                               0, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy copy{0, 0, kProbeBytes};
        vkCmdCopyBuffer(commandBuffer_, importedBuffer_, readback_.buffer, 1, &copy);
        BufferOwnershipBarrier(result_.queueFamily, ExternalQueueFamily(),
                               VK_ACCESS_TRANSFER_READ_BIT, 0,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return EndAndSubmit();
    }

    bool CopyUploadToImported()
    {
        if (!BeginCommands()) return false;
        BufferOwnershipBarrier(ExternalQueueFamily(), result_.queueFamily,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy copy{0, 0, kProbeBytes};
        vkCmdCopyBuffer(commandBuffer_, upload_.buffer, importedBuffer_, 1, &copy);
        BufferOwnershipBarrier(result_.queueFamily, ExternalQueueFamily(),
                               VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return EndAndSubmit();
    }

    void BufferOwnershipBarrier(uint32_t sourceFamily, uint32_t destinationFamily,
                                VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                VkPipelineStageFlags sourceStage,
                                VkPipelineStageFlags destinationStage)
    {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = destinationAccess;
        barrier.srcQueueFamilyIndex = sourceFamily;
        barrier.dstQueueFamilyIndex = destinationFamily;
        barrier.buffer = importedBuffer_;
        barrier.offset = 0;
        barrier.size = kProbeBytes;
        vkCmdPipelineBarrier(commandBuffer_, sourceStage, destinationStage, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
    }

    bool BeginCommands()
    {
        if (vkResetCommandBuffer(commandBuffer_, 0) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(commandBuffer_, &begin) == VK_SUCCESS;
    }

    bool EndAndSubmit()
    {
        if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) return false;
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        const VkResult submitResult = vkQueueSubmit(queue_, 1, &submit, fence);
        const VkResult waitResult = submitResult == VK_SUCCESS
            ? vkWaitForFences(device_, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000)
            : submitResult;
        vkDestroyFence(device_, fence, nullptr);
        return submitResult == VK_SUCCESS && waitResult == VK_SUCCESS;
    }

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred,
                            VkMemoryPropertyFlags* selectedFlags) const
    {
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < result_.memoryProperties.memoryTypeCount; ++i) {
            if (!(bits & (1u << i))) continue;
            const VkMemoryPropertyFlags flags =
                result_.memoryProperties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required) continue;
            if (fallback == UINT32_MAX) fallback = i;
            if ((flags & preferred) == preferred) {
                if (selectedFlags) *selectedFlags = flags;
                return i;
            }
        }
        if (fallback != UINT32_MAX && selectedFlags)
            *selectedFlags = result_.memoryProperties.memoryTypes[fallback].propertyFlags;
        return fallback;
    }

    bool CreateHostBuffer(VkBufferUsageFlags usage, HostBuffer& output)
    {
        output.size = kProbeBytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = output.size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &info, nullptr, &output.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, output.buffer, &requirements);
        const uint32_t type = FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &output.memoryFlags);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &output.memory) != VK_SUCCESS)
            return false;
        return vkBindBufferMemory(device_, output.buffer, output.memory, 0) == VK_SUCCESS;
    }

    bool FillHostBuffer(const HostBuffer& buffer, uint8_t salt)
    {
        void* mapped = nullptr;
        if (vkMapMemory(device_, buffer.memory, 0, buffer.size, 0, &mapped) != VK_SUCCESS)
            return false;
        Fill(mapped, salt);
        if (!(buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = buffer.memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        vkUnmapMemory(device_, buffer.memory);
        return true;
    }

    uint64_t VerifyHostBuffer(const HostBuffer& buffer, uint8_t salt)
    {
        void* mapped = nullptr;
        if (vkMapMemory(device_, buffer.memory, 0, buffer.size, 0, &mapped) != VK_SUCCESS)
            return UINT64_MAX;
        if (!(buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = buffer.memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(device_, 1, &range);
        }
        const uint64_t mismatches = Verify(mapped, salt);
        vkUnmapMemory(device_, buffer.memory);
        return mismatches;
    }

    static void Fill(void* mapping, uint8_t salt)
    {
        auto* bytes = static_cast<uint8_t*>(mapping);
        for (size_t i = 0; i < static_cast<size_t>(kProbeBytes); ++i)
            bytes[i] = Pattern(i, salt);
    }

    static uint64_t Verify(const void* mapping, uint8_t salt)
    {
        const auto* bytes = static_cast<const uint8_t*>(mapping);
        uint64_t mismatches = 0;
        for (size_t i = 0; i < static_cast<size_t>(kProbeBytes); ++i)
            mismatches += bytes[i] != Pattern(i, salt);
        return mismatches;
    }

    void Emit(const char* outputPath) const
    {
        const bool synchronized = result_.synchronizedCpuToGpuPassed &&
                                  result_.synchronizedGpuToCpuPassed;
        const bool fullUsage = result_.fullBufferExternalFeatures &
                               VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        const bool fullUpload = result_.persistentVkCpuToGpuPassed && fullUsage;
        const bool readback = result_.persistentVkGpuToCpuPassed;
        const bool anyDirect = result_.persistentVkCpuToGpuPassed || readback;
        const bool copyOnly = !anyDirect && synchronized;
        const char* status = !result_.fatal.empty() ? "FAIL" :
                             (fullUpload && readback) ? "PASS" :
                             (anyDirect || synchronized) ? "PARTIAL" : "UNSUPPORTED";
        std::ostringstream out;
        out << "{\n"
            << "  \"schemaVersion\":1,\n"
            << "  \"testId\":\"" << result_.testId << "\",\n"
            << "  \"status\":\"" << status << "\",\n"
            << "  \"fatal\":\"" << JsonEscape(result_.fatal.c_str()) << "\",\n"
            << "  \"device\":{\"name\":\""
            << JsonEscape(result_.deviceProperties.deviceName)
            << "\",\"vendorId\":" << result_.deviceProperties.vendorID
            << ",\"deviceId\":" << result_.deviceProperties.deviceID
            << ",\"apiVersion\":" << result_.deviceProperties.apiVersion
            << ",\"driverVersion\":" << result_.deviceProperties.driverVersion
            << ",\"queueFamily\":" << result_.queueFamily << "},\n"
            << "  \"extensions\":{\"VK_OHOS_external_memory\":"
            << JsonBool(result_.externalMemoryExtension)
            << ",\"VK_OHOS_native_buffer\":"
            << JsonBool(result_.legacyNativeBufferExtension)
            << ",\"VK_EXT_queue_family_foreign\":"
            << JsonBool(result_.foreignQueueExtension) << "},\n"
            << "  \"symbols\":{\"vkGetNativeBufferPropertiesOHOS\":"
            << JsonBool(result_.getNativeBufferPropertiesSymbol)
            << ",\"vkGetMemoryNativeBufferOHOS\":"
            << JsonBool(result_.getMemoryNativeBufferSymbol)
            << ",\"vkAcquireImageOHOS\":" << JsonBool(result_.acquireImageSymbol)
            << ",\"vkQueueSignalReleaseImageOHOS\":"
            << JsonBool(result_.releaseImageSymbol) << "},\n"
            << "  \"externalMemory\":{\"transferBufferFeatures\":"
            << result_.transferBufferExternalFeatures
            << ",\"fullBufferFeatures\":" << result_.fullBufferExternalFeatures
            << ",\"importedBufferUsage\":" << result_.importedBufferUsage
            << ",\"imageQueryResult\":" << result_.imageFormatQueryResult
            << ",\"imageFeatures\":" << result_.imageExternalFeatures
            << ",\"imageNativeUsage\":" << result_.imageNativeUsage << "},\n"
            << "  \"nativeBuffer\":{\"allocated\":" << JsonBool(result_.nativeAllocated)
            << ",\"source\":\"" << result_.nativeSource << "\""
            << ",\"allocationAttempts\":" << result_.nativeAllocationAttempts
            << ",\"sequence\":" << result_.nativeSequence
            << ",\"mapResult\":" << result_.nativeMapResult
            << ",\"unmapResult\":" << result_.nativeUnmapResult
            << ",\"remapPassed\":" << JsonBool(result_.nativeRemapPassed)
            << ",\"width\":" << result_.nativeWidth
            << ",\"height\":" << result_.nativeHeight
            << ",\"format\":" << result_.nativeFormat
            << ",\"usage\":" << result_.nativeUsage
            << ",\"stride\":" << result_.nativeStride << "},\n"
            << "  \"consumerSurface\":{\"created\":"
            << JsonBool(result_.consumerSurfaceCreated)
            << ",\"windowSource\":\"" << result_.windowSource << "\""
            << ",\"defaultSizeResult\":" << result_.consumerDefaultSizeResult
            << ",\"defaultUsageResult\":" << result_.consumerDefaultUsageResult
            << ",\"listenerResult\":" << result_.consumerListenerResult
            << ",\"windowAcquired\":" << JsonBool(result_.producerWindowAcquired)
            << ",\"geometryResult\":" << result_.producerGeometryResult
            << ",\"formatResult\":" << result_.producerFormatResult
            << ",\"usageResult\":" << result_.producerUsageResult
            << ",\"timeoutResult\":" << result_.producerTimeoutResult
            << ",\"queueSizeResult\":" << result_.producerQueueSizeResult
            << ",\"queueSize\":" << result_.producerQueueSize
            << ",\"surfaceIdResult\":" << result_.producerSurfaceIdResult
            << ",\"surfaceId\":" << result_.producerSurfaceId
            << ",\"preallocateResult\":" << result_.producerPreallocateResult
            << ",\"requestResult\":" << result_.producerRequestResult
            << ",\"fromWindowResult\":" << result_.nativeFromWindowResult
            << ",\"abortResult\":" << result_.producerAbortResult
            << ",\"frameSignals\":" << result_.consumerFrameSignals << "},\n"
            << "  \"eglProducer\":{\"getDisplayResult\":"
            << result_.eglGetDisplayResult
            << ",\"initializeResult\":" << result_.eglInitializeResult
            << ",\"major\":" << result_.eglMajor
            << ",\"minor\":" << result_.eglMinor
            << ",\"bindApiResult\":" << result_.eglBindApiResult
            << ",\"chooseConfigResult\":" << result_.eglChooseConfigResult
            << ",\"createContextResult\":" << result_.eglCreateContextResult
            << ",\"createSurfaceResult\":" << result_.eglCreateSurfaceResult
            << ",\"makeCurrentResult\":" << result_.eglMakeCurrentResult
            << ",\"swapIntervalResult\":" << result_.eglSwapIntervalResult
            << ",\"glClearError\":" << result_.glClearError
            << ",\"swapResult\":" << result_.eglSwapResult
            << ",\"swapError\":" << result_.eglSwapError
            << ",\"producerConnected\":" << JsonBool(result_.eglProducerConnected)
            << ",\"lastFlushedBufferResult\":"
            << result_.lastFlushedBufferResult
            << ",\"lastFlushedBufferPresent\":"
            << JsonBool(result_.lastFlushedBufferPresent)
            << ",\"fromWindowResult\":" << result_.flushedFromWindowResult
            << ",\"bufferReleaseResult\":"
            << result_.lastFlushedBufferReleaseResult << "},\n"
            << "  \"import\":{\"propertyResult\":" << result_.nativePropertyResult
            << ",\"createResult\":" << result_.externalBufferCreateResult
            << ",\"allocateResult\":" << result_.importAllocateResult
            << ",\"bindResult\":" << result_.importBindResult
            << ",\"vkMapResult\":" << result_.importedVkMapResult
            << ",\"persistentVkMapResult\":" << result_.persistentVkMapResult
            << ",\"persistentVkFlushResult\":" << result_.persistentVkFlushResult
            << ",\"persistentVkInvalidateResult\":"
            << result_.persistentVkInvalidateResult
            << ",\"explicitCacheMaintenanceRequired\":"
            << JsonBool(result_.explicitCacheMaintenanceRequired)
            << ",\"allocationSize\":" << result_.nativeAllocationSize
            << ",\"nativeMemoryTypeBits\":" << result_.nativeMemoryTypeBits
            << ",\"bufferMemoryTypeBits\":" << result_.bufferMemoryTypeBits
            << ",\"selectedMemoryType\":" << result_.selectedMemoryType
            << ",\"selectedMemoryFlags\":" << result_.selectedMemoryFlags << "},\n"
            << "  \"memoryTypes\":[";
        for (uint32_t i = 0; i < result_.memoryProperties.memoryTypeCount; ++i) {
            if (i) out << ',';
            const auto& type = result_.memoryProperties.memoryTypes[i];
            out << "{\"index\":" << i << ",\"propertyFlags\":"
                << type.propertyFlags << ",\"heapIndex\":" << type.heapIndex << "}";
        }
        out << "],\n"
            << "  \"visibility\":{\"synchronizedCpuToGpuPassed\":"
            << JsonBool(result_.synchronizedCpuToGpuPassed)
            << ",\"synchronizedGpuToCpuPassed\":"
            << JsonBool(result_.synchronizedGpuToCpuPassed)
            << ",\"persistentCpuToGpuPassed\":"
            << JsonBool(result_.persistentCpuToGpuPassed)
            << ",\"persistentGpuToCpuPassed\":"
            << JsonBool(result_.persistentGpuToCpuPassed)
            << ",\"persistentVkCpuToGpuPassed\":"
            << JsonBool(result_.persistentVkCpuToGpuPassed)
            << ",\"persistentVkGpuToCpuPassed\":"
            << JsonBool(result_.persistentVkGpuToCpuPassed)
            << ",\"synchronizedCpuToGpuMismatches\":"
            << result_.synchronizedCpuToGpuMismatches
            << ",\"synchronizedGpuToCpuMismatches\":"
            << result_.synchronizedGpuToCpuMismatches
            << ",\"persistentCpuToGpuMismatches\":"
            << result_.persistentCpuToGpuMismatches
            << ",\"persistentGpuToCpuMismatches\":"
            << result_.persistentGpuToCpuMismatches
            << ",\"persistentVkCpuToGpuMismatches\":"
            << result_.persistentVkCpuToGpuMismatches
            << ",\"persistentVkGpuToCpuMismatches\":"
            << result_.persistentVkGpuToCpuMismatches << "},\n"
            << "  \"decision\":{\"uploadBackingViable\":"
            << JsonBool(fullUpload)
            << ",\"readbackBackingViable\":" << JsonBool(readback)
            << ",\"copyOnlySharedBacking\":" << JsonBool(copyOnly)
            << ",\"synchronizedOnly\":" << JsonBool(synchronized && !anyDirect)
            << ",\"requiresShadowCopy\":" << JsonBool(!fullUpload)
            << ",\"uploadRequiresShadowCopy\":" << JsonBool(!fullUpload)
            << ",\"readbackRequiresShadowCopy\":" << JsonBool(!readback) << "}\n"
            << "}\n";
        const std::string json = out.str();
        std::fwrite(json.data(), 1, json.size(), stdout);
        if (outputPath && *outputPath) {
            std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
            file.write(json.data(), static_cast<std::streamsize>(json.size()));
        }
    }

    void ReleaseProducerBuffer()
    {
        if (producerWindow_ && windowBuffer_) {
            result_.producerAbortResult = OH_NativeWindow_NativeWindowAbortBuffer(
                producerWindow_, windowBuffer_);
            windowBuffer_ = nullptr;
        }
        if (windowFenceFd_ >= 0) {
            close(windowFenceFd_);
            windowFenceFd_ = -1;
        }
        if (flushedWindowBuffer_) {
            result_.lastFlushedBufferReleaseResult =
                OH_NativeWindow_NativeObjectUnreference(flushedWindowBuffer_);
            flushedWindowBuffer_ = nullptr;
        }
        ReleaseEglProducer();
        if (producerWindow_ && ownsProducerWindow_)
            OH_NativeWindow_DestroyNativeWindow(producerWindow_);
        // Otherwise the NativeImage owns the window returned by
        // AcquireNativeWindow.
        producerWindow_ = nullptr;
        result_.consumerFrameSignals = consumerFrameSignals_.load(std::memory_order_relaxed);
        if (consumerSurface_ && consumerListenerSet_) {
            OH_NativeImage_UnsetOnFrameAvailableListener(consumerSurface_);
            consumerListenerSet_ = false;
        }
        if (consumerSurface_) OH_NativeImage_Destroy(&consumerSurface_);
    }

    void DestroyHostBuffer(HostBuffer& buffer)
    {
        if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }

    void ReleaseProbeBacking()
    {
        DestroyHostBuffer(upload_);
        DestroyHostBuffer(readback_);
        if (importedBuffer_) {
            vkDestroyBuffer(device_, importedBuffer_, nullptr);
            importedBuffer_ = VK_NULL_HANDLE;
        }
        if (importedMemory_) {
            vkFreeMemory(device_, importedMemory_, nullptr);
            importedMemory_ = VK_NULL_HANDLE;
        }
        if (nativeBuffer_) {
            OH_NativeBuffer_Unreference(nativeBuffer_);
            nativeBuffer_ = nullptr;
        }
        ReleaseProducerBuffer();
    }

    void Cleanup()
    {
        if (device_) vkDeviceWaitIdle(device_);
        ReleaseProbeBacking();
        ReleaseProducerBuffer();
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (device_) vkDestroyDevice(device_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    Result result_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkBuffer importedBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory importedMemory_ = VK_NULL_HANDLE;
    HostBuffer upload_;
    HostBuffer readback_;
    OH_NativeBuffer* nativeBuffer_ = nullptr;
    OH_NativeImage* consumerSurface_ = nullptr;
    OHNativeWindow* producerWindow_ = nullptr;
    OHNativeWindowBuffer* windowBuffer_ = nullptr;
    OHNativeWindowBuffer* flushedWindowBuffer_ = nullptr;
    int windowFenceFd_ = -1;
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    std::atomic<uint32_t> consumerFrameSignals_{0};
    bool consumerListenerSet_ = false;
    bool ownsProducerWindow_ = false;
    bool externalProducerWindow_ = false;
    PFN_vkGetNativeBufferPropertiesOHOS getNativeBufferProperties_ = nullptr;
    PFN_vkGetMemoryNativeBufferOHOS getMemoryNativeBuffer_ = nullptr;
    PFN_vkAcquireImageOHOS acquireImage_ = nullptr;
    PFN_vkQueueSignalReleaseImageOHOS releaseImage_ = nullptr;
};

} // namespace

int WineHuaRunOhosNativeBufferVulkanProbe(const char* outputPath)
{
    NativeBufferVulkanProbe probe;
    return probe.Run(outputPath);
}

extern "C" int winehua_host_probe_main(int argc, char** argv)
{
    return WineHuaRunOhosNativeBufferVulkanProbe(argc > 1 ? argv[1] : nullptr);
}

extern "C" int winehua_host_nativebuffer_window_probe(
    uint64_t surfaceId, const char* outputPath)
{
    OHNativeWindow* window = nullptr;
    const int result = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (result != 0 || !window) return 4;
    NativeBufferVulkanProbe probe(window, true);
    return probe.Run(outputPath);
}

#ifndef WINEHUA_NATIVEBUFFER_PROBE_LIBRARY
int main(int argc, char** argv)
{
    return winehua_host_probe_main(argc, argv);
}
#endif
