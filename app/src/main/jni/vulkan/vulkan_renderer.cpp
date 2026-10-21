#include "vulkan_renderer.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <android/log.h>
#include <android/looper.h>

#include "shaders_spv.h"

#include <sys/system_properties.h>

#include <Limelight.h>

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
        float ycbcr[4];
    };

    constexpr float kOutputPassthrough = 0.0f;
    constexpr float kOutputPqToSdr = 1.0f;
    constexpr float kSdrWhiteNits = 203.0f;

    // Quarter turns clockwise for a pre-rotation, or -1 for a transform we can't render
    int quarterTurns(VkSurfaceTransformFlagBitsKHR transform) {
        switch (transform) {
            case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return 0;
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return 1;
            case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return 2;
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return 3;
            default: return -1;
        }
    }

    // Rendering the display's rotation ourselves keeps the layer untransformed. Display
    // hardware often can't rotate RGB layers, and then the compositor falls back to drawing
    // the layer on the GPU, which for HDR means its own tone mapping into an 8-bit target.
    VkSurfaceTransformFlagBitsKHR choosePreTransform(const VkSurfaceCapabilitiesKHR& caps) {
        if (quarterTurns(caps.currentTransform) >= 0 && (caps.supportedTransforms & caps.currentTransform)) {
            return caps.currentTransform;
        }
        if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
            return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        }
        return caps.currentTransform;
    }

    // The window's size in the display's natural orientation, as a pre-rotated swapchain needs
    VkExtent2D preRotatedExtent(VkExtent2D extent, VkSurfaceTransformFlagBitsKHR transform) {
        if (quarterTurns(transform) % 2 == 1) {
            std::swap(extent.width, extent.height);
        }
        return extent;
    }

    constexpr uint32_t kWakeFrame = 1;
    constexpr uint32_t kWakeHdr = 2;
    constexpr uint32_t kWakeQuit = 4;

    constexpr int kLooperIdWake = 1;
    constexpr int kLooperIdAlignedTimer = 2;

    // Window for the overlay's count of skipped frames
    constexpr int64_t kRecentSkipsNs = 10'000'000'000;

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

    const char* const kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };

    // The newest Vulkan a PyroWave instance and device use, where they have it
    constexpr uint32_t kPyrowaveApiVersion = VK_API_VERSION_1_3;

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

}

bool VulkanRenderer::ConversionKey::operator==(const ConversionKey& other) const {
    return externalFormat == other.externalFormat && format == other.format && ycbcr == other.ycbcr &&
           model == other.model && range == other.range &&
           components.r == other.components.r && components.g == other.components.g &&
           components.b == other.components.b && components.a == other.components.a &&
           xChromaOffset == other.xChromaOffset && yChromaOffset == other.yChromaOffset &&
           filter == other.filter;
}

VkInstance VulkanRenderer::createVkInstance(VkApi& vk, bool pyrowave, bool* hasColorspaceExt, InstanceSetup& setup,
                                            bool frontBuffer) {
    uint32_t version = 0;
    if (vk.vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_1) {
        ALOGI("Vulkan instance version too old");
        return VK_NULL_HANDLE;
    }
    // PyroWave's Vulkan backend runs on 1.1 with extensions, and uses what's core up to 1.3
    // where the device has it
    const uint32_t apiVersion = pyrowave ? std::min(version, kPyrowaveApiVersion) : VK_API_VERSION_1_1;

    uint32_t count = 0;
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    setup.extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    for (const char* name : setup.extensions) {
        if (!hasExtension(available, name)) {
            ALOGI("Instance lacks %s", name);
            return VK_NULL_HANDLE;
        }
    }

    // Needed for HDR10 swapchains
    *hasColorspaceExt = hasExtension(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (*hasColorspaceExt) {
        setup.extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    // VK_KHR_shared_presentable_image, for a front buffer, depends on it
    if (frontBuffer && hasExtension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        setup.extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }

    setup.app.pApplicationName = "Moonlight";
    setup.app.pEngineName = "Moonlight";
    setup.app.apiVersion = apiVersion;

    setup.info.pApplicationInfo = &setup.app;
    setup.info.enabledExtensionCount = static_cast<uint32_t>(setup.extensions.size());
    setup.info.ppEnabledExtensionNames = setup.extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vk.vkCreateInstance(&setup.info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateInstance failed: %d", result);
        return VK_NULL_HANDLE;
    }
    return instance;
}

bool VulkanRenderer::prepareDevice(VkApi& vk, VkPhysicalDevice device, uint32_t queueFamily, bool pyrowave,
                                   bool hdrMetadata, bool displayTiming, DeviceSetup& setup) {
    uint32_t familyCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    setup.queue.queueFamilyIndex = queueFamily;
    setup.queue.queueCount = 1;
    // To compare where PyroWave decodes: adb shell setprop debug.moonlight.pyrowave_queue low (its
    // own queue below rendering, the default), equal (its own queue, same priority) or shared
    // (rendering's queue). On a Pixel 10 Pro at 60 fps, a tenth of decodes take 10-16 ms instead
    // of 6, in streaks of a few frames, whichever queue they're on; shared also showed a few
    // times more frames late to the screen, a render waiting behind a decode.
    char queueMode[PROP_VALUE_MAX] = {};
    __system_property_get("debug.moonlight.pyrowave_queue", queueMode);
    if (pyrowave && strcmp(queueMode, "shared") != 0 && queueFamily < familyCount &&
            families[queueFamily].queueCount >= 2) {
        setup.queue.queueCount = 2;
        setup.priorities[0] = strcmp(queueMode, "equal") == 0 ? 1.0f : 0.0f;
    }
    if (pyrowave && queueMode[0] != 0) {
        ALOGI("PyroWave queue: %s was asked for", queueMode);
    }
    setup.queue.pQueuePriorities = setup.priorities;

    setup.extensions.assign(std::begin(kRequiredDeviceExtensions), std::end(kRequiredDeviceExtensions));
    if (hdrMetadata) {
        setup.extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }
    // When frames actually reach the screen, for pacer traces
    if (displayTiming) {
        setup.extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    }

    if (pyrowave) {
        // PyroWave sees what the device has enabled, so give it all the device supports
        VkPhysicalDeviceProperties props;
        vk.vkGetPhysicalDeviceProperties(device, &props);
        if (!setup.pyrowave.query(vk, device, std::min(props.apiVersion, kPyrowaveApiVersion))) {
            ALOGI("%s can't run PyroWave", props.deviceName);
            return false;
        }
        setup.extensions.insert(setup.extensions.end(), setup.pyrowave.extensions.begin(),
                                setup.pyrowave.extensions.end());
        setup.info.pNext = &setup.pyrowave.features2;
    }
    else {
        setup.ycbcr.samplerYcbcrConversion = VK_TRUE;
        setup.info.pNext = &setup.ycbcr;
    }
    setup.info.queueCreateInfoCount = 1;
    setup.info.pQueueCreateInfos = &setup.queue;
    setup.info.enabledExtensionCount = static_cast<uint32_t>(setup.extensions.size());
    setup.info.ppEnabledExtensionNames = setup.extensions.data();
    return true;
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
    InstanceSetup setup;
    VkInstance instance = createVkInstance(vk, false, &hasColorspaceExt, setup);
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

bool VulkanRenderer::probePyrowave() {
    VkApi vk;
    if (!loadNdkApi() || !PyrowaveDecoder::loadLibrary() || !vk.loadGlobal()) {
        return false;
    }

    bool hasColorspaceExt = false;
    InstanceSetup instanceSetup;
    VkInstance instance = createVkInstance(vk, true, &hasColorspaceExt, instanceSetup);
    if (instance == VK_NULL_HANDLE) {
        return false;
    }

    // Set PyroWave up on a device made as the renderer would make it, and have it make a
    // decoder there. That's the only sure way to know it can run.
    bool supported = false;
    if (vk.loadInstance(instance)) {
        uint32_t count = 0;
        vk.vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vk.vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice physicalDevice : devices) {
            if (!deviceIsSuitable(vk, physicalDevice)) {
                continue;
            }

            uint32_t familyCount = 0;
            vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
            uint32_t family = 0;
            while (family < familyCount && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                family++;
            }

            DeviceSetup deviceSetup;
            if (family == familyCount ||
                    !prepareDevice(vk, physicalDevice, family, true, false, false, deviceSetup)) {
                continue;
            }
            VkDevice device = VK_NULL_HANDLE;
            if (vk.vkCreateDevice(physicalDevice, &deviceSetup.info, nullptr, &device) != VK_SUCCESS) {
                continue;
            }
            if (vk.loadDevice(device)) {
                std::mutex queueMutex;
                PyrowaveDecoder::DeviceInfo info;
                info.instance = instance;
                info.physicalDevice = physicalDevice;
                info.device = device;
                info.instanceInfo = &instanceSetup.info;
                info.deviceInfo = &deviceSetup.info;
                vk.vkGetDeviceQueue(device, family, 0, &info.queue);
                info.queueFamily = family;
                info.queueMutex = &queueMutex;
                info.timelineSemaphores = deviceSetup.pyrowave.timelineSemaphores;
                supported = PyrowaveDecoder::create(vk, info, 64, 64, false) != nullptr;
                vk.vkDeviceWaitIdle(device);
                vk.vkDestroyDevice(device, nullptr);
            }
            else {
                auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
                        vk.vkGetInstanceProcAddr(instance, "vkDestroyDevice"));
                if (destroyDevice) {
                    destroyDevice(device, nullptr);
                }
            }
            if (supported) {
                break;
            }
        }
    }

    vk.vkDestroyInstance(instance, nullptr);
    return supported;
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
             config.displayRefreshHz > 1.0f ? static_cast<int64_t>(1e9 / config.displayRefreshHz) : 16'666'667,
             static_cast<JitterBuffer>(config.jitterBuffer)) {
    // The presets, from 144 fps lowest latency runs on a link the stream nearly filled. The
    // margin covers frames that arrive late taking longer than most to decode: the frames
    // around them are late too, so they wait behind them. Without one, four in ten frames cut
    // were still late; 1 ms left one in eight, 2 ms with a 30% minimum one in a hundred, and
    // each extra millisecond cut about a tenth more of the frames.
    //
    // The same trade applies to frames that lost packets: shown blurrier with less of them, or
    // dropped, which on a link that lost about a frame in 130 (a Pixel 10 Pro at 60 fps) was
    // about half of its stutters. Off and Sharper keep PyroWave's default of nine tenths.
    switch (config.pyrowaveLateFrames) {
    case 0:  // Off
        partialEnabled_ = false;
        lostFrameMinPercent_ = 90;
        break;
    case 1:  // Sharper
        partialMinPercent_ = 50;
        partialMarginNs_ = 1'000'000;
        lostFrameMinPercent_ = 90;
        break;
    case 3:  // Smoother
        partialMinPercent_ = 25;
        partialMarginNs_ = 3'000'000;
        lostFrameMinPercent_ = 50;
        break;
    case 4:  // Smoothest: any frame with its coarsest levels is shown, however blurred, and
             // frames are cut early enough to cover most of a slow decode
        partialMinPercent_ = 10;
        partialMarginNs_ = 5'000'000;
        lostFrameMinPercent_ = 0;
        break;
    default:  // Balanced
        partialMinPercent_ = 30;
        partialMarginNs_ = 2'000'000;
        lostFrameMinPercent_ = 75;
        break;
    }

    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.moonlight.partial", value) > 0) {
        partialEnabled_ = atoi(value) != 0;
    }
    if (__system_property_get("debug.moonlight.partial_min_pct", value) > 0) {
        partialMinPercent_ = std::clamp(atoi(value), 0, 100);
    }
    if (__system_property_get("debug.moonlight.partial_margin_us", value) > 0) {
        partialMarginNs_ = static_cast<int64_t>(std::clamp(atoi(value), -20000, 20000)) * 1000;
    }
    if (__system_property_get("debug.moonlight.partial_lost_pct", value) > 0) {
        lostFrameMinPercent_ = std::clamp(atoi(value), 0, 100);
    }
    // The setting asks for a front buffer aligned to the scan; to force a mode either way,
    // adb shell setprop debug.moonlight.front_buffer 0 (off), 1 (drawn on arrival, tearing) or 2.
    // The setting only takes it for host frame timing, which paces the front buffer's draws, and
    // lowest latency, which shows frames on arrival anyway. The other modes pick a frame at each
    // vsync, which a front buffer would bypass by drawing every frame as it arrives.
    const bool frontBufferMode = pacer_.mode() == PacingMode::HostTimed || pacer_.mode() == PacingMode::MinLatency;
    frontBufferRequested_ = config.frontBuffer && frontBufferMode;
    frontBufferAligned_ = frontBufferRequested_;
    if (__system_property_get("debug.moonlight.front_buffer", value) > 0) {
        frontBufferRequested_ = atoi(value) != 0;
        frontBufferAligned_ = atoi(value) == 2;
    }
    // PyroWave decodes on a device of its own with a front buffer, which needs the draws to
    // interrupt them; adb shell setprop debug.moonlight.pyrowave_device own (always) or shared
    // (never) to choose
    pyrowaveOwnDeviceRequested_ = frontBufferRequested_;
    if (__system_property_get("debug.moonlight.pyrowave_device", value) > 0) {
        if (strcmp(value, "own") == 0) {
            pyrowaveOwnDeviceRequested_ = true;
        }
        else if (strcmp(value, "shared") == 0) {
            pyrowaveOwnDeviceRequested_ = false;
        }
    }
    if (frontBufferAligned_ && __system_property_get("debug.moonlight.front_buffer_lead_us", value) > 0) {
        fixedAlignedLeadNs_ = static_cast<int64_t>(std::clamp(atoi(value), 0, 20000)) * 1000;
    }
    if (frontBufferRequested_) {
        ALOGI("Front buffer asked for%s", frontBufferAligned_ ? ", aligned to the scan" : "");
    }
    ALOGI("PyroWave frames that lost packets shown with at least %d%% of their blocks", lostFrameMinPercent_);
    ALOGI("Late PyroWave frames cut short: %s", partialEnabled_ ? "on" : "off");
    if (partialEnabled_) {
        ALOGI("Cut with %d%% of their packets in, %lld us margin", partialMinPercent_,
              static_cast<long long>(partialMarginNs_ / 1000));
    }
}

bool VulkanRenderer::init(ANativeWindow* output) {
    ANativeWindow_acquire(output);
    outputWindow_ = output;

    if (!vk_.loadGlobal() || !createInstance() || !pickDevice() || !createDevice() ||
            !createFrameResources() || !createShaderModules()) {
        return false;
    }

    // Performance hints for the render threads (PerfHint), set up before any of them start. On
    // unless turned off with adb shell setprop debug.moonlight.perf_hint 0; the target each
    // render's CPU time is held to can be changed with
    // adb shell setprop debug.moonlight.perf_hint_target_us 2000.
    int64_t perfHintTargetNs = PerfHint::kDefaultTargetNs;
    {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.moonlight.perf_hint", value) > 0 && atoi(value) == 0) {
            perfHintTargetNs = 0;
        }
        else if (__system_property_get("debug.moonlight.perf_hint_target_us", value) > 0 && atoi(value) > 0) {
            perfHintTargetNs = static_cast<int64_t>(std::clamp(atoi(value), 100, 100000)) * 1000;
        }
    }
    perfHint_.start(ndk_, perfHintTargetNs);

    // Keep-alive presents while the host sends nothing (keepAlive()), every 16 ms unless
    // changed with adb shell setprop debug.moonlight.keep_alive_ms <ms>, or 0 for none
    keepAliveNs_ = 16'000'000;
    {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.moonlight.keep_alive_ms", value) > 0) {
            keepAliveNs_ = static_cast<int64_t>(std::clamp(atoi(value), 0, 1000)) * 1'000'000;
        }
    }

    if (config_.pyrowave ? !createPyrowaveDecoder() : !createImageReader()) {
        return false;
    }

    // Work for the GPU between PyroWave decodes, so it keeps its clock up (GpuKeepBusy), with the
    // PyroWave GPU boost setting. To switch it on or off whatever the setting says:
    // adb shell setprop debug.moonlight.gpu_keep_busy 1 (or 0). Each piece of it takes about
    // adb shell setprop debug.moonlight.gpu_keep_busy_chunk_us 500, with
    // adb shell setprop debug.moonlight.gpu_keep_busy_groups 4 workgroups.
    if (config_.pyrowave) {
        char value[PROP_VALUE_MAX] = {};
        bool keepBusy = config_.pyrowaveGpuBoost;
        if (__system_property_get("debug.moonlight.gpu_keep_busy", value) > 0) {
            keepBusy = atoi(value) != 0;
            ALOGI("GPU keep-busy %s was asked for", keepBusy ? "on" : "off");
        }
        if (keepBusy) {
            int64_t chunkNs = 500'000;
            if (__system_property_get("debug.moonlight.gpu_keep_busy_chunk_us", value) > 0 && atoi(value) > 0) {
                chunkNs = static_cast<int64_t>(std::clamp(atoi(value), 100, 5000)) * 1000;
            }
            uint32_t groups = 4;
            if (__system_property_get("debug.moonlight.gpu_keep_busy_groups", value) > 0 && atoi(value) > 0) {
                groups = static_cast<uint32_t>(std::clamp(atoi(value), 1, 1024));
            }
            keepBusyGroups_ = keepBusy_.start(vk_, instance_, physicalDevice_, queueFamily_, globalPriorityExt_, chunkNs,
                                           groups) ? groups : 0;
        }
    }

    // To try a present delay or guard rather than the ones learned (PresentScheduler):
    // adb shell setprop debug.moonlight.present_delay 1 (vsyncs), and
    // adb shell setprop debug.moonlight.present_guard_us 6000. Empty or 0 delay learns as usual.
    {
        char value[PROP_VALUE_MAX] = {};
        int fixedDelay = 0;
        int64_t fixedGuardNs = -1;
        if (__system_property_get("debug.moonlight.present_delay", value) > 0) {
            fixedDelay = atoi(value);
        }
        if (__system_property_get("debug.moonlight.present_guard_us", value) > 0) {
            fixedGuardNs = static_cast<int64_t>(std::clamp(atoi(value), 0, 20000)) * 1000;
        }
        presentScheduler_.fix(fixedDelay, fixedGuardNs);
        if (fixedDelay > 0 || fixedGuardNs >= 0) {
            ALOGI("Present delay %s, guard %s", fixedDelay > 0 ? std::to_string(presentScheduler_.delayVsyncs()).c_str() : "learned",
                  fixedGuardNs >= 0 ? (std::to_string(fixedGuardNs / 1000) + " us").c_str() : "learned");
        }
    }

    // An aligned front buffer draws frames presented ahead at their scan, which needs no
    // display timing
    presentAhead_ = (hasDisplayTimingExt_ || (frontBufferAligned_ && hasSharedPresentExt_)) &&
            pacer_.mode() == PacingMode::HostTimed;
    instant_ = pacer_.mode() == PacingMode::HostTimed && pacer_.timeline().jitterBuffer() == JitterBuffer::Instant;
    pacer_.setPresentAhead(presentAhead_);
    pacer_.setPresentGuardNs(presentScheduler_.guardNs());

    char configuration[512];
    snprintf(configuration, sizeof(configuration),
             "mode=%d,jitterBuffer=%d,presentAhead=%d,streamFps=%d,width=%d,height=%d,periodNs=%lld,refreshHz=%.3f,tenBit=%d,dither=%d,"
             "perfHintUs=%lld,gpuPriority=%s,gpuKeepBusy=%u,keepAliveMs=%lld,codec=%s,decoder=%s",
             config_.framePacing, config_.jitterBuffer, presentAhead_ ? 1 : 0, config_.streamFps, config_.streamWidth, config_.streamHeight,
             static_cast<long long>(pacer_.vsyncPeriodNs()), config_.displayRefreshHz, config_.tenBit ? 1 : 0,
             config_.ditherMode, static_cast<long long>(perfHintTargetNs / 1000), highPriorityQueue_ ? "high" : "default",
             keepBusyGroups_, static_cast<long long>(keepAliveNs_ / 1'000'000), config_.codec.c_str(), config_.decoderName.c_str());
    trace_.start(config_.traceDirectory, configuration);
    traceRecording_ = trace_.recording();

    if (trace_.recording() && timestampValidBits_ > 0 && timestampPeriodNs_ > 0 && vk_.vkCreateQueryPool &&
            vk_.vkDestroyQueryPool && vk_.vkGetQueryPoolResults && vk_.vkCmdResetQueryPool && vk_.vkCmdWriteTimestamp) {
        VkQueryPoolCreateInfo queryInfo {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kTimestampsPerFrame * kFramesInFlight;
        gpuTiming_ = vk_.vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampPool_) == VK_SUCCESS;
    }

    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        ALOGE("eventfd failed");
        return false;
    }

    renderThread_ = std::thread(&VulkanRenderer::renderThreadMain, this);
    return true;
}

bool VulkanRenderer::createInstance() {
    instance_ = createVkInstance(vk_, config_.pyrowave, &hasColorspaceExt_, instanceSetup_, frontBufferRequested_);
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
                hasExternalMemoryFdExt_ = hasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
                hasSharedPresentExt_ = frontBufferRequested_ &&
                        hasExtension(extensions, VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME) &&
                        std::any_of(instanceSetup_.extensions.begin(), instanceSetup_.extensions.end(), [](const char* name) {
                            return strcmp(name, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
                        });
                globalPriorityExt_ = hasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)
                        ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME
                        : hasExtension(extensions, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)
                        ? VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME : nullptr;

                VkPhysicalDeviceProperties props;
                vk_.vkGetPhysicalDeviceProperties(device, &props);
                timestampValidBits_ = families[i].timestampValidBits;
                timestampPeriodNs_ = props.limits.timestampPeriod;
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
    if (!prepareDevice(vk_, physicalDevice_, queueFamily_, config_.pyrowave, hasHdrMetadataExt_, hasDisplayTimingExt_,
                       deviceSetup_)) {
        return false;
    }
    // Before the global priority extension, which is taken off the end if refused
    if (hasSharedPresentExt_) {
        deviceSetup_.extensions.push_back(VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME);
        deviceSetup_.info.enabledExtensionCount = static_cast<uint32_t>(deviceSetup_.extensions.size());
        deviceSetup_.info.ppEnabledExtensionNames = deviceSetup_.extensions.data();
    }
    else if (frontBufferRequested_) {
        ALOGW("Front buffer asked for, but the device has no shared presentable images");
    }

    // PyroWave on a device of its own: this one only renders, on one queue, and imports the
    // planes' memory
    const bool ownDecodeDevice = config_.pyrowave && pyrowaveOwnDeviceRequested_ && hasExternalMemoryFdExt_;
    if (config_.pyrowave && pyrowaveOwnDeviceRequested_ && !hasExternalMemoryFdExt_) {
        ALOGW("PyroWave on its own device asked for, but the device can't share memory between devices");
    }
    if (ownDecodeDevice) {
        deviceSetup_.queue.queueCount = 1;
        deviceSetup_.priorities[0] = 1.0f;
        deviceSetup_.extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        deviceSetup_.info.enabledExtensionCount = static_cast<uint32_t>(deviceSetup_.extensions.size());
        deviceSetup_.info.ppEnabledExtensionNames = deviceSetup_.extensions.data();
    }

    // Our queues at high global priority, so the GPU runs a render (one small draw, due at a
    // vsync) ahead of other apps' work, and can preempt it. Android only lets some apps have
    // it, and a driver refuses the device when it won't, so it's tried and dropped if refused.
    // Not for PyroWave, whose decodes share the queue family and so the priority: they keep the
    // GPU busy most of each frame, and ahead of the compositor's own GPU work they held up the
    // screen. On an iPlay 70 mini Ultra at 500 Mbps and 144 fps, about one in ten wake-ups from
    // the host's idle desktop left the screen unchanged for 80 ms; at the default priority, none
    // in 36. With PyroWave on a device of its own, the decodes stay at the default priority.
    // To choose: adb shell setprop debug.moonlight.gpu_priority default (never) or high (always)
    char priorityMode[PROP_VALUE_MAX] = {};
    __system_property_get("debug.moonlight.gpu_priority", priorityMode);
    const bool tryHighPriority = globalPriorityExt_ && strcmp(priorityMode, "default") != 0 &&
            (!config_.pyrowave || ownDecodeDevice || strcmp(priorityMode, "high") == 0);
    if (tryHighPriority) {
        deviceSetup_.globalPriority.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
        deviceSetup_.queue.pNext = &deviceSetup_.globalPriority;
        deviceSetup_.extensions.push_back(globalPriorityExt_);
        deviceSetup_.info.enabledExtensionCount = static_cast<uint32_t>(deviceSetup_.extensions.size());
        deviceSetup_.info.ppEnabledExtensionNames = deviceSetup_.extensions.data();
    }

    VkDevice device = VK_NULL_HANDLE;
    VkResult result = vk_.vkCreateDevice(physicalDevice_, &deviceSetup_.info, nullptr, &device);
    highPriorityQueue_ = tryHighPriority && result == VK_SUCCESS;
    if (tryHighPriority && result != VK_SUCCESS) {
        ALOGI("High priority GPU queue refused (%d), using the default", result);
        deviceSetup_.queue.pNext = nullptr;
        deviceSetup_.extensions.pop_back();
        deviceSetup_.info.enabledExtensionCount = static_cast<uint32_t>(deviceSetup_.extensions.size());
        deviceSetup_.info.ppEnabledExtensionNames = deviceSetup_.extensions.data();
        result = vk_.vkCreateDevice(physicalDevice_, &deviceSetup_.info, nullptr, &device);
    }
    else if (highPriorityQueue_) {
        ALOGI("High priority GPU queue (%s)", globalPriorityExt_);
    }
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

    if (deviceSetup_.queue.queueCount >= 2) {
        vk_.vkGetDeviceQueue(device_, queueFamily_, 0, &decodeQueue_);
        vk_.vkGetDeviceQueue(device_, queueFamily_, 1, &queue_);
    }
    else {
        vk_.vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }
    if (ownDecodeDevice) {
        createDecodeDevice();
    }
    return true;
}

void VulkanRenderer::createDecodeDevice() {
    // As PyroWave's share of our device would be, on one queue at the default priority
    if (!prepareDevice(vk_, physicalDevice_, queueFamily_, true, false, false, decodeDeviceSetup_)) {
        return;
    }
    decodeDeviceSetup_.queue.queueCount = 1;
    decodeDeviceSetup_.priorities[0] = 1.0f;
    decodeDeviceSetup_.extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    decodeDeviceSetup_.info.enabledExtensionCount = static_cast<uint32_t>(decodeDeviceSetup_.extensions.size());
    decodeDeviceSetup_.info.ppEnabledExtensionNames = decodeDeviceSetup_.extensions.data();

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = vk_.vkCreateDevice(physicalDevice_, &decodeDeviceSetup_.info, nullptr, &device);
    if (result != VK_SUCCESS) {
        ALOGW("Couldn't make a device for PyroWave to decode on (%d); decoding on the render device's queue", result);
        return;
    }
    decodeVk_ = vk_;
    if (!decodeVk_.loadDevice(device)) {
        auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vk_.vkGetInstanceProcAddr(instance_, "vkDestroyDevice"));
        if (destroyDevice) {
            destroyDevice(device, nullptr);
        }
        return;
    }
    decodeDevice_ = device;
    decodeVk_.vkGetDeviceQueue(decodeDevice_, queueFamily_, 0, &decodeDeviceQueue_);
    ALOGI("PyroWave decodes on a device of its own, at the default GPU priority; rendering at %s",
          highPriorityQueue_ ? "high priority" : "the default priority");
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
    if (vk_.vkCreateShaderModule(device_, &info, nullptr, &fragShader_) != VK_SUCCESS) {
        return false;
    }
    if (config_.pyrowave) {
        info.codeSize = sizeof(kVideoPlanarFragSpv);
        info.pCode = kVideoPlanarFragSpv;
        if (vk_.vkCreateShaderModule(device_, &info, nullptr, &planarFragShader_) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createPyrowaveDecoder() {
    PyrowaveDecoder::DeviceInfo info;
    info.instance = instance_;
    info.physicalDevice = physicalDevice_;
    info.device = device_;
    info.instanceInfo = &instanceSetup_.info;
    info.deviceInfo = &deviceSetup_.info;
    info.queue = decodeQueue_ ? decodeQueue_ : queue_;
    info.queueFamily = queueFamily_;
    info.queueMutex = &queueMutex_;
    info.timestampValidBits = timestampValidBits_;
    info.timestampPeriodNs = timestampPeriodNs_;
    info.timelineSemaphores = deviceSetup_.pyrowave.timelineSemaphores;
    if (decodeDevice_) {
        info.device = decodeDevice_;
        info.deviceInfo = &decodeDeviceSetup_.info;
        info.queue = decodeDeviceQueue_;
        info.queueMutex = &decodeDeviceQueueMutex_;
        info.renderDevice = device_;
        info.renderVk = &vk_;
    }
    else if (decodeQueue_) {
        ALOGI("PyroWave decodes on its own queue, %s rendering",
              deviceSetup_.priorities[0] < deviceSetup_.priorities[1] ? "below" : "level with");
    }
    pyrowave_ = PyrowaveDecoder::create(decodeDevice_ ? decodeVk_ : vk_, info, config_.streamWidth,
                                        config_.streamHeight, config_.tenBit);
    if (!pyrowave_) {
        return false;
    }
    pyrowave_->setLostFrameMinBlocks(lostFrameMinPercent_ / 100.0f);

    const VkDescriptorSetLayout setLayout = pyrowave_->setLayout();
    VkPushConstantRange range {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk_.vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &planarPipelineLayout_) != VK_SUCCESS) {
        return false;
    }

    pyrowaveWaiter_ = std::thread(&VulkanRenderer::pyrowaveWaiterMain, this);
    return true;
}

void VulkanRenderer::pyrowaveWaiterMain() {
    // Frames are timed from here, as they are on the image reader's thread
    raiseThreadPriority("PyroWave wait");
    perfHint_.addThread();

    for (;;) {
        FramePtr frame;
        {
            std::unique_lock<std::mutex> lock(decodingMutex_);
            decodingCv_.wait(lock, [this] { return decodingQuit_ || !decoding_.empty(); });
            if (decodingQuit_) {
                return;
            }
            frame = std::move(decoding_.front());
            decoding_.pop_front();
        }

        const VkResult result = pyrowave_->waitDecoded(frame->readyValue, kFenceTimeoutNs);
        if (result != VK_SUCCESS) {
            ALOGE("PyroWave decode didn't finish: %d", result);
            continue;
        }

        frame->timing.arrivalNs = nowNs();

        // What decoding took, without the waits: the CPU's share of it, then its run on the GPU.
        // A decode can start on the GPU before the one before ends, so its run is counted from
        // then. Untimed on the GPU, it's until the decode was done, waits and all.
        int64_t gpuStartNs = 0;
        int64_t gpuEndNs = 0;
        if (!pyrowave_->gpuTimes(frame->readyValue, &gpuStartNs, &gpuEndNs) || gpuEndNs < gpuStartNs) {
            gpuStartNs = 0;
            gpuEndNs = 0;
        }
        const int64_t cpuNs = frame->decodeQueuedNs - frame->decodeStartNs - frame->decodeTimes.waitedNs;
        if (gpuEndNs != 0) {
            const int64_t runStartNs = std::max(gpuStartNs, lastDecodeGpuEndNs_);
            decodedNs_ += cpuNs + std::max<int64_t>(gpuEndNs - runStartNs, 0);
            lastDecodeGpuEndNs_ = std::max(gpuEndNs, lastDecodeGpuEndNs_);
        }
        else {
            decodedNs_ += frame->timing.arrivalNs - frame->decodeStartNs;
        }
        // Without a trace, this needn't hold up the render thread
        if (traceRecording_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(mutex_);
            trace_.decoded(frame->timing.hostPtsNs, frame->decodeStartNs, frame->decodeQueuedNs,
                           frame->timing.arrivalNs, frame->decodeTimes, gpuStartNs, gpuEndNs);
        }
        enqueueFrame(std::move(frame));
    }
}

void VulkanRenderer::waitIdle() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    vk_.vkDeviceWaitIdle(device_);
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

    if (pyrowaveWaiter_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(decodingMutex_);
            decodingQuit_ = true;
        }
        decodingCv_.notify_one();
        pyrowaveWaiter_.join();
        decoding_.clear();
    }

    if (reader_) {
        ndk_->AImageReader_setImageListener(reader_, nullptr);
    }

    std::deque<FramePtr> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        pending.swap(pending_);
        trace_.close();
        traceRecording_ = false;
    }
    pending.clear();

    if (device_) {
        waitIdle();
    }
    current_.reset();
    for (auto& frame : slotFrames_) {
        frame.reset();
    }

    if (device_) {
        // Every frame has given its planes back
        pyrowave_.reset();
        if (decodeDevice_) {
            decodeVk_.vkDestroyDevice(decodeDevice_, nullptr);
            decodeDevice_ = VK_NULL_HANDLE;
        }
        if (planarPipelineLayout_) vk_.vkDestroyPipelineLayout(device_, planarPipelineLayout_, nullptr);
        if (planarFragShader_) vk_.vkDestroyShaderModule(device_, planarFragShader_, nullptr);
        destroyConversion();
        for (int i = 0; i < kFramesInFlight; i++) {
            if (fences_[i]) vk_.vkDestroyFence(device_, fences_[i], nullptr);
            if (imageAcquired_[i]) vk_.vkDestroySemaphore(device_, imageAcquired_[i], nullptr);
        }
        if (timestampPool_) vk_.vkDestroyQueryPool(device_, timestampPool_, nullptr);
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

    keepBusy_.stop();

    if (renderThread_.joinable()) {
        quit_ = true;
        wake(kWakeQuit);
        renderThread_.join();
    }

    // The image reader may be presenting a frame: wait for it, and keep it from starting another
    std::lock_guard<std::mutex> renderLock(renderMutex_);
    renderStopped_ = true;
    alignedDraws_.clear();
    if (device_) {
        waitIdle();
        destroySwapchain();
        if (renderPass_) {
            destroyPipelines();
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
        ALOGI("HDR metadata: max %u nits, min %.4f nits, MaxCLL %u, MaxFALL %u", v[8], v[9] * 0.0001f, v[10], v[11]);

        // A 10000 nit peak is the top of the PQ range, which hosts send when they don't know
        // the display's real peak. Passed on, it has the compositor squeeze everything above
        // the panel's own peak into a sliver, flattening the host's luminance, so leave it out.
        if (v[8] >= 10000) {
            ALOGI("Ignoring placeholder HDR metadata");
        }
        else {
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
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        hdrEnabled_ = enabled;
        hdrMetadata_ = parsed;
        hdrChanged_ = true;
    }
    wake(kWakeHdr);
}

void VulkanRenderer::setJitterBuffer(JitterBuffer jitterBuffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jitterBuffer == pacer_.timeline().jitterBuffer()) {
        return;
    }
    ALOGI("Jitter buffer changed to %d", static_cast<int>(jitterBuffer));
    pacer_.setJitterBuffer(jitterBuffer);
    instant_ = pacer_.mode() == PacingMode::HostTimed && jitterBuffer == JitterBuffer::Instant;
    trace_.jitterBuffer(jitterBuffer);
}

std::string VulkanRenderer::rendererText() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string text = "Vulkan " + outputDescription_;
    if (pyrowave_) {
        text += pyrowave_->fragmentPath() ? ", fragment path" : ", compute path";
    }
    return text;
}

std::string VulkanRenderer::pacingText() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string headline;
    std::string details;
    char text[96];
    if (instant_) {
        // No buffer: frames go to the next vsync they can make
        snprintf(text, sizeof(text), "instant, %.2f Hz", 1e9 / static_cast<double>(pacer_.vsyncPeriodNs()));
        headline = text;
    }
    else if (pacer_.mode() == PacingMode::HostTimed && pacer_.timeline().hasEstimate()) {
        // The display's measured refresh rate, to compare with the host's frame rate
        snprintf(text, sizeof(text), "%.1f ms buffer, %.2f Hz", (pacer_.timeline().bufferNs() + pacer_.scheduleDelayNs()) / 1e6,
                 1e9 / static_cast<double>(pacer_.vsyncPeriodNs()));
        headline = text;
        if (pacer_.phaseLocked()) {
            snprintf(text, sizeof(text), "locked at %d vsync%s/frame", static_cast<int>(pacer_.slotVsyncs()),
                     pacer_.slotVsyncs() == 1 ? "" : "s");
            details = text;
        }
        else {
            details = "unlocked";
        }
    }
    // Frames skipped recently. A running total since the stream started says little about
    // how it's going now.
    const int64_t now = nowNs();
    const uint64_t total = pacer_.framesSkipped() + queueOverflowDrops_;
    skipSamples_.push_back({now, total});
    while (skipSamples_.size() > 1 && skipSamples_[1].timeNs <= now - kRecentSkipsNs) {
        skipSamples_.pop_front();
    }
    const uint64_t skipped = total - skipSamples_.front().total;
    if (skipped > 0) {
        snprintf(text, sizeof(text), "%" PRIu64 " skipped in last %d s", skipped,
                 static_cast<int>(kRecentSkipsNs / 1'000'000'000));
        details += details.empty() ? text : std::string(", ") + text;
    }
    return headline + "\n" + details;
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
    if (imageReaderTid_.exchange(gettid()) != gettid()) {
        perfHint_.addThread();
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) {
                return;
            }
        }

        // The image comes with a fence for the decoder finishing writing it, which we wait on
        // here. AImageReader_acquireNextImage() waits on it itself, but holding the reader's
        // lock, which AImage_delete() needs too: releasing a frame we'd shown (renderFrame())
        // then waited for the next one to decode. On an iPlay 70 mini Ultra that blocked
        // renders for 1-5 ms, and with a present delay of one vsync, 6-9% of frames reached
        // the screen a vsync late, in bursts of seconds.
        AImage* image = nullptr;
        int fenceFd = -1;
        media_status_t status = ndk_->AImageReader_acquireNextImageAsync
                ? ndk_->AImageReader_acquireNextImageAsync(reader_, &image, &fenceFd)
                : ndk_->AImageReader_acquireNextImage(reader_, &image);
        if (status != AMEDIA_OK) {
            if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
                ALOGW("All reader images are held");
            }
            return;
        }
        if (fenceFd >= 0) {
            pollfd fence {fenceFd, POLLIN, 0};
            int ready;
            do {
                ready = poll(&fence, 1, static_cast<int>(kFenceTimeoutNs / 1'000'000));
            } while (ready < 0 && errno == EINTR);
            close(fenceFd);
            if (ready <= 0) {
                ALOGE("Timed out waiting for the decoder to write a frame");
                ndk_->AImage_delete(image);
                continue;
            }
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

        enqueueFrame(std::move(frame));
    }
}

bool VulkanRenderer::submitPyrowaveFrame(const uint8_t* data, size_t size, const PyrowaveDecoder::Gap* gaps,
                                         size_t gapCount, const std::vector<size_t>* recordStarts,
                                         PyrowaveDecoder::Partial partial, int64_t hostPtsNs, int64_t lastPacketUs) {
    // Frames queue up behind this thread, so it gets the render thread's priority
    static thread_local bool prioritized = false;
    if (!prioritized) {
        prioritized = true;
        raiseThreadPriority("PyroWave decode");
    }
    if (pyrowaveDecodeTid_.exchange(gettid()) != gettid()) {
        perfHint_.addThread();
    }
    const int64_t startNs = nowNs();
    if (!pyrowave_) {
        return false;
    }
    bool deadlineEnabled;
    int64_t deadlineOffsetUs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return false;
        }
        deadlineEnabled = partialDeadlineEnabled_;
        deadlineOffsetUs = partialDeadlineOffsetUs_;
        if (partial != PyrowaveDecoder::Partial::None) {
            trace_.partial(hostPtsNs, static_cast<int>(partial));
        }
    }
    if (deadlineEnabled != partialDeadlineSent_ || (deadlineEnabled && deadlineOffsetUs != partialDeadlineSentUs_)) {
        LiSetPartialFrameDeadline(deadlineEnabled, deadlineOffsetUs, partialMinPercent_);
        partialDeadlineSent_ = deadlineEnabled;
        partialDeadlineSentUs_ = deadlineOffsetUs;
    }

    uint64_t readyValue = 0;
    PyrowaveDecoder::DecodeTimes times;
    PyrowavePlanes* planes = pyrowave_->decode(data, size, gaps, gapCount, recordStarts, partial,
                                               &readyValue, &times);
    if (!planes) {
        return false;
    }

    // The decode is only queued on the GPU. The waiting thread passes the frame on once it's done.
    auto frame = std::make_shared<VideoFrame>(pyrowave_.get(), planes, readyValue);
    frame->timing.hostPtsNs = hostPtsNs;
    frame->decodeStartNs = startNs;
    frame->decodeQueuedNs = nowNs();
    frame->decodeTimes = times;
    frame->lastPacketUs = lastPacketUs;
    {
        std::lock_guard<std::mutex> lock(decodingMutex_);
        if (decodingQuit_) {
            return false;
        }
        decoding_.push_back(std::move(frame));
    }
    decodingCv_.notify_one();
    return true;
}

void VulkanRenderer::enqueueFrame(FramePtr frame) {
    noteDecoded(*frame);

    std::deque<FramePtr> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return;
        }
        pacer_.onFrameArrived(frame->timing);
        if (frame->lastPacketUs != 0) {
            updatePartialDeadline(*frame);
        }
        trace_.frame(frame->timing, pending_.size() + 1, queueOverflowDrops_);
        pending_.push_back(std::move(frame));
        while (pending_.size() > pacer_.maxQueued()) {
            dropped.push_back(std::move(pending_.front()));
            pending_.pop_front();
            queueOverflowDrops_++;
        }
    }

    if (presentAhead_ && !presentOnArrival_) {
        // Presented from this thread rather than waking the render thread for it: on a
        // Pixel 10 Pro, the render thread took 1 ms to wake at the median and over 3 ms one
        // time in ten, time the compositor then didn't have
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        if (!renderStopped_) {
            presentAhead();
        }
    }
    else if (presentOnArrival_) {
        wake(kWakeFrame);
    }
    // Frames in `dropped` are released here, outside the lock
}

// A frame still arriving when it should be ready to show is cut short then, rather than shown a
// vsync late: PyroWave sends the coarsest detail first, so it's missing only its finest. On a
// link the stream nearly fills, the last packets of some frames come late even though their
// first come on time. The deadline is the frame's due time less how long frames take from their
// last packet to being decoded (FramePacer::partialDeadlineOffsetNs()). Under mutex_.
void VulkanRenderer::updatePartialDeadline(const VideoFrame& frame) {
    // moonlight-common-c's clock is CLOCK_MONOTONIC_RAW, which runs a little apart from ours
    const int64_t commonToLocalNs = nowNs() - static_cast<int64_t>(LiGetMicroseconds()) * 1000;
    const int64_t lastPacketNs = frame.lastPacketUs * 1000 + commonToLocalNs;
    if (frame.timing.arrivalNs >= lastPacketNs) {
        pacer_.addReadyCost(frame.timing.arrivalNs - lastPacketNs);
    }

    int64_t offsetNs, readyByNs, readyCostNs;
    partialDeadlineEnabled_ = partialEnabled_ &&
            pacer_.partialDeadlineOffsetNs(frame.timing, &offsetNs, &readyByNs, &readyCostNs);
    if (partialDeadlineEnabled_) {
        // Host timestamps are the same on both sides; the deadline is on moonlight-common-c's clock
        partialDeadlineOffsetUs_ = (offsetNs - partialMarginNs_ - commonToLocalNs) / 1000;
        trace_.partialDeadline(frame.timing.hostPtsNs, readyByNs, readyCostNs, commonToLocalNs,
                               partialDeadlineOffsetUs_);
    }
}

// ---------------------------------------------------------------------------------------------
// Render thread

void VulkanRenderer::renderThreadMain() {
    // A vsync callback that runs late can miss the compositor's deadline, and the frame reaches
    // the screen a vsync late. Android runs its own render threads at display priority.
    raiseThreadPriority("render");
    perfHint_.addThread();

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

    if (frontBufferAligned_) {
        alignedTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (alignedTimerFd_ >= 0) {
            ALooper_addFd(looper, alignedTimerFd_, kLooperIdAlignedTimer, ALOOPER_EVENT_INPUT,
                          &VulkanRenderer::onAlignedTimerThunk, this);
        }
        else {
            ALOGW("timerfd_create failed; the front buffer won't be aligned to the scan");
        }
    }

    while (!quit_) {
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
    }

    if (refreshRateCallbackRegistered_) {
        ndk_->AChoreographer_unregisterRefreshRateCallback(choreographer_, &VulkanRenderer::onRefreshRateThunk, this);
    }
    ALooper_removeFd(looper, wakeFd_);
    if (alignedTimerFd_ >= 0) {
        ALooper_removeFd(looper, alignedTimerFd_);
        close(alignedTimerFd_);
        alignedTimerFd_ = -1;
    }
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
    std::lock_guard<std::mutex> renderLock(renderMutex_);

    if (flags & kWakeHdr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hdrChanged_) {
            hdrChanged_ = false;
            hdrActive_ = hdrEnabled_;
            // New swapchain format and color space, and a new YCbCr model for the frames
            swapchainDirty_ = true;
        }
    }

    if ((flags & kWakeFrame) && presentAhead_ && !presentOnArrival_) {
        presentAhead();
    }
    else if ((flags & kWakeFrame) && frontBuffer_ && frontBufferAligned_) {
        scheduleAlignedDraw();
    }
    else if (flags & kWakeFrame) {
        // Lowest latency with a mailbox swapchain: show the newest frame right away and let
        // the display take whichever was presented last before each vsync
        presentNewest();
    }
    else if ((flags & kWakeHdr) && current_ && renderFrame(current_)) {
        // Redrawn in the new output format
    }
}

void VulkanRenderer::scheduleAlignedDraw() {
    if (alignedTimerSet_) {
        // The draw already set takes the newest frame when it runs
        return;
    }
    if (lastVsyncNs_ == 0 || alignedTimerFd_ < 0) {
        presentNewest();
        return;
    }
    int64_t periodNs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        periodNs = pacer_.vsyncPeriodNs();
    }

    // The first scan after now that there's still time to start drawing for, counted on from
    // the last vsync callback (Choreographer's vsync times, less the app vsync offset the lead
    // allows for, match when the display shows a frame to within 0.2 ms on an iPlay 70 mini Ultra)
    const int64_t leadNs = alignedLeadNs();
    const int64_t now = nowNs();
    int64_t vsyncNs = lastVsyncNs_ + periodNs;
    if (vsyncNs - leadNs <= now) {
        vsyncNs += ((now - (vsyncNs - leadNs)) / periodNs + 1) * periodNs;
    }
    const int64_t startNs = vsyncNs - leadNs;

    itimerspec spec {};
    spec.it_value.tv_sec = static_cast<time_t>(startNs / 1'000'000'000);
    spec.it_value.tv_nsec = static_cast<long>(startNs % 1'000'000'000);
    if (timerfd_settime(alignedTimerFd_, TFD_TIMER_ABSTIME, &spec, nullptr) != 0) {
        presentNewest();
        return;
    }
    alignedTimerSet_ = true;
    alignedVsyncNs_ = vsyncNs;
}

int64_t VulkanRenderer::alignedLeadNs() const {
    // Only the offset's phase within a refresh matters: one negative or over a refresh long
    // still puts the scan the same distance before a frame time
    int64_t offsetNs = vsyncOffsetNs_;
    const int64_t periodNs = offsetPeriodNs_;
    if (periodNs > 0) {
        offsetNs = (offsetNs % periodNs + periodNs) % periodNs;
    }
    if (fixedAlignedLeadNs_ >= 0) {
        return fixedAlignedLeadNs_ + offsetNs;
    }
    if (drawSubmitNs_.size() < 8) {
        return kAlignedDefaultLeadNs + offsetNs;
    }
    std::vector<int64_t> sorted(drawSubmitNs_.begin(), drawSubmitNs_.end());
    const auto p90 = sorted.begin() + (sorted.size() - 1) * 9 / 10;
    std::nth_element(sorted.begin(), p90, sorted.end());
    return *p90 + kAlignedGpuStartNs + offsetNs;
}

int VulkanRenderer::onAlignedTimerThunk(int fd, int, void* data) {
    uint64_t expirations;
    (void) !read(fd, &expirations, sizeof(expirations));
    auto* self = static_cast<VulkanRenderer*>(data);
    if (!self->quit_) {
        std::lock_guard<std::mutex> renderLock(self->renderMutex_);
        self->alignedTimerSet_ = false;
        if (self->frontBufferPaced_) {
            self->drawDueAligned();
        }
        else {
            self->presentNewest(self->alignedVsyncNs_);
        }
    }
    return 1;
}

void VulkanRenderer::presentNewest(int64_t targetVsyncNs) {
    FramePtr frame;
    uint64_t presentId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
            return;
        }
        frame = std::move(pending_.back());
        pending_.clear();
        const int64_t now = nowNs();
        pacer_.onPresentedImmediately(now);

        // Tagged to find out when it reached the screen, for traces and the capture to present
        // statistic. A front buffer is never presented as a new image, so it gets no reports;
        // its capture to present is counted up to the present.
        if (hasDisplayTimingExt_ && !frontBuffer_) {
            presentId = nextPresentId_++;
        }
        trace_.immediate(now, frame->timing.hostPtsNs, presentId, targetVsyncNs);
    }
    if (renderFrame(frame, presentId, 0, false, 0, true)) {
        presentedFrames_++;
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
        else if (!refreshRateCallbackRegistered_ && vsyncDeltas_.size() == 60) {
            // Without the refresh rate callback, the refresh rate changed if most recent
            // intervals are far off. A single short one isn't enough: callbacks bunch up after
            // a late one, and taking that for a new refresh rate threw the pacer off.
            std::vector<int64_t> sorted(vsyncDeltas_.begin(), vsyncDeltas_.end());
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const int64_t median = sorted[sorted.size() / 2];
            if (median < period * 3 / 4 || median > period * 5 / 4) {
                ALOGI("Vsync period changed to %" PRId64 " ns", median);
                trace_.period(median);
                pacer_.setVsyncPeriod(median);
                vsyncDeltas_.clear();
            }
        }
        offsetPeriodNs_ = pacer_.vsyncPeriodNs();
    }
    lastVsyncNs_ = frameTimeNanos;
}

void VulkanRenderer::onVsync(int64_t frameTimeNanos) {
    if (quit_) {
        return;
    }

    const int64_t callbackLateNs = nowNs() - frameTimeNanos;
    std::unique_lock<std::mutex> renderLock(renderMutex_);
    trackVsyncPeriod(frameTimeNanos);
    if (frontBufferPaced_) {
        // A frame is committed to a scan only if it can be drawn in time for it
        const int64_t leadNs = alignedLeadNs();
        std::lock_guard<std::mutex> lock(mutex_);
        pacer_.setPresentGuardNs(leadNs);
    }

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
        if (frontBufferPaced_ && !alignedDraws_.empty()) {
            // Older frames waiting for later scans would be drawn over this one
            alignedDraws_.clear();
            setAlignedTimer();
        }
        if (renderFrame(frame, presentId)) {
            presentedFrames_++;
        }
    }
    else if (swapchainDirty_ && current_) {
        // Keep the picture up across a swapchain rebuild
        renderFrame(current_);
    }
    if (presentAhead_ && !presentOnArrival_) {
        presentAhead();
    }
    if (!frame) {
        keepAlive();
    }
    collectPresentTimings();

    ndk_->AChoreographer_postFrameCallback64(choreographer_, &VulkanRenderer::onVsyncThunk, this);
}

// While the host sends nothing (a game paused, or its desktop with the frame rate allowed to drop
// to zero), presents the frame on screen again every keepAliveNs_. Left idle, the compositor was
// slow to show the next frame: on an iPlay 70 mini Ultra streaming PyroWave at 144 fps, the first
// frames after the host had been quiet for a few hundred milliseconds reached the screen 4-13
// vsyncs late, though rendered 2-3 ms after arriving and presented before their vsync, and the
// compositor reported they couldn't have been sooner. Each costs one render (about 1 ms on the
// GPU), only while idle. Stamped with the time it's presented, like a frame shown on arrival, so
// the compositor drops it if a new frame is due at the same vsync.
void VulkanRenderer::keepAlive() {
    // Without a time to present it at, a FIFO swapchain would show it ahead of the next new frame
    // presented for the same vsync, a vsync late
    // A front buffer is taken again at every vsync anyway
    if (keepAliveNs_ <= 0 || !current_ || swapchainDirty_ || frontBuffer_ ||
            !(hasDisplayTimingExt_ || presentOnArrival_)) {
        return;
    }
    const int64_t now = nowNs();
    if (now - lastNewFrameNs_ < kKeepAliveAfterNs || now - lastPresentNs_ < keepAliveNs_) {
        return;
    }

    uint64_t presentId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.empty()) {
            // A frame waiting for its vsync, which will be presented then
            return;
        }
        if (hasDisplayTimingExt_) {
            presentId = nextPresentId_++;
        }
        trace_.keepAlive(now, presentId);
    }
    renderFrame(current_, presentId, 0, false, 0, true);
}

void VulkanRenderer::presentAhead() {
    // In order, as long as each waiting frame's vsync is known and comes after the last one
    // committed. Anything else waits for its vsync.
    for (;;) {
        FramePtr frame;
        uint64_t presentId = 0;
        int64_t showVsyncNs = 0;
        int64_t commitNs = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty()) {
                return;
            }
            showVsyncNs = pacer_.plannedVsyncNs(pending_.front()->timing);
            if (showVsyncNs == 0) {
                return;
            }
            pacer_.onPresentedAhead(showVsyncNs);
            frame = std::move(pending_.front());
            pending_.pop_front();
            presentId = nextPresentId_++;

            // Replacing the last frame presented ahead at the same vsync: the compositor won't
            // show that one, by design
            if (lastAheadPresentId_ != 0 && showVsyncNs <= lastAheadVsyncNs_ + pacer_.vsyncPeriodNs() / 2) {
                auto previous = pendingPresents_.find(lastAheadPresentId_);
                if (previous != pendingPresents_.end()) {
                    previous->second.replaced = true;
                }
            }
            lastAheadPresentId_ = presentId;
            lastAheadVsyncNs_ = showVsyncNs;
            currentPeriodNs_ = pacer_.vsyncPeriodNs();
            commitNs = nowNs();
            trace_.ahead(commitNs, showVsyncNs, frame->timing.hostPtsNs, presentId, presentScheduler_.delayVsyncs(),
                         pacer_);
        }
        if (frontBufferPaced_) {
            queueAlignedDraw({std::move(frame), presentId, showVsyncNs, commitNs});
        }
        else if (renderFrame(frame, presentId, showVsyncNs, true, commitNs)) {
            presentedFrames_++;
        }
    }
}

void VulkanRenderer::queueAlignedDraw(AlignedDraw draw) {
    // Replacing the frame committed to the same scan, which the pacer counted as skipped
    const int64_t periodNs = currentPeriodNs_;
    while (!alignedDraws_.empty() && alignedDraws_.back().vsyncNs >= draw.vsyncNs - periodNs / 2) {
        alignedDraws_.pop_back();
    }
    alignedDraws_.push_back(std::move(draw));
    setAlignedTimer();
}

void VulkanRenderer::drawDueAligned() {
    // The newest draw whose time has come; any before it (the timer ran late) would only be
    // drawn over
    const int64_t dueNs = nowNs() + kAlignedTimerSlackNs + alignedLeadNs();
    bool found = false;
    AlignedDraw draw;
    while (!alignedDraws_.empty() && alignedDraws_.front().vsyncNs <= dueNs) {
        draw = std::move(alignedDraws_.front());
        alignedDraws_.pop_front();
        found = true;
    }
    if (found && renderFrame(draw.frame, draw.presentId, draw.vsyncNs, true, draw.commitNs)) {
        presentedFrames_++;
    }
    setAlignedTimer();
}

void VulkanRenderer::setAlignedTimer() {
    if (alignedTimerFd_ < 0) {
        return;
    }
    itimerspec spec {};
    if (!alignedDraws_.empty()) {
        // A time already past fires at once
        const int64_t startNs = std::max<int64_t>(alignedDraws_.front().vsyncNs - alignedLeadNs(), 1);
        spec.it_value.tv_sec = static_cast<time_t>(startNs / 1'000'000'000);
        spec.it_value.tv_nsec = static_cast<long>(startNs % 1'000'000'000);
    }
    timerfd_settime(alignedTimerFd_, TFD_TIMER_ABSTIME, &spec, nullptr);
    alignedTimerSet_ = !alignedDraws_.empty();
}

// ---------------------------------------------------------------------------------------------
// Rendering

bool VulkanRenderer::renderFrame(const FramePtr& frame, uint64_t presentId, int64_t showVsyncNs, bool ahead,
                                 int64_t commitNs, bool onArrival) {
    const int64_t startNs = nowNs();
    if (!ensureSwapchain()) {
        return false;
    }

    // A PyroWave frame's planes, or MediaCodec's buffer imported into Vulkan
    const bool planar = frame->planes != nullptr;
    ImportedBuffer* imported = nullptr;
    if (planar) {
        if (!ensurePlanarPipeline()) {
            return false;
        }
    }
    else {
        imported = importBuffer(frame->buffer);
        if (!imported || !ensurePipeline()) {
            return false;
        }
    }

    const int slot = static_cast<int>(frameCounter_ % kFramesInFlight);
    // The fence is nearly always done by now, so ask before waiting on it
    VkResult result = vk_.vkGetFenceStatus ? vk_.vkGetFenceStatus(device_, fences_[slot]) : VK_NOT_READY;
    const bool fenceWaited = result != VK_SUCCESS;
    const int64_t fenceWaitNs = fenceWaited ? nowNs() : 0;
    if (fenceWaited) {
        result = vk_.vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kFenceTimeoutNs);
    }
    if (result != VK_SUCCESS) {
        ALOGE("Timed out waiting for the GPU: %d", result);
        return false;
    }
    const int64_t fencedNs = nowNs();
    // Gives MediaCodec's image back to the reader (see onImageAvailable())
    slotFrames_[slot].reset();
    const int64_t releasedNs = nowNs();

    // The fence covers the last present rendered in this slot, so its timestamps are written
    if (gpuTiming_ && slotPresentIds_[slot] != 0) {
        uint64_t ticks[kTimestampsPerFrame] = {};
        if (vk_.vkGetQueryPoolResults(device_, timestampPool_, kTimestampsPerFrame * slot, kTimestampsPerFrame,
                                      sizeof(ticks), ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t mask = timestampValidBits_ >= 64 ? ~0ULL : (1ULL << timestampValidBits_) - 1;
            const auto toNs = [&](uint64_t t) {
                return static_cast<int64_t>(static_cast<double>(t & mask) * timestampPeriodNs_);
            };
            std::lock_guard<std::mutex> lock(mutex_);
            trace_.gpuTimes(slotPresentIds_[slot], toNs(ticks[0]), toNs(ticks[3]), toNs(ticks[1]), toNs(ticks[2]));
        }
    }
    slotPresentIds_[slot] = 0;

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

    const int64_t acquiredNs = nowNs();

    // Where the picture is within the buffer, which the decoder may pad
    PushConstants pc {};
    const uint32_t width = planar ? pyrowave_->width() : imported->width;
    const uint32_t height = planar ? pyrowave_->height() : imported->height;
    const float bufferWidth = static_cast<float>(width);
    const float bufferHeight = static_cast<float>(height);
    AImageCropRect crop = frame->crop;
    if (crop.right <= crop.left || crop.bottom <= crop.top) {
        crop = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
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
    pc.params2[1] = static_cast<float>(std::max(quarterTurns(preTransform_), 0));
    if (planar) {
        // PyroWave is always full range, in the colorspace we asked for, or BT.2020 for HDR
        const int colorspace = hdrActive_ ? 2 : config_.colorspace;
        pc.ycbcr[0] = colorspace == 0 ? 0.299f : colorspace == 2 ? 0.2627f : 0.2126f;
        pc.ycbcr[1] = colorspace == 0 ? 0.114f : colorspace == 2 ? 0.0593f : 0.0722f;
        // Gray chroma as the host's 8-bit UNORM target stores it; 16-bit is as good as 0.5
        pc.ycbcr[2] = pyrowave_->tenBit() ? 0.5f : 128.0f / 255.0f;
        // This fork's Sunshine sites chroma with the left luma sample of each pair (MPEG-2
        // style); the nonary host sites it at the center of each 2x2 quad
        pc.ycbcr[3] = config_.pyrowaveCenterChroma ? 0.0f : 0.5f / bufferWidth;
    }

    VkCommandBuffer cmd = commandBuffers_[slot];
    vk_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(cmd, &beginInfo);

    const bool timed = gpuTiming_ && presentId != 0;
    if (timed) {
        vk_.vkCmdResetQueryPool(cmd, timestampPool_, kTimestampsPerFrame * slot, kTimestampsPerFrame);
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, kTimestampsPerFrame * slot);
    }

    // Take the decoder's buffer from the foreign (non-Vulkan) queue family. PyroWave's planes
    // are ours and stay in GENERAL, and the wait for their decode makes its writes visible.
    VkImageMemoryBarrier acquire {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    if (!planar) {
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
    }
    // PyroWave decoding on a device of its own: its planes from that device, whose decode into
    // them was done before the frame was passed on
    const bool ownDecodeDevice = planar && pyrowave_->ownDevice();
    VkImageMemoryBarrier planeBarriers[3] {};
    if (ownDecodeDevice) {
        for (int i = 0; i < 3; i++) {
            planeBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            planeBarriers[i].srcAccessMask = 0;
            planeBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            planeBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            planeBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            planeBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            planeBarriers[i].dstQueueFamilyIndex = queueFamily_;
            planeBarriers[i].image = frame->planes->renderImages[i];
            planeBarriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 3, planeBarriers);
    }
    // Without timeline semaphores, nothing below waits for the decode on the GPU: it was waited
    // for on its fence before the frame got here, and this makes its writes visible
    const bool waitDecode = planar && !ownDecodeDevice && pyrowave_->timeline() != VK_NULL_HANDLE;
    if (planar && !ownDecodeDevice && !waitDecode) {
        VkMemoryBarrier decoded {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        decoded.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        decoded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &decoded, 0, nullptr, 0, nullptr);
    }
    if (timed) {
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, kTimestampsPerFrame * slot + 1);
    }

    VkRenderPassBeginInfo rpBegin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffers_[imageIndex];
    rpBegin.renderArea = {{0, 0}, extent_};
    vk_.vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, extent_};
    vk_.vkCmdSetViewport(cmd, 0, 1, &viewport);
    vk_.vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkPipelineLayout layout = planar ? planarPipelineLayout_ : pipelineLayout_;
    const VkDescriptorSet descriptorSet = planar ? frame->planes->descriptorSet : imported->descriptorSet;
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, planar ? planarPipeline_ : pipeline_);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptorSet, 0, nullptr);
    vk_.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    vk_.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk_.vkCmdEndRenderPass(cmd);
    if (timed) {
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, kTimestampsPerFrame * slot + 2);
    }

    // Hand the buffer back to the decoder
    if (!planar) {
        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = queueFamily_;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &release);
    }
    // And back to the decode device. It's only given new planes once this render's fence is done.
    if (ownDecodeDevice) {
        for (int i = 0; i < 3; i++) {
            planeBarriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            planeBarriers[i].dstAccessMask = 0;
            planeBarriers[i].srcQueueFamilyIndex = queueFamily_;
            planeBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 3, planeBarriers);
    }

    if (timed) {
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, kTimestampsPerFrame * slot + 3);
    }
    vk_.vkEndCommandBuffer(cmd);

    // The swapchain image, and for PyroWave the decode into the planes. Timed, everything waits
    // for the image, so the start timestamp shows when it was free; only a draw's vertex work
    // could otherwise go ahead of it, which for one triangle is nothing. A decode on a device of
    // its own was waited for before the frame got here, and its timeline is on that device.
    const VkSemaphore waitSemaphores[2] = {imageAcquired_[slot], waitDecode ? pyrowave_->timeline() : VK_NULL_HANDLE};
    const VkPipelineStageFlags waitStages[2] = {
            timed ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    const uint64_t waitValues[2] = {0, frame->readyValue};
    VkTimelineSemaphoreSubmitInfo timelineInfo {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = 2;
    timelineInfo.pWaitSemaphoreValues = waitValues;

    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = waitDecode ? &timelineInfo : nullptr;
    submit.waitSemaphoreCount = waitDecode ? 2 : 1;
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone_[imageIndex];

    vk_.vkResetFences(device_, 1, &fences_[slot]);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        result = vk_.vkQueueSubmit(queue_, 1, &submit, fences_[slot]);
    }
    if (result != VK_SUCCESS) {
        ALOGE("vkQueueSubmit failed: %d", result);
        return false;
    }
    if (frontBufferAligned_ && frontBuffer_) {
        drawSubmitNs_.push_back(nowNs() - startNs);
        if (drawSubmitNs_.size() > kAlignedLeadSamples) {
            drawSubmitNs_.pop_front();
        }
    }
    if (timed) {
        slotPresentIds_[slot] = presentId;
    }

    VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;

    // Ask for the vsync the image should reach the screen at (see PresentScheduler), and tag the
    // present so we find out when it did. A frame shown on arrival is left out of
    // PresentScheduler. With the mailbox swapchain it asks for no time (0, as soon as possible);
    // otherwise (a keep-alive present), for now, which is as soon as possible too, but lets the
    // compositor drop it for a newer frame: it only drops a waiting frame that has a time of its
    // own. Without one, frames queued behind each other: on an iPlay 70 mini Ultra at 144 fps
    // shown on arrival, 44% reached the screen a vsync later than they could have.
    VkPresentTimeGOOGLE presentTime {static_cast<uint32_t>(presentId), 0};
    VkPresentTimesInfoGOOGLE presentTimes {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    // A front buffer is drawn at its time and never presented as a new image, so it has no
    // time to ask for, and its capture to present is counted up to the present
    if (presentId != 0 && hasDisplayTimingExt_ && !frontBuffer_ && (onArrival || currentPeriodNs_ > 0)) {
        const int64_t vsyncNs = showVsyncNs != 0 ? showVsyncNs : currentVsyncNs_;
        if (!onArrival) {
            presentTime.desiredPresentTime =
                    static_cast<uint64_t>(presentScheduler_.desiredPresentNs(vsyncNs, currentPeriodNs_));
        }
        else if (!presentOnArrival_) {
            presentTime.desiredPresentTime = static_cast<uint64_t>(nowNs());
        }
        presentTimes.swapchainCount = 1;
        presentTimes.pTimes = &presentTime;
        present.pNext = &presentTimes;

        // Slack from the commit, not from now: the pacer holds frames to the guard when it
        // commits them, and on a Pixel 10 Pro rendering sometimes took milliseconds after that
        // (waiting for a swapchain image). Measured from here, the guard learned was a third of
        // what commits needed.
        PendingPresent& pending = pendingPresents_[presentId];
        pending = {vsyncNs, presentScheduler_.delayVsyncs(), ahead,
                   ahead ? vsyncNs - (commitNs != 0 ? commitNs : nowNs()) : 0};
        pending.onArrival = onArrival;
        if (!frame->presentCounted) {
            // Counted when the present is reported, at the time it reached the screen
            pending.originNs = frame->originNs;
            pending.originSynced = frame->originSynced;
            frame->presentCounted = true;
        }
        if (pendingPresents_.size() > 256) {
            // Reports lost across a swapchain rebuild never arrive
            while (!pendingPresents_.empty() && pendingPresents_.begin()->first + 128 < presentId) {
                pendingPresents_.erase(pendingPresents_.begin());
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        result = vk_.vkQueuePresentKHR(queue_, &present);
    }
    // The render's CPU time for the performance hint session, without waiting for the GPU or a
    // swapchain image
    perfHint_.reportWork(nowNs() - startNs - (acquiredNs - releasedNs) - (fenceWaited ? fencedNs - fenceWaitNs : 0));

    if (!frame->presentCounted && frame->originNs != 0 && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
        // Without a report of when it reached the screen, until the present was queued. A frame
        // drawn again (across a swapchain rebuild) is only counted the first time.
        frame->presentCounted = true;
        std::lock_guard<std::mutex> lock(latencyMutex_);
        countLatency(captureLatency_.presented, nowNs() - frame->originNs, frame->originSynced);
    }
    if (presentId != 0) {
        const int64_t queuedNs = nowNs();
        std::lock_guard<std::mutex> lock(mutex_);
        trace_.rendered(presentId, startNs, fencedNs, acquiredNs, queuedNs, imageIndex, fenceWaited, releasedNs);
    }

    slotFrames_[slot] = frame;
    lastPresentNs_ = nowNs();
    if (frame != current_) {
        lastNewFrameNs_ = lastPresentNs_;
    }
    current_ = frame;
    if (imported) {
        imported->lastUsed = frameCounter_;
    }
    frameCounter_++;

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
    }
    else if (result == VK_SUBOPTIMAL_KHR) {
        // Android reports suboptimal when the display's rotation no longer matches our
        // pre-rotation. A new rotation or size needs a new swapchain.
        VkSurfaceCapabilitiesKHR caps;
        if (vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps) == VK_SUCCESS) {
            const VkSurfaceTransformFlagBitsKHR transform = choosePreTransform(caps);
            const VkExtent2D extent = preRotatedExtent(caps.currentExtent, transform);
            if (transform != preTransform_ ||
                    (caps.currentExtent.width != UINT32_MAX &&
                     (extent.width != extent_.width || extent.height != extent_.height))) {
                swapchainDirty_ = true;
            }
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

        // Reports come in present order, so earlier presents still without one never reached
        // the screen: the compositor dropped them
        for (auto it = pendingPresents_.begin(); it != pendingPresents_.end() && it->first < t.presentID;) {
            if (it->second.ahead && !it->second.replaced) {
                presentScheduler_.onAheadOutcome(it->second.slackNs, it->second.vsyncNs, it->second.delayVsyncs,
                                                 false, 0, 0, 0, currentPeriodNs_);
            }
            it = pendingPresents_.erase(it);
        }

        auto pending = pendingPresents_.find(t.presentID);
        if (pending != pendingPresents_.end() && pending->second.ahead && !pending->second.replaced) {
            presentScheduler_.onAheadOutcome(pending->second.slackNs, pending->second.vsyncNs,
                                             pending->second.delayVsyncs, true,
                                             static_cast<int64_t>(t.actualPresentTime),
                                             static_cast<int64_t>(t.earliestPresentTime),
                                             static_cast<int64_t>(t.presentMargin), currentPeriodNs_);
        }

        // With frames presented ahead, the delay is tuned for those. A frame that arrived late,
        // and so was presented at its vsync or with less time than the guard, may miss; that
        // frame is late on screen whatever the delay, and it shouldn't raise the delay for all
        // the others.
        const bool counts = pending != pendingPresents_.end() && !pending->second.onArrival &&
                (!presentAhead_ || (pending->second.ahead && pending->second.slackNs >= presentScheduler_.guardNs()));
        if (counts) {
            presentScheduler_.onPresented(pending->second.vsyncNs, pending->second.delayVsyncs, currentPeriodNs_,
                                          static_cast<int64_t>(t.actualPresentTime),
                                          static_cast<int64_t>(t.earliestPresentTime),
                                          static_cast<int64_t>(t.presentMargin),
                                          pending->second.ahead ? pending->second.slackNs : -1);
        }
        if (pending != pendingPresents_.end()) {
            // A frame replaced at its vsync never reached the screen
            if (pending->second.originNs != 0 && !pending->second.replaced) {
                std::lock_guard<std::mutex> lock(latencyMutex_);
                countLatency(captureLatency_.presented,
                             static_cast<int64_t>(t.actualPresentTime) - pending->second.originNs,
                             pending->second.originSynced);
            }
            pendingPresents_.erase(pending);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    presentScheduler_.setAheadWindowNs(pacer_.presentAheadVsyncs() * pacer_.vsyncPeriodNs());
    if (!frontBufferPaced_) {
        pacer_.setPresentGuardNs(presentScheduler_.guardNs());
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkPastPresentationTimingGOOGLE& t = timings[i];
        if (t.presentID != 0) {
            trace_.presented(t.presentID, static_cast<int64_t>(t.actualPresentTime),
                             static_cast<int64_t>(t.earliestPresentTime), static_cast<int64_t>(t.presentMargin));
        }
    }
}

void VulkanRenderer::noteReceived(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs, int64_t hostProcessingNs) {
    // Called as the frame goes to the decoder, so the time on our clock is when decoding starts
    const int64_t queuedNs = nowNs();
    const int64_t commonToLocalNs = queuedNs - static_cast<int64_t>(LiGetMicroseconds()) * 1000;

    // Where its capture latency counts from. A frame the host gave no processing time for repeats
    // an earlier one, so it has no capture of its own.
    if (hostProcessingNs != 0) {
        FrameOrigin origin {0, false};
        uint64_t captureUs = 0;
        if (LiGetHostTimeOnLocalClock(static_cast<uint64_t>(hostPtsNs / 1000), &captureUs)) {
            origin = {static_cast<int64_t>(captureUs) * 1000 + commonToLocalNs, true};
        }
        else if (receiveNs != 0) {
            // Without clock sync, nothing measures the network transit per frame, so it's left out.
            // The host's part is on its clock and the rest on ours, so they add without the clocks
            // needing to agree.
            origin = {receiveNs + commonToLocalNs - hostProcessingNs, false};
        }
        if (origin.ns != 0) {
            std::lock_guard<std::mutex> lock(latencyMutex_);
            origins_[hostPtsNs] = origin;
            while (origins_.size() > 64) {
                origins_.erase(origins_.begin());
            }
        }
    }

    if (!traceRecording_.load(std::memory_order_relaxed)) {
        return;
    }
    uint32_t rttMs = 0, rttVarianceMs = 0;
    if (!LiGetEstimatedRttInfo(&rttMs, &rttVarianceMs)) {
        rttMs = 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    trace_.received(hostPtsNs, receiveNs, enqueueNs, queuedNs, commonToLocalNs, hostProcessingNs, rttMs);
}

void VulkanRenderer::noteDecoded(VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    auto it = origins_.find(frame.timing.hostPtsNs);
    if (it == origins_.end()) {
        return;
    }
    frame.originNs = it->second.ns;
    frame.originSynced = it->second.synced;

    // Frames are decoded in order, so any still waiting before this one never will be
    origins_.erase(origins_.begin(), std::next(it));
    countLatency(captureLatency_.decoded, frame.timing.arrivalNs - frame.originNs, frame.originSynced);
}

// Under latencyMutex_. Outside a second, the clock sync or the frame's timing must be wrong.
void VulkanRenderer::countLatency(LatencySpan& span, int64_t latencyNs, bool synced) {
    if (latencyNs < 0 || latencyNs >= 1'000'000'000) {
        return;
    }
    span.totalNs += latencyNs;
    span.frames++;
    if (!synced) {
        span.unsynced++;
    }
}

VulkanRenderer::CaptureLatency VulkanRenderer::takeCaptureLatency() {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    CaptureLatency latency = captureLatency_;
    captureLatency_ = {};
    return latency;
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
    const VkSurfaceTransformFlagBitsKHR transform = choosePreTransform(caps);
    extent = preRotatedExtent(extent, transform);

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
    const bool shared = hasSharedPresentExt_ &&
            std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) != modes.end();
    if (shared) {
        // The compositor takes the image again at every vsync, so whatever was last drawn into
        // it is shown without presenting a new buffer
        presentMode = VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
    }
    else if (pacer_.mode() == PacingMode::MinLatency &&
            std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (hasSharedPresentExt_ && !shared) {
        ALOGW("Front buffer asked for, but the surface has no shared present mode");
    }

    // Frames presented ahead wait in the swapchain until their vsync: up to the jitter buffer
    // plus the present delay's worth of them at once. A shared image is the only one.
    uint32_t imageCount = std::max(caps.minImageCount, presentAhead_ ? 6u : 3u);
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    if (shared) {
        imageCount = 1;
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    else if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
        compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
                caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
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
    waitIdle();

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = vk_.vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain);
    destroySwapchain();
    if (result != VK_SUCCESS && shared) {
        // The driver offered the mode but refused it: the normal swapchain from now on
        ALOGW("Front buffer swapchain refused (%d); using the normal swapchain", result);
        hasSharedPresentExt_ = false;
        return createSwapchain();
    }
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateSwapchainKHR failed: %d", result);
        return false;
    }
    swapchain_ = swapchain;
    surfaceFormat_ = chosen;
    presentMode_ = presentMode;
    extent_ = extent;
    preTransform_ = transform;
    outputPq_ = pq;
    outputBits_ = chosen.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 10 : 8;
    frontBuffer_ = shared;
    frontBufferUnavailable_ = frontBufferRequested_ && !shared;
    frontBufferPaced_ = shared && frontBufferAligned_ && presentAhead_;
    presentOnArrival_ = presentMode == VK_PRESENT_MODE_MAILBOX_KHR || (shared && !frontBufferPaced_);
    presentScheduler_.onSwapchainCreated();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trace_.swapchain(nowNs(), chosen.format, chosen.colorSpace, hdrActive_, extent.width, extent.height,
                         presentMode);
    }

    // A shared image stays in its own layout, in which the display reads it
    const VkImageLayout finalLayout = shared ? VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (renderPass_ && (renderPassFormat_ != chosen.format || renderPassFinalLayout_ != finalLayout)) {
        destroyPipelines();
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
        color.finalLayout = finalLayout;

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
        // A shared image is acquired with nothing to wait for, so it also waits for the last
        // frame drawn into it
        dependency.srcAccessMask = shared ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
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
        renderPassFinalLayout_ = finalLayout;
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
        snprintf(description, sizeof(description), "%s%s",
                 outputPq_ ? "HDR10" : (outputBits_ == 10 ? "10-bit SDR" : "8-bit SDR"),
                 (hdrActive_ && !outputPq_) ? " (tone mapped)" : "");
        outputDescription_ = description;
    }

    ALOGI("Swapchain %ux%u, format %d, color space %d, %u images, %s, pre-rotated %d degrees", extent.width,
          extent.height, chosen.format, chosen.colorSpace, count,
          shared ? "front buffer" : presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "FIFO",
          quarterTurns(transform) * 90);
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
        ALOGW("Can't pass HDR metadata to the compositor: %s",
              hasHdrMetadataExt_ ? "no swapchain" : "no VK_EXT_hdr_metadata");
        return;
    }
    VkHdrMetadataEXT metadata;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metadata = hdrMetadata_.vk;
    }
    if (metadata.maxLuminance > 0) {
        vk_.vkSetHdrMetadataEXT(device_, 1, &swapchain_, &metadata);
        ALOGI("Applied HDR metadata: max %.0f nits, MaxCLL %.0f, MaxFALL %.0f", metadata.maxLuminance,
              metadata.maxContentLightLevel, metadata.maxFrameAverageLightLevel);
    }
    else {
        ALOGW("No HDR metadata from the host; the compositor will assume a default peak");
    }
}

bool VulkanRenderer::ensureConversion(const ConversionKey& key) {
    if (haveConversion_ && key == conversionKey_) {
        return true;
    }

    waitIdle();
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
    return createPipeline(pipelineLayout_, fragShader_, &pipeline_);
}

bool VulkanRenderer::ensurePlanarPipeline() {
    if (planarPipeline_) {
        return true;
    }
    return createPipeline(planarPipelineLayout_, planarFragShader_, &planarPipeline_);
}

void VulkanRenderer::destroyPipelines() {
    if (pipeline_) {
        vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (planarPipeline_) {
        vk_.vkDestroyPipeline(device_, planarPipeline_, nullptr);
        planarPipeline_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::createPipeline(VkPipelineLayout layout, VkShaderModule fragShader, VkPipeline* pipeline) {
    if (!renderPass_ || !layout) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
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
    info.layout = layout;
    info.renderPass = renderPass_;
    info.subpass = 0;

    VkResult result = vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, pipeline);
    if (result != VK_SUCCESS) {
        ALOGE("vkCreateGraphicsPipelines failed: %d", result);
        *pipeline = VK_NULL_HANDLE;
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
