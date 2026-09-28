#include "vulkan_renderer.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>
#include <android/log.h>
#include <android/looper.h>

#include "shaders_spv.h"

#define LOG_TAG "VulkanRenderer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace vkr {

namespace {
    // Keep in sync with the push constant block in the shaders
    struct PushConstants {
        float uvRect[4];
        float uvClamp[4];
        float params[4];
        float params2[4];
    };

    constexpr float kOutputPassthrough = 0.0f;
    constexpr float kOutputPqToSdr = 1.0f;
    constexpr float kSdrWhiteNits = 203.0f;

    constexpr uint32_t kWakeFrame = 1;
    constexpr uint32_t kWakeHdr = 2;
    constexpr uint32_t kWakeQuit = 4;

    constexpr int kLooperIdWake = 1;

    // Imported buffers are kept while the decoder keeps cycling through the same ones
    constexpr size_t kMaxImports = 24;

    // Images we may hold at once: the waiting queue, the ones the GPU is reading, the one on
    // screen, and one being acquired
    constexpr int32_t kMaxReaderImages = static_cast<int32_t>(FramePacer::kMaxQueuedFrames) + 4;

    constexpr uint64_t kFenceTimeoutNs = 1'000'000'000;
    constexpr uint64_t kAcquireTimeoutNs = 100'000'000;

    void raiseThreadPriority(const char* name) {
        // ANDROID_PRIORITY_URGENT_DISPLAY, then ANDROID_PRIORITY_DISPLAY
        const pid_t tid = gettid();
        if (setpriority(PRIO_PROCESS, tid, -8) != 0 && setpriority(PRIO_PROCESS, tid, -4) != 0) {
            ALOGW("Could not raise the %s thread's priority", name);
            return;
        }
        ALOGI("%s thread priority %d", name, getpriority(PRIO_PROCESS, tid));
    }

    int64_t nowNs() {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
        for (const auto& ext : extensions) {
            if (strcmp(ext.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    bool isYcbcrFormat(VkFormat format) {
        return (format >= VK_FORMAT_G8B8G8R8_422_UNORM && format <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
               (format >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM && format <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
    }

    const char* pacingName(PacingMode mode) {
        switch (mode) {
            case PacingMode::MinLatency: return "lowest latency";
            case PacingMode::Balanced: return "balanced";
            case PacingMode::CapFps: return "FPS limit";
            case PacingMode::Smoothness: return "smoothest";
            case PacingMode::HostTimed: return "host frame timing";
        }
        return "unknown";
    }

    const char* const kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };

    // Checks the parts of a physical device we can't run without
    bool deviceIsSuitable(VkApi& vk, VkPhysicalDevice device) {
        VkPhysicalDeviceProperties props;
        vk.vkGetPhysicalDeviceProperties(device, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) {
            ALOGI("%s only supports Vulkan %u.%u", props.deviceName,
                  VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
            return false;
        }

        uint32_t count = 0;
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
        for (const char* name : kRequiredDeviceExtensions) {
            if (!hasExtension(extensions, name)) {
                ALOGI("%s lacks %s", props.deviceName, name);
                return false;
            }
        }

        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
        VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &ycbcr;
        vk.vkGetPhysicalDeviceFeatures2(device, &features);
        if (!ycbcr.samplerYcbcrConversion) {
            ALOGI("%s lacks samplerYcbcrConversion", props.deviceName);
            return false;
        }

        return true;
    }

    VkInstance createVkInstance(VkApi& vk, bool* hasColorspaceExt) {
        uint32_t version = 0;
        if (vk.vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_1) {
            ALOGI("Vulkan instance version too old");
            return VK_NULL_HANDLE;
        }

        uint32_t count = 0;
        vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        for (const char* name : extensions) {
            if (!hasExtension(available, name)) {
                ALOGI("Instance lacks %s", name);
                return VK_NULL_HANDLE;
            }
        }

        // Needed for HDR10 swapchains
        *hasColorspaceExt = hasExtension(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        if (*hasColorspaceExt) {
            extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }

        VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Moonlight";
        app.pEngineName = "Moonlight";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();

        VkInstance instance = VK_NULL_HANDLE;
        VkResult result = vk.vkCreateInstance(&info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            ALOGE("vkCreateInstance failed: %d", result);
            return VK_NULL_HANDLE;
        }
        return instance;
    }
}

bool VulkanRenderer::ConversionKey::operator==(const ConversionKey& other) const {
    return externalFormat == other.externalFormat && format == other.format && ycbcr == other.ycbcr &&
           model == other.model && range == other.range &&
           components.r == other.components.r && components.g == other.components.g &&
           components.b == other.components.b && components.a == other.components.a &&
           xChromaOffset == other.xChromaOffset && yChromaOffset == other.yChromaOffset &&
           filter == other.filter;
}

bool VulkanRenderer::probe() {
    if (!loadNdkApi()) {
        return false;
    }

    VkApi vk;
    if (!vk.loadGlobal()) {
        return false;
    }

    bool hasColorspaceExt = false;
    VkInstance instance = createVkInstance(vk, &hasColorspaceExt);
    if (instance == VK_NULL_HANDLE) {
        return false;
    }

    bool suitable = false;
    if (vk.loadInstance(instance)) {
        uint32_t count = 0;
        vk.vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vk.vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice device : devices) {
            if (deviceIsSuitable(vk, device)) {
                suitable = true;
                break;
            }
        }
    }

    vk.vkDestroyInstance(instance, nullptr);
    return suitable;
}

std::unique_ptr<VulkanRenderer> VulkanRenderer::create(ANativeWindow* output, const RendererConfig& config) {
    const NdkApi* ndk = loadNdkApi();
    if (!ndk) {
        return nullptr;
    }

    std::unique_ptr<VulkanRenderer> renderer(new VulkanRenderer(ndk, config));
    if (!renderer->init(output)) {
        return nullptr;
    }
    return renderer;
}

VulkanRenderer::VulkanRenderer(const NdkApi* ndk, const RendererConfig& config)
    : ndk_(ndk),
      config_(config),
      pacer_(static_cast<PacingMode>(config.framePacing), config.streamFps,
             config.displayRefreshHz > 1.0f ? static_cast<int64_t>(1e9 / config.displayRefreshHz) : 16'666'667) {
}

bool VulkanRenderer::init(ANativeWindow* output) {
    ANativeWindow_acquire(output);
    outputWindow_ = output;

    if (!vk_.loadGlobal() || !createInstance() || !pickDevice() || !createDevice() ||
            !createFrameResources() || !createShaderModules() || !createImageReader()) {
        return false;
    }

    char configuration[256];
    snprintf(configuration, sizeof(configuration),
             "mode=%d,streamFps=%d,width=%d,height=%d,periodNs=%lld,refreshHz=%.3f,tenBit=%d,dither=%d",
             config_.framePacing, config_.streamFps, config_.streamWidth, config_.streamHeight,
             static_cast<long long>(pacer_.vsyncPeriodNs()), config_.displayRefreshHz, config_.tenBit ? 1 : 0,
             config_.ditherMode);
    trace_.start(config_.traceDirectory, configuration);

    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        ALOGE("eventfd failed");
        return false;
    }

    renderThread_ = std::thread(&VulkanRenderer::renderThreadMain, this);
    return true;
}

bool VulkanRenderer::createInstance() {
    instance_ = createVkInstance(vk_, &hasColorspaceExt_);
    if (instance_ == VK_NULL_HANDLE || !vk_.loadInstance(instance_)) {
        return false;
    }

    VkAndroidSurfaceCreateInfoKHR surfaceInfo {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.window = outputWindow_;
    VkResult result = vk_.vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateAndroidSurfaceKHR failed: %d", result);
        return false;
    }
    return true;
}

bool VulkanRenderer::pickDevice() {
    uint32_t count = 0;
    vk_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (VkPhysicalDevice device : devices) {
        if (!deviceIsSuitable(vk_, device)) {
            continue;
        }

        uint32_t familyCount = 0;
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        for (uint32_t i = 0; i < familyCount; i++) {
            VkBool32 presentSupported = VK_FALSE;
            vk_.vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupported);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported) {
                physicalDevice_ = device;
                queueFamily_ = i;

                uint32_t extCount = 0;
                vk_.vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
                std::vector<VkExtensionProperties> extensions(extCount);
                vk_.vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, extensions.data());
                hasHdrMetadataExt_ = hasExtension(extensions, VK_EXT_HDR_METADATA_EXTENSION_NAME);
                hasDisplayTimingExt_ = hasExtension(extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

                VkPhysicalDeviceProperties props;
                vk_.vkGetPhysicalDeviceProperties(device, &props);
                ALOGI("Using %s (Vulkan %u.%u.%u), queue family %u", props.deviceName,
                      VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                      VK_API_VERSION_PATCH(props.apiVersion), i);
                return true;
            }
        }
    }

    ALOGE("No suitable Vulkan device");
    return false;
}

bool VulkanRenderer::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    std::vector<const char*> extensions(std::begin(kRequiredDeviceExtensions), std::end(kRequiredDeviceExtensions));
    if (hasHdrMetadataExt_) {
        extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }
    // When frames actually reach the screen, for pacer traces
    if (hasDisplayTimingExt_) {
        extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycbcr.samplerYcbcrConversion = VK_TRUE;

    VkDeviceCreateInfo info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &ycbcr;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VkDevice device = VK_NULL_HANDLE;
    VkResult result = vk_.vkCreateDevice(physicalDevice_, &info, nullptr, &device);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateDevice failed: %d", result);
        return false;
    }
    if (!vk_.loadDevice(device)) {
        // Teardown can't rely on the device functions, so destroy it through the instance
        auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vk_.vkGetInstanceProcAddr(instance_, "vkDestroyDevice"));
        if (destroyDevice) {
            destroyDevice(device, nullptr);
        }
        return false;
    }
    device_ = device;
    if (!vk_.vkSetHdrMetadataEXT) {
        hasHdrMetadataExt_ = false;
    }
    if (!vk_.vkGetPastPresentationTimingGOOGLE) {
        hasDisplayTimingExt_ = false;
    }

    vk_.vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool VulkanRenderer::createFrameResources() {
    VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vk_.vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;
    if (vk_.vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_) != VK_SUCCESS) {
        return false;
    }

    for (int i = 0; i < kFramesInFlight; i++) {
        VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk_.vkCreateFence(device_, &fenceInfo, nullptr, &fences_[i]) != VK_SUCCESS ||
                vk_.vkCreateSemaphore(device_, &semInfo, nullptr, &imageAcquired_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createShaderModules() {
    VkShaderModuleCreateInfo info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = sizeof(kVideoVertSpv);
    info.pCode = kVideoVertSpv;
    if (vk_.vkCreateShaderModule(device_, &info, nullptr, &vertShader_) != VK_SUCCESS) {
        return false;
    }
    info.codeSize = sizeof(kVideoFragSpv);
    info.pCode = kVideoFragSpv;
    return vk_.vkCreateShaderModule(device_, &info, nullptr, &fragShader_) == VK_SUCCESS;
}

bool VulkanRenderer::createImageReader() {
    media_status_t status = ndk_->AImageReader_newWithUsage(
            config_.streamWidth, config_.streamHeight, AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, kMaxReaderImages, &reader_);
    if (status != AMEDIA_OK) {
        ALOGE("AImageReader_newWithUsage failed: %d", status);
        return false;
    }

    imageListener_.context = this;
    imageListener_.onImageAvailable = &VulkanRenderer::onImageAvailableThunk;
    ndk_->AImageReader_setImageListener(reader_, &imageListener_);

    if (ndk_->AImageReader_getWindow(reader_, &decoderWindow_) != AMEDIA_OK) {
        ALOGE("AImageReader_getWindow failed");
        return false;
    }
    return true;
}

VulkanRenderer::~VulkanRenderer() {
    stop();

    if (reader_) {
        ndk_->AImageReader_setImageListener(reader_, nullptr);
    }

    std::deque<FramePtr> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        pending.swap(pending_);
        trace_.close();
    }
    pending.clear();

    if (device_) {
        vk_.vkDeviceWaitIdle(device_);
    }
    current_.reset();
    for (auto& frame : slotFrames_) {
        frame.reset();
    }

    if (device_) {
        destroyConversion();
        for (int i = 0; i < kFramesInFlight; i++) {
            if (fences_[i]) vk_.vkDestroyFence(device_, fences_[i], nullptr);
            if (imageAcquired_[i]) vk_.vkDestroySemaphore(device_, imageAcquired_[i], nullptr);
        }
        if (commandPool_) vk_.vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (vertShader_) vk_.vkDestroyShaderModule(device_, vertShader_, nullptr);
        if (fragShader_) vk_.vkDestroyShaderModule(device_, fragShader_, nullptr);
        vk_.vkDestroyDevice(device_, nullptr);
    }
    if (instance_ && vk_.vkDestroyInstance) {
        vk_.vkDestroyInstance(instance_, nullptr);
    }

    // Waits for any image callback still running, which sees closing_ and backs out
    if (reader_) {
        ndk_->AImageReader_delete(reader_);
    }
    if (wakeFd_ >= 0) {
        close(wakeFd_);
    }
}

void VulkanRenderer::stop() {
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    if (stopped_) {
        return;
    }
    stopped_ = true;

    if (renderThread_.joinable()) {
        quit_ = true;
        wake(kWakeQuit);
        renderThread_.join();
    }

    if (device_) {
        vk_.vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (renderPass_) {
            if (pipeline_) {
                vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
                pipeline_ = VK_NULL_HANDLE;
            }
            vk_.vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
    }
    if (surface_) {
        vk_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (outputWindow_) {
        ANativeWindow_release(outputWindow_);
        outputWindow_ = nullptr;
    }
}

void VulkanRenderer::setHdrMode(bool enabled, const uint8_t* metadata, size_t metadataLength) {
    HdrMetadata parsed;
    if (metadata && metadataLength >= 24) {
        uint16_t v[12];
        memcpy(v, metadata, sizeof(v));

        // Layout of SS_HDR_METADATA: R, G, B primaries and white point in 0.00002 units,
        // max display luminance in nits, min display luminance in 0.0001 nits, then MaxCLL
        // and MaxFALL in nits
        auto xy = [](uint16_t x, uint16_t y) { return VkXYColorEXT {x * 0.00002f, y * 0.00002f}; };
        parsed.vk.displayPrimaryRed = xy(v[0], v[1]);
        parsed.vk.displayPrimaryGreen = xy(v[2], v[3]);
        parsed.vk.displayPrimaryBlue = xy(v[4], v[5]);
        parsed.vk.whitePoint = xy(v[6], v[7]);
        parsed.vk.maxLuminance = v[8];
        parsed.vk.minLuminance = v[9] * 0.0001f;
        parsed.vk.maxContentLightLevel = v[10];
        parsed.vk.maxFrameAverageLightLevel = v[11];

        if (v[10] != 0) {
            parsed.contentPeakNits = v[10];
        }
        else if (v[8] != 0) {
            parsed.contentPeakNits = v[8];
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        hdrEnabled_ = enabled;
        hdrMetadata_ = parsed;
        hdrChanged_ = true;
    }
    wake(kWakeHdr);
}

std::string VulkanRenderer::statsText() {
    std::lock_guard<std::mutex> lock(mutex_);

    char text[256];
    int len = snprintf(text, sizeof(text), "Vulkan: %s, %s pacing", outputDescription_.c_str(),
                       pacingName(pacer_.mode()));
    if (pacer_.mode() == PacingMode::HostTimed && pacer_.timeline().hasEstimate() && len < (int) sizeof(text)) {
        len += snprintf(text + len, sizeof(text) - len, " (+%.1f ms buffer, %s, %d vsync%s per frame)",
                        (pacer_.timeline().bufferNs() + pacer_.scheduleDelayNs()) / 1e6,
                        pacer_.phaseLocked() ? "vsync locked" : "unlocked",
                        static_cast<int>(pacer_.slotVsyncs()), pacer_.slotVsyncs() == 1 ? "" : "s");
    }
    const uint64_t dropped = pacer_.framesSkipped() + queueOverflowDrops_;
    if (dropped > 0 && len < (int) sizeof(text)) {
        snprintf(text + len, sizeof(text) - len, ", %" PRIu64 " frames skipped", dropped);
    }
    return text;
}

void VulkanRenderer::wake(uint32_t flags) {
    wakeFlags_.fetch_or(flags);
    if (wakeFd_ >= 0) {
        uint64_t one = 1;
        (void) !write(wakeFd_, &one, sizeof(one));
    }
}

// ---------------------------------------------------------------------------------------------
// Decoder side

void VulkanRenderer::onImageAvailableThunk(void* context, AImageReader*) {
    static_cast<VulkanRenderer*>(context)->onImageAvailable();
}

void VulkanRenderer::onImageAvailable() {
    // This runs on the image reader's own thread. Frames are timed from when it acquires them,
    // so it gets the same priority as the render thread.
    static thread_local bool prioritized = false;
    if (!prioritized) {
        prioritized = true;
        raiseThreadPriority("image reader");
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) {
                return;
            }
        }

        // Waits for the decoder to finish writing the image
        AImage* image = nullptr;
        media_status_t status = ndk_->AImageReader_acquireNextImage(reader_, &image);
        if (status != AMEDIA_OK) {
            if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
                ALOGW("All reader images are held");
            }
            return;
        }

        auto frame = std::make_shared<VideoFrame>(ndk_, image);
        frame->timing.arrivalNs = nowNs();
        if (ndk_->AImage_getHardwareBuffer(image, &frame->buffer) != AMEDIA_OK || !frame->buffer) {
            ALOGE("Image has no hardware buffer");
            continue;
        }
        // MediaCodec stamps each image with the frame's presentation time, which is the host's
        // timestamp for the frame
        ndk_->AImage_getTimestamp(image, &frame->timing.hostPtsNs);
        if (ndk_->AImage_getCropRect(image, &frame->crop) != AMEDIA_OK) {
            frame->crop = {0, 0, 0, 0};
        }

        std::deque<FramePtr> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) {
                return;
            }
            pacer_.onFrameArrived(frame->timing);
            trace_.frame(frame->timing, pending_.size() + 1, queueOverflowDrops_);
            pending_.push_back(std::move(frame));
            while (pending_.size() > pacer_.maxQueued()) {
                dropped.push_back(std::move(pending_.front()));
                pending_.pop_front();
                queueOverflowDrops_++;
            }
        }

        if (presentOnArrival_) {
            wake(kWakeFrame);
        }
        // Frames in `dropped` are released here, outside the lock
    }
}

// ---------------------------------------------------------------------------------------------
// Render thread

void VulkanRenderer::renderThreadMain() {
    // A vsync callback that runs late can miss the compositor's deadline, and the frame reaches
    // the screen a vsync late. Android runs its own render threads at display priority.
    raiseThreadPriority("render");

    ALooper* looper = ALooper_prepare(0);
    ALooper_addFd(looper, wakeFd_, kLooperIdWake, ALOOPER_EVENT_INPUT, &VulkanRenderer::onWakeThunk, this);

    choreographer_ = ndk_->AChoreographer_getInstance();
    if (!choreographer_) {
        ALOGE("No Choreographer on the render thread");
        ALooper_removeFd(looper, wakeFd_);
        return;
    }
    if (ndk_->AChoreographer_registerRefreshRateCallback) {
        ndk_->AChoreographer_registerRefreshRateCallback(choreographer_, &VulkanRenderer::onRefreshRateThunk, this);
        refreshRateCallbackRegistered_ = true;
    }
    ndk_->AChoreographer_postFrameCallback64(choreographer_, &VulkanRenderer::onVsyncThunk, this);

    while (!quit_) {
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
    }

    if (refreshRateCallbackRegistered_) {
        ndk_->AChoreographer_unregisterRefreshRateCallback(choreographer_, &VulkanRenderer::onRefreshRateThunk, this);
    }
    ALooper_removeFd(looper, wakeFd_);
}

void VulkanRenderer::onVsyncThunk(int64_t frameTimeNanos, void* data) {
    static_cast<VulkanRenderer*>(data)->onVsync(frameTimeNanos);
}

void VulkanRenderer::onRefreshRateThunk(int64_t vsyncPeriodNanos, void* data) {
    auto* self = static_cast<VulkanRenderer*>(data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->trace_.period(vsyncPeriodNanos);
    self->pacer_.setVsyncPeriod(vsyncPeriodNanos);
}

int VulkanRenderer::onWakeThunk(int fd, int, void* data) {
    uint64_t value;
    (void) !read(fd, &value, sizeof(value));
    static_cast<VulkanRenderer*>(data)->onWake();
    return 1;
}

void VulkanRenderer::onWake() {
    const uint32_t flags = wakeFlags_.exchange(0);
    if (quit_ || (flags & kWakeQuit)) {
        return;
    }

    if (flags & kWakeHdr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hdrChanged_) {
            hdrChanged_ = false;
            hdrActive_ = hdrEnabled_;
            // New swapchain format and color space, and a new YCbCr model for the frames
            swapchainDirty_ = true;
        }
    }

    if (flags & kWakeFrame) {
        // Lowest latency with a mailbox swapchain: show the newest frame right away and let
        // the display take whichever was presented last before each vsync
        FramePtr frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()) {
                frame = std::move(pending_.back());
                pending_.clear();
                const int64_t now = nowNs();
                pacer_.onPresentedImmediately(now);
                trace_.immediate(now);
            }
        }
        if (frame && renderFrame(frame)) {
            presentedFrames_++;
        }
    }
    else if ((flags & kWakeHdr) && current_ && renderFrame(current_)) {
        // Redrawn in the new output format
    }
}

void VulkanRenderer::trackVsyncPeriod(int64_t frameTimeNanos) {
    // Refined even with the refresh rate callback, which only reports changes
    if (lastVsyncNs_ != 0) {
        const int64_t delta = frameTimeNanos - lastVsyncNs_;
        vsyncDeltas_.push_back(delta);
        if (vsyncDeltas_.size() > 60) {
            vsyncDeltas_.pop_front();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t period = pacer_.vsyncPeriodNs();
        if (delta > period * 3 / 4 && delta < period * 5 / 4) {
            const int64_t refined = period + (delta - period) / 16;
            trace_.period(refined);
            pacer_.setVsyncPeriod(refined);
        }
        else if (vsyncDeltas_.size() == 60) {
            // The refresh rate changed if even the shortest recent interval is far off. Longer
            // intervals alone just mean callbacks were late.
            const int64_t shortest = *std::min_element(vsyncDeltas_.begin(), vsyncDeltas_.end());
            if (shortest < period * 3 / 4 || shortest > period * 5 / 4) {
                ALOGI("Vsync period changed to %" PRId64 " ns", shortest);
                trace_.period(shortest);
                pacer_.setVsyncPeriod(shortest);
                vsyncDeltas_.clear();
            }
        }
    }
    lastVsyncNs_ = frameTimeNanos;
}

void VulkanRenderer::onVsync(int64_t frameTimeNanos) {
    if (quit_) {
        return;
    }

    const int64_t callbackLateNs = nowNs() - frameTimeNanos;
    trackVsyncPeriod(frameTimeNanos);

    FramePtr frame;
    uint64_t presentId = 0;
    std::deque<FramePtr> skipped;
    if (!presentOnArrival_) {
        std::lock_guard<std::mutex> lock(mutex_);
        FrameTiming timings[FramePacer::kMaxQueuedFrames];
        const size_t count = std::min(pending_.size(), FramePacer::kMaxQueuedFrames);
        for (size_t i = 0; i < count; i++) {
            timings[i] = pending_[i]->timing;
        }

        const int choice = pacer_.onVsync(frameTimeNanos, timings, count);
        if (choice >= 0 && hasDisplayTimingExt_) {
            presentId = nextPresentId_++;
        }
        currentVsyncNs_ = frameTimeNanos;
        currentPeriodNs_ = pacer_.vsyncPeriodNs();
        trace_.vsync(frameTimeNanos, count, choice, choice >= 0 ? timings[choice].hostPtsNs : 0, pacer_,
                     callbackLateNs, presentId, hasDisplayTimingExt_ ? presentScheduler_.delayVsyncs() : 0);
        if (choice >= 0) {
            for (int i = 0; i < choice; i++) {
                skipped.push_back(std::move(pending_.front()));
                pending_.pop_front();
            }
            frame = std::move(pending_.front());
            pending_.pop_front();
        }
    }

    if (frame) {
        if (renderFrame(frame, presentId)) {
            presentedFrames_++;
        }
    }
    else if (swapchainDirty_ && current_) {
        // Keep the picture up across a swapchain rebuild
        renderFrame(current_);
    }
    collectPresentTimings();

    ndk_->AChoreographer_postFrameCallback64(choreographer_, &VulkanRenderer::onVsyncThunk, this);
}

// ---------------------------------------------------------------------------------------------
// Rendering

bool VulkanRenderer::renderFrame(const FramePtr& frame, uint64_t presentId) {
    if (!ensureSwapchain()) {
        return false;
    }

    ImportedBuffer* imported = importBuffer(frame->buffer);
    if (!imported || !ensurePipeline()) {
        return false;
    }

    const int slot = static_cast<int>(frameCounter_ % kFramesInFlight);
    VkResult result = vk_.vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kFenceTimeoutNs);
    if (result != VK_SUCCESS) {
        ALOGE("Timed out waiting for the GPU: %d", result);
        return false;
    }
    slotFrames_[slot].reset();

    uint32_t imageIndex = 0;
    result = vk_.vkAcquireNextImageKHR(device_, swapchain_, kAcquireTimeoutNs, imageAcquired_[slot],
                                       VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        if (!ensureSwapchain() || !ensurePipeline()) {
            return false;
        }
        result = vk_.vkAcquireNextImageKHR(device_, swapchain_, kAcquireTimeoutNs, imageAcquired_[slot],
                                           VK_NULL_HANDLE, &imageIndex);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        if (result != VK_TIMEOUT && result != VK_NOT_READY) {
            ALOGE("vkAcquireNextImageKHR failed: %d", result);
            swapchainDirty_ = true;
        }
        return false;
    }

    // Where the picture is within the buffer, which the decoder may pad
    PushConstants pc {};
    const float bufferWidth = static_cast<float>(imported->width);
    const float bufferHeight = static_cast<float>(imported->height);
    AImageCropRect crop = frame->crop;
    if (crop.right <= crop.left || crop.bottom <= crop.top) {
        crop = {0, 0, static_cast<int32_t>(imported->width), static_cast<int32_t>(imported->height)};
    }
    pc.uvRect[0] = crop.left / bufferWidth;
    pc.uvRect[1] = crop.top / bufferHeight;
    pc.uvRect[2] = (crop.right - crop.left) / bufferWidth;
    pc.uvRect[3] = (crop.bottom - crop.top) / bufferHeight;
    // Stay half a texel inside the crop so filtering never pulls in padding
    pc.uvClamp[0] = (crop.left + 0.5f) / bufferWidth;
    pc.uvClamp[1] = (crop.top + 0.5f) / bufferHeight;
    pc.uvClamp[2] = (crop.right - 0.5f) / bufferWidth;
    pc.uvClamp[3] = (crop.bottom - 0.5f) / bufferHeight;

    // Low dithers at the output's own precision. High dithers at 8-bit precision even on a
    // 10-bit output, which also hides banding from an 8-bit panel or compositor further on.
    float ditherAmplitude = 0.0f;
    if (config_.ditherMode == 1) {
        ditherAmplitude = 1.0f / static_cast<float>((1 << outputBits_) - 1);
    }
    else if (config_.ditherMode == 2) {
        ditherAmplitude = 1.0f / 255.0f;
    }
    float contentPeakNits;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        contentPeakNits = hdrMetadata_.contentPeakNits;
    }
    pc.params[0] = ditherAmplitude;
    pc.params[1] = static_cast<float>(frameCounter_ % 64);
    pc.params[2] = (hdrActive_ && !outputPq_) ? kOutputPqToSdr : kOutputPassthrough;
    pc.params[3] = contentPeakNits;
    pc.params2[0] = kSdrWhiteNits;

    VkCommandBuffer cmd = commandBuffers_[slot];
    vk_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(cmd, &beginInfo);

    // Take the decoder's buffer from the foreign (non-Vulkan) queue family
    VkImageMemoryBarrier acquire {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    acquire.dstQueueFamilyIndex = queueFamily_;
    acquire.image = imported->image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &acquire);

    VkRenderPassBeginInfo rpBegin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffers_[imageIndex];
    rpBegin.renderArea = {{0, 0}, extent_};
    vk_.vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, extent_};
    vk_.vkCmdSetViewport(cmd, 0, 1, &viewport);
    vk_.vkCmdSetScissor(cmd, 0, 1, &scissor);
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &imported->descriptorSet, 0, nullptr);
    vk_.vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    vk_.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk_.vkCmdEndRenderPass(cmd);

    // Hand the buffer back to the decoder
    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = queueFamily_;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &release);

    vk_.vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAcquired_[slot];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone_[imageIndex];

    vk_.vkResetFences(device_, 1, &fences_[slot]);
    result = vk_.vkQueueSubmit(queue_, 1, &submit, fences_[slot]);
    if (result != VK_SUCCESS) {
        ALOGE("vkQueueSubmit failed: %d", result);
        return false;
    }

    VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;

    // Ask for the vsync the image should reach the screen at (see PresentScheduler), and tag the
    // present so we find out when it did
    VkPresentTimeGOOGLE presentTime {static_cast<uint32_t>(presentId), 0};
    VkPresentTimesInfoGOOGLE presentTimes {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    if (presentId != 0 && hasDisplayTimingExt_ && currentPeriodNs_ > 0) {
        presentTime.desiredPresentTime =
                static_cast<uint64_t>(presentScheduler_.desiredPresentNs(currentVsyncNs_, currentPeriodNs_));
        presentTimes.swapchainCount = 1;
        presentTimes.pTimes = &presentTime;
        present.pNext = &presentTimes;

        pendingPresents_[presentId] = {currentVsyncNs_, presentScheduler_.delayVsyncs()};
        if (pendingPresents_.size() > 256) {
            // Reports lost across a swapchain rebuild never arrive
            for (auto it = pendingPresents_.begin(); it != pendingPresents_.end();) {
                it = it->first + 128 < presentId ? pendingPresents_.erase(it) : std::next(it);
            }
        }
    }
    result = vk_.vkQueuePresentKHR(queue_, &present);

    slotFrames_[slot] = frame;
    current_ = frame;
    imported->lastUsed = frameCounter_;
    frameCounter_++;

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
    }
    else if (result == VK_SUBOPTIMAL_KHR) {
        // Android reports suboptimal whenever we don't pre-rotate for the display, which we
        // leave to the compositor. Only a size change needs a new swapchain.
        VkSurfaceCapabilitiesKHR caps;
        if (vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps) == VK_SUCCESS &&
                caps.currentExtent.width != UINT32_MAX &&
                (caps.currentExtent.width != extent_.width || caps.currentExtent.height != extent_.height)) {
            swapchainDirty_ = true;
        }
    }
    else if (result != VK_SUCCESS) {
        ALOGE("vkQueuePresentKHR failed: %d", result);
        swapchainDirty_ = true;
    }

    evictImports();
    return true;
}

void VulkanRenderer::collectPresentTimings() {
    if (!hasDisplayTimingExt_ || !swapchain_) {
        return;
    }
    uint32_t count = 0;
    if (vk_.vkGetPastPresentationTimingGOOGLE(device_, swapchain_, &count, nullptr) != VK_SUCCESS || count == 0) {
        return;
    }
    std::vector<VkPastPresentationTimingGOOGLE> timings(count);
    if (vk_.vkGetPastPresentationTimingGOOGLE(device_, swapchain_, &count, timings.data()) < 0) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkPastPresentationTimingGOOGLE& t = timings[i];
        auto pending = pendingPresents_.find(t.presentID);
        if (pending != pendingPresents_.end()) {
            presentScheduler_.onPresented(pending->second.vsyncNs, pending->second.delayVsyncs, currentPeriodNs_,
                                          static_cast<int64_t>(t.actualPresentTime),
                                          static_cast<int64_t>(t.earliestPresentTime),
                                          static_cast<int64_t>(t.presentMargin));
            pendingPresents_.erase(pending);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; i++) {
        const VkPastPresentationTimingGOOGLE& t = timings[i];
        if (t.presentID != 0) {
            trace_.presented(t.presentID, static_cast<int64_t>(t.actualPresentTime),
                             static_cast<int64_t>(t.earliestPresentTime), static_cast<int64_t>(t.presentMargin));
        }
    }
}

bool VulkanRenderer::tracing() {
    std::lock_guard<std::mutex> lock(mutex_);
    return trace_.recording();
}

void VulkanRenderer::noteReceived(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    trace_.received(hostPtsNs, receiveNs, enqueueNs);
}

bool VulkanRenderer::ensureSwapchain() {
    if (!swapchainDirty_ && swapchain_) {
        return true;
    }
    if (!surface_) {
        return false;
    }
    if (!createSwapchain()) {
        return false;
    }
    swapchainDirty_ = false;
    return true;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps) != VK_SUCCESS) {
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = static_cast<uint32_t>(ANativeWindow_getWidth(outputWindow_));
        extent.height = static_cast<uint32_t>(ANativeWindow_getHeight(outputWindow_));
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    uint32_t formatCount = 0;
    vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
    if (formats.empty()) {
        return false;
    }

    auto findFormat = [&](VkFormat format, VkColorSpaceKHR colorSpace, VkSurfaceFormatKHR* out) {
        for (const auto& f : formats) {
            if (f.format == format && f.colorSpace == colorSpace) {
                *out = f;
                return true;
            }
        }
        return false;
    };

    VkSurfaceFormatKHR chosen {};
    bool found = false;
    bool pq = false;
    if (hdrActive_ && hasColorspaceExt_) {
        // The stream is already PQ-encoded BT.2020, so it passes straight through
        found = pq = findFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, &chosen);
        if (!found) {
            ALOGW("Surface has no HDR10 format; tone mapping HDR to SDR");
        }
    }
    if (!found && (config_.tenBit || hdrActive_)) {
        found = findFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, &chosen);
    }
    if (!found) {
        found = findFormat(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, &chosen);
    }
    if (!found) {
        chosen = formats[0];
    }

    uint32_t modeCount = 0;
    vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (pacer_.mode() == PacingMode::MinLatency &&
            std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    uint32_t imageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    else if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
        compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
                caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    }

    // The compositor rotates for the display, as it does for the decoder's own output
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (!(caps.supportedTransforms & transform)) {
        transform = caps.currentTransform;
    }

    VkSwapchainCreateInfoKHR info {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform;
    info.compositeAlpha = compositeAlpha;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;

    // Nothing may still use the old swapchain's images when we destroy them
    vk_.vkDeviceWaitIdle(device_);

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = vk_.vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain);
    destroySwapchain();
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateSwapchainKHR failed: %d", result);
        return false;
    }
    swapchain_ = swapchain;
    surfaceFormat_ = chosen;
    presentMode_ = presentMode;
    extent_ = extent;
    outputPq_ = pq;
    outputBits_ = chosen.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 10 : 8;
    presentOnArrival_ = presentMode == VK_PRESENT_MODE_MAILBOX_KHR;

    if (renderPass_ && renderPassFormat_ != chosen.format) {
        if (pipeline_) {
            vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        vk_.vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (!renderPass_) {
        VkAttachmentDescription color {};
        color.format = chosen.format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // Every pixel is drawn
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;

        // Wait for the presentation engine to be done with the image before writing it
        VkSubpassDependency dependency {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &color;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;
        if (vk_.vkCreateRenderPass(device_, &rpInfo, nullptr, &renderPass_) != VK_SUCCESS) {
            ALOGE("vkCreateRenderPass failed");
            return false;
        }
        renderPassFormat_ = chosen.format;
    }

    uint32_t count = 0;
    vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    std::vector<VkImage> images(count);
    vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &count, images.data());

    for (VkImage image : images) {
        VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = chosen.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vk_.vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            return false;
        }
        swapchainViews_.push_back(view);

        VkFramebufferCreateInfo fbInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &view;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vk_.vkCreateFramebuffer(device_, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            return false;
        }
        framebuffers_.push_back(framebuffer);

        VkSemaphoreCreateInfo semInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vk_.vkCreateSemaphore(device_, &semInfo, nullptr, &semaphore) != VK_SUCCESS) {
            return false;
        }
        renderDone_.push_back(semaphore);
    }

    if (outputPq_) {
        applyHdrMetadata();
    }

    // A change in HDR state also changes the YCbCr model, so drop the old conversion
    {
        std::lock_guard<std::mutex> lock(mutex_);
        char description[128];
        snprintf(description, sizeof(description), "%ux%u %s%s", extent.width, extent.height,
                 outputPq_ ? "HDR10" : (outputBits_ == 10 ? "10-bit SDR" : "8-bit SDR"),
                 (hdrActive_ && !outputPq_) ? " (tone mapped)" : "");
        outputDescription_ = description;
    }

    ALOGI("Swapchain %ux%u, format %d, color space %d, %u images, %s", extent.width, extent.height,
          chosen.format, chosen.colorSpace, count, presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "FIFO");
    return true;
}

void VulkanRenderer::destroySwapchain() {
    for (VkFramebuffer fb : framebuffers_) {
        vk_.vkDestroyFramebuffer(device_, fb, nullptr);
    }
    for (VkImageView view : swapchainViews_) {
        vk_.vkDestroyImageView(device_, view, nullptr);
    }
    for (VkSemaphore sem : renderDone_) {
        vk_.vkDestroySemaphore(device_, sem, nullptr);
    }
    framebuffers_.clear();
    swapchainViews_.clear();
    renderDone_.clear();
    if (swapchain_) {
        vk_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::applyHdrMetadata() {
    if (!hasHdrMetadataExt_ || !swapchain_) {
        return;
    }
    VkHdrMetadataEXT metadata;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metadata = hdrMetadata_.vk;
    }
    if (metadata.maxLuminance > 0) {
        vk_.vkSetHdrMetadataEXT(device_, 1, &swapchain_, &metadata);
    }
}

bool VulkanRenderer::ensureConversion(const ConversionKey& key) {
    if (haveConversion_ && key == conversionKey_) {
        return true;
    }

    vk_.vkDeviceWaitIdle(device_);
    destroyConversion();

    if (key.ycbcr) {
        VkExternalFormatANDROID externalFormat {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
        externalFormat.externalFormat = key.externalFormat;

        VkSamplerYcbcrConversionCreateInfo info {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        info.pNext = key.externalFormat ? &externalFormat : nullptr;
        info.format = key.format;
        info.ycbcrModel = key.model;
        info.ycbcrRange = key.range;
        info.components = key.components;
        info.xChromaOffset = key.xChromaOffset;
        info.yChromaOffset = key.yChromaOffset;
        info.chromaFilter = key.filter;
        info.forceExplicitReconstruction = VK_FALSE;
        VkResult result = vk_.vkCreateSamplerYcbcrConversion(device_, &info, nullptr, &conversion_);
        if (result != VK_SUCCESS) {
            ALOGE("vkCreateSamplerYcbcrConversion failed: %d", result);
            return false;
        }
    }

    VkSamplerYcbcrConversionInfo conversionInfo {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    conversionInfo.conversion = conversion_;

    VkSamplerCreateInfo samplerInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.pNext = conversion_ ? &conversionInfo : nullptr;
    samplerInfo.magFilter = key.filter;
    samplerInfo.minFilter = key.filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vk_.vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        ALOGE("vkCreateSampler failed");
        return false;
    }

    // A YCbCr conversion has to be baked into the layout as an immutable sampler
    VkDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler_;
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vk_.vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        return false;
    }

    // Multi-planar images can take several descriptors each, so leave room
    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kMaxImports + 8) * 3};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kMaxImports + 8;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vk_.vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange range {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &range;
    if (vk_.vkCreatePipelineLayout(device_, &plInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        return false;
    }

    conversionKey_ = key;
    haveConversion_ = true;
    ALOGI("YCbCr conversion: external format %" PRIu64 ", format %d, model %d, range %d, filter %d",
          key.externalFormat, key.format, key.model, key.range, key.filter);
    return true;
}

void VulkanRenderer::destroyConversion() {
    for (auto& entry : imports_) {
        destroyImport(entry.first, entry.second);
    }
    imports_.clear();

    if (pipeline_) vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vk_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vk_.vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (setLayout_) vk_.vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (sampler_) vk_.vkDestroySampler(device_, sampler_, nullptr);
    if (conversion_) vk_.vkDestroySamplerYcbcrConversion(device_, conversion_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    conversion_ = VK_NULL_HANDLE;
    haveConversion_ = false;
}

bool VulkanRenderer::ensurePipeline() {
    if (pipeline_) {
        return true;
    }
    if (!renderPass_ || !pipelineLayout_) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    info.renderPass = renderPass_;
    info.subpass = 0;

    VkResult result = vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateGraphicsPipelines failed: %d", result);
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

VulkanRenderer::ImportedBuffer* VulkanRenderer::importBuffer(AHardwareBuffer* buffer) {
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &formatProps;

    // The color model can change with HDR, so the key is checked even for known buffers
    auto it = imports_.find(buffer);
    if (it == imports_.end() || !haveConversion_) {
        VkResult result = vk_.vkGetAndroidHardwareBufferPropertiesANDROID(device_, buffer, &props);
        if (result != VK_SUCCESS) {
            ALOGE("vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", result);
            return nullptr;
        }

        ConversionKey key;
        key.format = formatProps.format;
        key.externalFormat = formatProps.format == VK_FORMAT_UNDEFINED ? formatProps.externalFormat : 0;
        key.ycbcr = key.externalFormat != 0 || isYcbcrFormat(formatProps.format);
        key.components = formatProps.samplerYcbcrConversionComponents;
        key.xChromaOffset = formatProps.suggestedXChromaOffset;
        key.yChromaOffset = formatProps.suggestedYChromaOffset;
        key.filter = (!key.ycbcr || (formatProps.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT))
                     ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        // We know how the host encoded the stream, so use that instead of what the buffer
        // suggests. Sunshine sends HDR as BT.2020.
        int colorspace = hdrActive_ ? 2 : config_.colorspace;
        key.model = colorspace == 0 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 :
                    colorspace == 2 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020 :
                                      VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        key.range = config_.fullRange ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        if (!key.ycbcr) {
            key.components = {};
        }

        if (!ensureConversion(key)) {
            return nullptr;
        }
        // A new conversion drops every import, including this buffer's
        it = imports_.find(buffer);
    }
    else {
        const int colorspace = hdrActive_ ? 2 : config_.colorspace;
        const VkSamplerYcbcrModelConversion model =
                colorspace == 0 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 :
                colorspace == 2 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020 :
                                  VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        if (conversionKey_.ycbcr && conversionKey_.model != model) {
            ConversionKey key = conversionKey_;
            key.model = model;
            if (!ensureConversion(key)) {
                return nullptr;
            }
            it = imports_.find(buffer);
        }
    }

    if (it != imports_.end()) {
        it->second.lastUsed = frameCounter_;
        return &it->second;
    }

    if (props.allocationSize == 0) {
        VkResult result = vk_.vkGetAndroidHardwareBufferPropertiesANDROID(device_, buffer, &props);
        if (result != VK_SUCCESS) {
            ALOGE("vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", result);
            return nullptr;
        }
    }

    AHardwareBuffer_Desc desc {};
    ndk_->AHardwareBuffer_describe(buffer, &desc);

    ImportedBuffer imported;
    imported.width = desc.width;
    imported.height = desc.height;
    imported.lastUsed = frameCounter_;

    VkExternalFormatANDROID externalFormat {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    externalFormat.externalFormat = conversionKey_.externalFormat;
    VkExternalMemoryImageCreateInfo externalMemory {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalMemory.pNext = &externalFormat;
    externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalMemory;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = conversionKey_.format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vk_.vkCreateImage(device_, &imageInfo, nullptr, &imported.image);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateImage for hardware buffer failed: %d", result);
        return nullptr;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &importInfo;
    dedicated.image = imported.image;
    VkMemoryAllocateInfo allocInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &dedicated;
    allocInfo.allocationSize = props.allocationSize;
    allocInfo.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(props.memoryTypeBits));
    result = vk_.vkAllocateMemory(device_, &allocInfo, nullptr, &imported.memory);
    if (result != VK_SUCCESS) {
        ALOGE("Importing hardware buffer failed: %d", result);
        vk_.vkDestroyImage(device_, imported.image, nullptr);
        return nullptr;
    }
    vk_.vkBindImageMemory(device_, imported.image, imported.memory, 0);

    VkSamplerYcbcrConversionInfo conversionInfo {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    conversionInfo.conversion = conversion_;
    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.pNext = conversion_ ? &conversionInfo : nullptr;
    viewInfo.image = imported.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = conversionKey_.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vk_.vkCreateImageView(device_, &viewInfo, nullptr, &imported.view);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateImageView for hardware buffer failed: %d", result);
        vk_.vkFreeMemory(device_, imported.memory, nullptr);
        vk_.vkDestroyImage(device_, imported.image, nullptr);
        return nullptr;
    }

    VkDescriptorSetAllocateInfo setInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    result = vk_.vkAllocateDescriptorSets(device_, &setInfo, &imported.descriptorSet);
    if (result != VK_SUCCESS) {
        ALOGE("vkAllocateDescriptorSets failed: %d", result);
        vk_.vkDestroyImageView(device_, imported.view, nullptr);
        vk_.vkFreeMemory(device_, imported.memory, nullptr);
        vk_.vkDestroyImage(device_, imported.image, nullptr);
        return nullptr;
    }

    VkDescriptorImageInfo imageDescriptor {};
    imageDescriptor.imageView = imported.view;
    imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = imported.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageDescriptor;
    vk_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Holding a reference keeps the buffer, and so this cache key, from being reused
    ndk_->AHardwareBuffer_acquire(buffer);
    return &imports_.emplace(buffer, imported).first->second;
}

void VulkanRenderer::destroyImport(AHardwareBuffer* buffer, ImportedBuffer& imported) {
    if (imported.descriptorSet) vk_.vkFreeDescriptorSets(device_, descriptorPool_, 1, &imported.descriptorSet);
    if (imported.view) vk_.vkDestroyImageView(device_, imported.view, nullptr);
    if (imported.image) vk_.vkDestroyImage(device_, imported.image, nullptr);
    if (imported.memory) vk_.vkFreeMemory(device_, imported.memory, nullptr);
    ndk_->AHardwareBuffer_release(buffer);
}

void VulkanRenderer::evictImports() {
    // Buffers the decoder stopped using. The last few frames may still be on the GPU.
    while (imports_.size() > kMaxImports) {
        auto oldest = imports_.end();
        for (auto it = imports_.begin(); it != imports_.end(); ++it) {
            if (it->second.lastUsed + kFramesInFlight + 1 < frameCounter_ &&
                    (oldest == imports_.end() || it->second.lastUsed < oldest->second.lastUsed)) {
                oldest = it;
            }
        }
        if (oldest == imports_.end()) {
            return;
        }
        destroyImport(oldest->first, oldest->second);
        imports_.erase(oldest);
    }
}

}  // namespace vkr
