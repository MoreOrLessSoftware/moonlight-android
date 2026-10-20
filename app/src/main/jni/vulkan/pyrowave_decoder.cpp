#include "pyrowave_decoder.h"

#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <type_traits>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>

// pyrowave.h names VkQueueGlobalPriority as the Vulkan 1.4 headers do; the NDK's are older
#ifndef VK_API_VERSION_1_4
typedef VkQueueGlobalPriorityKHR VkQueueGlobalPriority;
#endif
#include "../pyrowave/pyrowave.h"

#include "frame_pacer.h"
#include "pyrowave_bitstream.h"

#define LOG_TAG "PyroWave"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace vkr {

namespace {
    // Entry points of libpyrowave-shared.so, resolved at runtime
    struct PyrowaveApi {
        decltype(&::pyrowave_get_api_version) getApiVersion;
        decltype(&::pyrowave_create_device) createDevice;
        decltype(&::pyrowave_device_destroy) deviceDestroy;
        decltype(&::pyrowave_device_report_performance_stats) reportPerformanceStats;
        decltype(&::pyrowave_device_set_command_buffer) deviceSetCommandBuffer;
        decltype(&::pyrowave_decoder_device_prefers_fragment_path) prefersFragmentPath;
        decltype(&::pyrowave_decoder_create) decoderCreate;
        decltype(&::pyrowave_decoder_clear) decoderClear;
        decltype(&::pyrowave_decoder_push_packet) decoderPushPacket;
        decltype(&::pyrowave_decoder_decode_is_ready) decoderDecodeIsReady;
        decltype(&::pyrowave_decoder_decode_is_ready_with_sideband) decoderDecodeIsReadyWithSideband;
        decltype(&::pyrowave_decoder_decode_gpu_buffer) decoderDecodeGpuBuffer;
        decltype(&::pyrowave_decoder_destroy) decoderDestroy;
    };

    PyrowaveApi api {};

    // Frames that can hold planes at once: the pacer's queue, the renders in flight, the one on
    // screen, and the one being decoded
    constexpr size_t kMaxPlanes = FramePacer::kMaxQueuedFrames + 4;

    // How often the decoder's GPU timings are logged
    constexpr int64_t kStatsIntervalNs = 10'000'000'000;

    int64_t nowNs() {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    void logStat(void*, const char* message) {
        ALOGI("%s", message);
    }
}

void PyrowaveDecoder::lockQueue(void* userdata) {
    auto* decoder = static_cast<PyrowaveDecoder*>(userdata);
    const int64_t startNs = nowNs();
    decoder->info_.queueMutex->lock();
    decoder->queueLockedNs_ = nowNs();
    decoder->waitedNs_ += decoder->queueLockedNs_ - startNs;
}

void PyrowaveDecoder::unlockQueue(void* userdata) {
    auto* decoder = static_cast<PyrowaveDecoder*>(userdata);
    decoder->queueHeldNs_ += nowNs() - decoder->queueLockedNs_;
    decoder->queueHolds_++;
    decoder->info_.queueMutex->unlock();
}

bool PyrowaveDeviceFeatures::query(const VkApi& vk, VkPhysicalDevice device, uint32_t apiVersion) {
    uint32_t count = 0;
    vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    const auto has = [&](const char* name) {
        for (const auto& ext : available) {
            if (strcmp(ext.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    // The chain, and the extensions for what isn't core: each struct goes in only where its
    // extension is enabled
    extensions.clear();
    void** next = &features2.pNext;
    const auto chain = [&](auto& features) {
        *next = &features;
        next = &features.pNext;
    };
    const auto chainExtension = [&](auto& features, const char* name) {
        if (has(name)) {
            chain(features);
            extensions.push_back(name);
        }
    };
    if (apiVersion >= VK_API_VERSION_1_2) {
        chain(vk11);
        chain(vk12);
    }
    else {
        // Sampler YCbCr conversion and 16-bit storage are core in 1.1, with structs of their own,
        // but Granite only reads those when their extensions are enabled too
        chain(ycbcr);
        if (has(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
        }
        chain(storage16);
        if (has(VK_KHR_16BIT_STORAGE_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
        }
        chainExtension(storage8, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
        chainExtension(float16Int8, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        chainExtension(timeline, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        chainExtension(hostQueryReset, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
        chainExtension(descriptorIndexing, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        chainExtension(scalarBlockLayout, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        // Granite needs it before 1.2, without a features struct
        if (has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        }
        if (has(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        }
        // Granite only reads the driver ID with it enabled, and PyroWave picks the fragment
        // path by it: without it, a Pixel 4's Adreno 640 got the compute path, at 18 ms a frame
        if (has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
        }
    }
    if (apiVersion >= VK_API_VERSION_1_3) {
        chain(vk13);
    }
    else {
        chainExtension(subgroupSizeControl, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        chainExtension(demote, VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME);
        // Granite emulates it where it's missing
        chainExtension(sync2, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    *next = nullptr;
    vk.vkGetPhysicalDeviceFeatures2(device, &features2);

    // Robustness costs performance and nothing here needs it, and protected memory needs
    // protected queues we don't make
    features2.features.robustBufferAccess = VK_FALSE;
    vk13.robustImageAccess = VK_FALSE;
    vk11.protectedMemory = VK_FALSE;

    // What our side needs: rendering samples through a YCbCr conversion. Decodes are ordered
    // and waited for on a timeline semaphore where there is one, and with fences where not.
    timelineSemaphores = apiVersion >= VK_API_VERSION_1_2 ? vk12.timelineSemaphore : timeline.timelineSemaphore;
    const bool hasYcbcr = apiVersion >= VK_API_VERSION_1_2 ? vk11.samplerYcbcrConversion : ycbcr.samplerYcbcrConversion;
    if (!hasYcbcr) {
        ALOGI("No PyroWave on Vulkan %u.%u without sampler YCbCr conversion", VK_API_VERSION_MAJOR(apiVersion),
              VK_API_VERSION_MINOR(apiVersion));
        return false;
    }
    if (!timelineSemaphores) {
        ALOGI("Vulkan %u.%u device lacks timeline semaphores; PyroWave decodes are tracked with fences",
              VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
    }
    // PyroWave's own needs, which it checks when a decoder is made; logged here to say why
    const bool hasSubgroupSizeControl = apiVersion >= VK_API_VERSION_1_3
            ? vk13.subgroupSizeControl && vk13.computeFullSubgroups
            : subgroupSizeControl.subgroupSizeControl && subgroupSizeControl.computeFullSubgroups;
    if (!hasSubgroupSizeControl) {
        ALOGI("Vulkan %u.%u device lacks subgroup size control, which PyroWave's decoder needs",
              VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
    }
    if (apiVersion < VK_API_VERSION_1_2 && !has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
        ALOGI("Vulkan 1.1 device lacks %s, which PyroWave needs", VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
    return true;
}

bool PyrowaveDecoder::loadLibrary() {
    static std::once_flag once;
    static bool loaded = false;

    std::call_once(once, []() {
        // Kept loaded for the life of the process
        void* lib = dlopen("libpyrowave-shared.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            ALOGI("libpyrowave-shared.so not available, so neither is the codec: %s", dlerror());
            return;
        }

        bool resolved = true;
        auto resolve = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (!fn) {
                ALOGE("Missing entry point %s", name);
                resolved = false;
            }
        };
        resolve(api.getApiVersion, "pyrowave_get_api_version");
        resolve(api.createDevice, "pyrowave_create_device");
        resolve(api.deviceDestroy, "pyrowave_device_destroy");
        resolve(api.reportPerformanceStats, "pyrowave_device_report_performance_stats");
        resolve(api.deviceSetCommandBuffer, "pyrowave_device_set_command_buffer");
        resolve(api.prefersFragmentPath, "pyrowave_decoder_device_prefers_fragment_path");
        resolve(api.decoderCreate, "pyrowave_decoder_create");
        resolve(api.decoderClear, "pyrowave_decoder_clear");
        resolve(api.decoderPushPacket, "pyrowave_decoder_push_packet");
        resolve(api.decoderDecodeIsReady, "pyrowave_decoder_decode_is_ready");
        resolve(api.decoderDecodeIsReadyWithSideband, "pyrowave_decoder_decode_is_ready_with_sideband");
        resolve(api.decoderDecodeGpuBuffer, "pyrowave_decoder_decode_gpu_buffer");
        resolve(api.decoderDestroy, "pyrowave_decoder_destroy");
        if (!resolved) {
            return;
        }

        // From 1.0 the API and ABI are stable within a major version, and a later minor version
        // only adds to them
        uint32_t major = 0, minor = 0, patch = 0;
        api.getApiVersion(&major, &minor, &patch);
        if (major != PYROWAVE_API_VERSION_MAJOR || minor < PYROWAVE_API_VERSION_MINOR) {
            ALOGE("Library version %u.%u.%u doesn't provide the %u.%u API we were built with", major, minor, patch,
                  PYROWAVE_API_VERSION_MAJOR, PYROWAVE_API_VERSION_MINOR);
            return;
        }

        ALOGI("Loaded library version %u.%u.%u", major, minor, patch);
        loaded = true;
    });

    return loaded;
}

std::unique_ptr<PyrowaveDecoder> PyrowaveDecoder::create(const VkApi& vk, const DeviceInfo& info, int width,
                                                         int height, bool tenBit) {
    if (!loadLibrary()) {
        return nullptr;
    }

    std::unique_ptr<PyrowaveDecoder> decoder(new PyrowaveDecoder(vk, info));
    if (!decoder->init(width, height, tenBit)) {
        return nullptr;
    }
    return decoder;
}

PyrowaveDecoder::PyrowaveDecoder(const VkApi& vk, const DeviceInfo& info) : vk_(vk), info_(info) {
}

bool PyrowaveDecoder::init(int width, int height, bool tenBit) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        ALOGE("4:2:0 needs an even width and height, not %dx%d", width, height);
        return false;
    }
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    tenBit_ = tenBit;

    // Shares our device and queue. PyroWave reads the queue while it's being created.
    pyrowave_device_create_queue_info queueInfo {info_.queue, info_.queueFamily, 0};
    pyrowave_device_create_info deviceInfo {};
    deviceInfo.GetInstanceProcAddr = vk_.vkGetInstanceProcAddr;
    deviceInfo.instance = info_.instance;
    deviceInfo.physical_device = info_.physicalDevice;
    deviceInfo.device = info_.device;
    deviceInfo.instance_create_info = info_.instanceInfo;
    deviceInfo.device_create_info = info_.deviceInfo;
    deviceInfo.queue_info = &queueInfo;
    deviceInfo.queue_info_count = 1;
    deviceInfo.queue_lock_callback = &PyrowaveDecoder::lockQueue;
    deviceInfo.queue_unlock_callback = &PyrowaveDecoder::unlockQueue;
    deviceInfo.userdata = this;
    pyrowave_result result = api.createDevice(&deviceInfo, &device_);
    if (result != PYROWAVE_SUCCESS) {
        ALOGE("pyrowave_create_device failed: %d", result);
        device_ = nullptr;
        return false;
    }

    // The planes are sampled, and decoded into either as color attachments (the fragment path,
    // which mobile GPUs with weak compute prefer) or as storage images
    format_ = tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    VkFormatProperties formatProps {};
    vk_.vkGetPhysicalDeviceFormatProperties(info_.physicalDevice, format_, &formatProps);
    const VkFormatFeatureFlags features = formatProps.optimalTilingFeatures;
    const bool canFragment = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    const bool canCompute = (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || (!canFragment && !canCompute)) {
        ALOGE("Format %d can't be decoded into and sampled", format_);
        return false;
    }
    // PyroWave picks the fragment path for Qualcomm and Mali. PowerVR is the same: on a Pixel 10
    // Pro at 2276x1280 10-bit, a frame took 6 ms that way and 15 ms on the compute path. To
    // compare the two: adb shell setprop debug.moonlight.pyrowave_path fragment (or compute)
    // (Core in Vulkan 1.2, and from VK_KHR_driver_properties before)
    VkPhysicalDeviceDriverProperties driverProps {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &driverProps;
    vk_.vkGetPhysicalDeviceProperties2(info_.physicalDevice, &props2);
    bool prefersFragment = api.prefersFragmentPath(device_) ||
                           driverProps.driverID == VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
    char forced[PROP_VALUE_MAX] = {};
    __system_property_get("debug.moonlight.pyrowave_path", forced);
    if (strcmp(forced, "fragment") == 0 || strcmp(forced, "compute") == 0) {
        prefersFragment = strcmp(forced, "fragment") == 0;
        ALOGI("The %s path was asked for", forced);
    }
    fragmentPath_ = prefersFragment ? canFragment : !canCompute;
    usage_ = VK_IMAGE_USAGE_SAMPLED_BIT |
             (fragmentPath_ ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT);

    pyrowave_decoder_create_info decoderInfo {};
    decoderInfo.device = device_;
    decoderInfo.width = width;
    decoderInfo.height = height;
    decoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = fragmentPath_;
    result = api.decoderCreate(&decoderInfo, &decoder_);
    if (result != PYROWAVE_SUCCESS) {
        ALOGE("pyrowave_decoder_create failed: %d", result);
        decoder_ = nullptr;
        return false;
    }

    VkSamplerCreateInfo samplerInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const VkFilter filter = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR
                                                                                            : VK_FILTER_NEAREST;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (renderVk().vkCreateSampler(renderDevice(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }

    const VkSampler samplers[3] = {sampler_, sampler_, sampler_};
    VkDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 3;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = samplers;
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (renderVk().vkCreateDescriptorSetLayout(renderDevice(), &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(kMaxPlanes * 3)};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kMaxPlanes;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (renderVk().vkCreateDescriptorPool(renderDevice(), &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandPoolCreateInfo commandPoolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = info_.queueFamily;
    if (vk_.vkCreateCommandPool(info_.device, &commandPoolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vk_.vkAllocateCommandBuffers(info_.device, &allocInfo, &commandBuffer_) != VK_SUCCESS) {
        return false;
    }
    for (auto& slot : decodeSlots_) {
        if (vk_.vkAllocateCommandBuffers(info_.device, &allocInfo, &slot.commandBuffer) != VK_SUCCESS) {
            return false;
        }
    }
    if (info_.timestampValidBits > 0 && info_.timestampPeriodNs > 0 && vk_.vkCreateQueryPool &&
            vk_.vkDestroyQueryPool && vk_.vkGetQueryPoolResults && vk_.vkCmdResetQueryPool && vk_.vkCmdWriteTimestamp) {
        VkQueryPoolCreateInfo queryInfo {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * kDecodeSlots;
        if (vk_.vkCreateQueryPool(info_.device, &queryInfo, nullptr, &timestampPool_) != VK_SUCCESS) {
            ALOGW("Decodes can't be timed on the GPU");
            timestampPool_ = VK_NULL_HANDLE;
        }
    }
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk_.vkCreateFence(info_.device, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) {
        return false;
    }

    if (info_.timelineSemaphores) {
        if (!vk_.vkWaitSemaphores) {
            ALOGE("vkWaitSemaphores not available");
            return false;
        }
        VkSemaphoreTypeCreateInfo timelineInfo {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;
        VkSemaphoreCreateInfo semaphoreInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &timelineInfo;
        if (vk_.vkCreateSemaphore(info_.device, &semaphoreInfo, nullptr, &timeline_) != VK_SUCCESS) {
            return false;
        }
    }
    else {
        // Unsignaled: one is only waited for once its decode is submitted
        VkFenceCreateInfo slotFenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& slot : decodeSlots_) {
            if (vk_.vkCreateFence(info_.device, &slotFenceInfo, nullptr, &slot.fence) != VK_SUCCESS) {
                return false;
            }
        }
    }

    ALOGI("Decoding %ux%u %s 4:2:0 on the %s path", width_, height_, tenBit_ ? "10-bit" : "8-bit",
          fragmentPath_ ? "fragment" : "compute");
    return true;
}

PyrowaveDecoder::~PyrowaveDecoder() {
    // Waits for the GPU to be done with its own work
    if (decoder_) {
        api.decoderDestroy(decoder_);
    }
    for (auto& planes : planes_) {
        destroyPlanes(*planes);
    }
    planes_.clear();
    if (device_) {
        api.deviceDestroy(device_);
    }

    VkDevice device = info_.device;
    if (fence_) vk_.vkDestroyFence(device, fence_, nullptr);
    for (auto& slot : decodeSlots_) {
        if (slot.fence) vk_.vkDestroyFence(device, slot.fence, nullptr);
    }
    if (timestampPool_) vk_.vkDestroyQueryPool(device, timestampPool_, nullptr);
    if (commandPool_) vk_.vkDestroyCommandPool(device, commandPool_, nullptr);
    if (timeline_) vk_.vkDestroySemaphore(device, timeline_, nullptr);
    if (descriptorPool_) renderVk().vkDestroyDescriptorPool(renderDevice(), descriptorPool_, nullptr);
    if (setLayout_) renderVk().vkDestroyDescriptorSetLayout(renderDevice(), setLayout_, nullptr);
    if (sampler_) renderVk().vkDestroySampler(renderDevice(), sampler_, nullptr);
}

PyrowavePlanes* PyrowaveDecoder::decode(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount,
                                        const std::vector<size_t>* recordStarts, Partial partial,
                                        uint64_t* readyValue, DecodeTimes* times) {
    const int64_t startNs = nowNs();
    waitedNs_ = 0;
    queueHeldNs_ = 0;
    queueHolds_ = 0;
    *times = {};

    // Each frame arrives on its own, so anything left from an earlier one is stale
    api.decoderClear(decoder_);

    if (partial != Partial::None) {
        const bool pushed = recordStarts
                ? pushArrivedRecords(data, size, gaps, gapCount, recordStarts->data(), recordStarts->size(),
                                     [&](size_t offset, size_t length) {
                                         return api.decoderPushPacket(decoder_, data + offset, length) == PYROWAVE_SUCCESS;
                                     })
                : pushPartialFrame(data, size, gaps, gapCount);
        if (!pushed) {
            partialDropped_++;
            return nullptr;
        }
    }
    else {
        // Without padding records, that's the whole frame in one go
        pyrowave_result result = PYROWAVE_SUCCESS;
        const bool pushed = pushRecords(data, size, [&](size_t offset, size_t length) {
            result = api.decoderPushPacket(decoder_, data + offset, length);
            return result == PYROWAVE_SUCCESS;
        });
        if (!pushed && result == PYROWAVE_SUCCESS) {
            result = PYROWAVE_ERROR_INVALID_ARGUMENT;
        }
        if (result != PYROWAVE_SUCCESS) {
            if (!loggedFailure_) {
                ALOGE("Invalid frame (%d)", result);
                loggedFailure_ = true;
            }
            return nullptr;
        }
    }

    if (!api.decoderDecodeIsReady(decoder_, false)) {
        // Missing blocks decode as zero (a little blur), which beats dropping the frame. PyroWave
        // wants the two coarsest levels complete and, for a frame that lost packets, a share of
        // the blocks (setLostFrameMinBlocks()), told which blocks were lost rather than never
        // sent. A frame cut short at its
        // deadline needs only the coarsest levels: it's missing the finest blocks at its end,
        // and was only cut with most of its packets in. Dropping it would leave the frame before
        // on screen for another frame, worse than the late frame it was cut short to avoid. In
        // record framing, moonlight-common-c already checked that the coarsest level arrived, and
        // which blocks were sent can't be told, so it needs nothing more.
        bool ready;
        if (partial == Partial::None) {
            ready = api.decoderDecodeIsReady(decoder_, true);
        }
        else if (recordStarts) {
            ready = api.decoderDecodeIsReadyWithSideband(decoder_, true, 0, 0.0f, nullptr, 0);
        }
        else {
            ready = api.decoderDecodeIsReadyWithSideband(decoder_, true, 2, partial == Partial::Cut ? 0.0f : lostFrameMinBlocks_,
                                                         lostBlocks_.data(), lostBlocks_.size());
        }
        if (!ready) {
            if (partial != Partial::None) {
                partialDropped_++;
            }
            return nullptr;
        }
        if (partial != Partial::None) {
            partialDecoded_++;
        }
        else if (!loggedFailure_) {
            ALOGW("Decoding an incomplete frame");
            loggedFailure_ = true;
        }
    }

    times->pushNs = nowNs() - startNs;

    PyrowavePlanes* planes = acquirePlanes();
    if (!planes) {
        times->waitedNs = waitedNs_;
        return nullptr;
    }

    pyrowave_gpu_buffers buffers {};
    for (int i = 0; i < 3; i++) {
        pyrowave_image_view& view = buffers.planes[i];
        view.image = planes->images[i];
        view.width = planeWidth(i);
        view.height = planeHeight(i);
        view.image_format = format_;
        view.view_format = format_;
        view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Into our own commands, so the decode can be timed apart from any wait for the queue. The
    // GPU must be done with the last decode in them.
    const size_t slotIndex = nextSlot_;
    DecodeSlot& slot = decodeSlots_[slotIndex];
    nextSlot_ = (nextSlot_ + 1) % kDecodeSlots;
    // PyroWave double buffers what it records with, so decoding waits for the decode two before
    // to finish on the GPU, which it often hasn't when the GPU is busy. Waiting for it here first
    // counts that as a wait rather than as decoding. It's later than the last decode in our
    // commands, so it covers that too.
    uint64_t doneValue = decodeSlots_[(slotIndex + kDecodeSlots - 2) % kDecodeSlots].value;
    if (doneValue == 0) {
        doneValue = slot.value;
    }
    if (doneValue != 0) {
        const int64_t waitStartNs = nowNs();
        const VkResult waited = waitDecoded(doneValue, UINT64_MAX);
        waitedNs_ += nowNs() - waitStartNs;
        if (waited != VK_SUCCESS) {
            ALOGE("Waiting for a decode's commands failed: %d", waited);
            release(planes);
            times->waitedNs = waitedNs_;
            return nullptr;
        }
    }
    {
        // Its timestamps are about to be reset, and its fence. Its decode is done, so anything
        // still waiting for the fence is about to return.
        std::unique_lock<std::mutex> lock(timestampMutex_);
        slot.value = 0;
        slotWaitersCv_.wait(lock, [&slot] { return slot.waiters == 0; });
    }
    if (slot.fence) {
        vk_.vkResetFences(info_.device, 1, &slot.fence);
    }

    VkCommandBuffer cmd = slot.commandBuffer;
    vk_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(cmd, &beginInfo);
    const uint32_t firstQuery = static_cast<uint32_t>(2 * slotIndex);
    if (timestampPool_) {
        vk_.vkCmdResetQueryPool(cmd, timestampPool_, firstQuery, 2);
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, firstQuery);
    }

    // With our commands, PyroWave leaves synchronizing them to us: the submission below waits
    // for the decode before at the stage that writes the planes, which makes the planes' writes
    // visible to it, and the render waits for the timeline before reading them. Without a
    // timeline, a barrier does the same for the decode before, which is ahead of this one on
    // the queue, and the render makes the writes visible itself.
    const VkPipelineStageFlags waitStage = fragmentPath_ ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                         : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (!timeline_) {
        VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vk_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, waitStage, 0, 1, &barrier, 0, nullptr,
                                 0, nullptr);
    }
    if (ownDevice()) {
        recordOwnership(cmd, *planes, true);
    }
    const int64_t recordStartNs = nowNs();
    const int64_t recordStartWaitedNs = waitedNs_;
    api.deviceSetCommandBuffer(device_, cmd);
    const pyrowave_result result = api.decoderDecodeGpuBuffer(decoder_, nullptr, nullptr, &buffers);
    api.deviceSetCommandBuffer(device_, VK_NULL_HANDLE);
    if (ownDevice()) {
        recordOwnership(cmd, *planes, false);
    }
    times->recordNs = nowNs() - recordStartNs - (waitedNs_ - recordStartWaitedNs);
    times->pyrowaveSubmitNs = queueHeldNs_;
    times->pyrowaveSubmits = queueHolds_;
    if (timestampPool_) {
        vk_.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, firstQuery + 1);
    }
    vk_.vkEndCommandBuffer(cmd);
    if (result != PYROWAVE_SUCCESS) {
        ALOGE("Decoding failed (%d)", result);
        release(planes);
        times->waitedNs = waitedNs_;
        return nullptr;
    }

    // After the decode before it, which may have been into these planes. Only the stage that
    // writes them waits, as PyroWave's own submissions did, so the start of a decode (the
    // fragment path's dequantizing) can overlap the end of the one before. Its start timestamp
    // can then be written before the one before ends.
    const uint64_t waitValue = lastValue_;
    const uint64_t signalValue = lastValue_ + 1;
    VkTimelineSemaphoreSubmitInfo timelineInfo {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = waitValue != 0 ? 1 : 0;
    timelineInfo.pWaitSemaphoreValues = &waitValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (timeline_) {
        submit.pNext = &timelineInfo;
        submit.waitSemaphoreCount = waitValue != 0 ? 1 : 0;
        submit.pWaitSemaphores = &timeline_;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline_;
    }
    VkResult submitted;
    {
        const int64_t lockStartNs = nowNs();
        std::lock_guard<std::mutex> lock(*info_.queueMutex);
        const int64_t submitStartNs = nowNs();
        waitedNs_ += submitStartNs - lockStartNs;
        submitted = vk_.vkQueueSubmit(info_.queue, 1, &submit, slot.fence);
        times->submitNs = nowNs() - submitStartNs;
    }
    times->waitedNs = waitedNs_;
    if (submitted != VK_SUCCESS) {
        ALOGE("vkQueueSubmit failed for a decode: %d", submitted);
        release(planes);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(timestampMutex_);
        slot.value = signalValue;
    }

    *readyValue = ++lastValue_;

    // GPU time per decode, by stage, over the last interval
    const int64_t now = nowNs();
    if (lastStatsNs_ == 0) {
        lastStatsNs_ = now;
    }
    else if (now - lastStatsNs_ >= kStatsIntervalNs) {
        lastStatsNs_ = now;
        api.reportPerformanceStats(device_, &logStat, nullptr, true);
        if (partialDecoded_ != 0 || partialDropped_ != 0) {
            ALOGI("Partial frames (lost packets or cut short): %u decoded from what arrived, %u too incomplete to decode",
                  partialDecoded_, partialDropped_);
            partialDecoded_ = 0;
            partialDropped_ = 0;
        }
    }
    return planes;
}

bool PyrowaveDecoder::gpuTimes(uint64_t readyValue, int64_t* startNs, int64_t* endNs) {
    if (!timestampPool_ || readyValue == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(timestampMutex_);
    for (size_t i = 0; i < kDecodeSlots; i++) {
        if (decodeSlots_[i].value != readyValue) {
            continue;
        }
        uint64_t ticks[2] = {};
        if (vk_.vkGetQueryPoolResults(info_.device, timestampPool_, static_cast<uint32_t>(2 * i), 2, sizeof(ticks),
                                      ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            return false;
        }
        const uint64_t mask = info_.timestampValidBits >= 64 ? ~0ULL : (1ULL << info_.timestampValidBits) - 1;
        *startNs = static_cast<int64_t>(static_cast<double>(ticks[0] & mask) * info_.timestampPeriodNs);
        *endNs = static_cast<int64_t>(static_cast<double>(ticks[1] & mask) * info_.timestampPeriodNs);
        return true;
    }
    return false;
}

VkResult PyrowaveDecoder::waitDecoded(uint64_t value, uint64_t timeoutNs) {
    if (timeline_) {
        VkSemaphoreWaitInfo waitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &value;
        return vk_.vkWaitSemaphores(info_.device, &waitInfo, timeoutNs);
    }

    // The decode's slot's fence, which also covers everything submitted to the queue before it.
    // A slot is only reused once a later decode is done, so if it's gone, so is this decode.
    DecodeSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(timestampMutex_);
        for (auto& candidate : decodeSlots_) {
            if (candidate.value == value) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            return VK_SUCCESS;
        }
        slot->waiters++;
    }
    const VkResult result = vk_.vkWaitForFences(info_.device, 1, &slot->fence, VK_TRUE, timeoutNs);
    {
        std::lock_guard<std::mutex> lock(timestampMutex_);
        slot->waiters--;
    }
    slotWaitersCv_.notify_all();
    return result;
}

bool PyrowaveDecoder::pushPartialFrame(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount) {
    return pushArrivedBlocks(data, size, gaps, gapCount, width_, height_, lostBlocks_, [&](size_t offset, size_t length) {
        return api.decoderPushPacket(decoder_, data + offset, length) == PYROWAVE_SUCCESS;
    });
}

void PyrowaveDecoder::release(PyrowavePlanes* planes) {
    std::lock_guard<std::mutex> lock(poolMutex_);
    planes->inUse = false;
}

PyrowavePlanes* PyrowaveDecoder::acquirePlanes() {
    PyrowavePlanes* planes = nullptr;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        for (auto& candidate : planes_) {
            if (!candidate->inUse) {
                candidate->inUse = true;
                return candidate.get();
            }
        }
        if (planes_.size() >= kMaxPlanes) {
            ALOGW("Every plane set is held");
            return nullptr;
        }
        planes_.push_back(std::make_unique<PyrowavePlanes>());
        planes = planes_.back().get();
        planes->inUse = true;
    }

    // Only this thread adds planes, so they stay at the back while they're made
    if (!createPlanes(*planes)) {
        destroyPlanes(*planes);
        std::lock_guard<std::mutex> lock(poolMutex_);
        planes_.pop_back();
        return nullptr;
    }
    return planes;
}

bool PyrowaveDecoder::createPlanes(PyrowavePlanes& planes) {
    VkDevice device = info_.device;
    if (!fence_) {
        return false;
    }

    // With a decode device of its own, the memory is shared with the render device
    VkExternalMemoryImageCreateInfo externalInfo {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = ownDevice() ? &externalInfo : nullptr;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format_;
        imageInfo.extent = {planeWidth(i), planeHeight(i), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage_;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.vkCreateImage(device, &imageInfo, nullptr, &planes.images[i]) != VK_SUCCESS) {
            ALOGE("vkCreateImage failed");
            return false;
        }

        VkMemoryRequirements requirements {};
        vk_.vkGetImageMemoryRequirements(device, planes.images[i], &requirements);
        VkPhysicalDeviceMemoryProperties memoryProps {};
        vk_.vkGetPhysicalDeviceMemoryProperties(info_.physicalDevice, &memoryProps);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memoryProps.memoryTypeCount; t++) {
            if ((requirements.memoryTypeBits & (1u << t)) &&
                    (memoryProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = t;
                break;
            }
        }
        if (type == UINT32_MAX) {
            ALOGE("No device local memory for the planes");
            return false;
        }

        // Exported memory is given to its image alone, as the import into the render device is
        VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = planes.images[i];
        VkExportMemoryAllocateInfo exportInfo {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.pNext = ownDevice() ? &exportInfo : nullptr;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = type;
        if (vk_.vkAllocateMemory(device, &allocInfo, nullptr, &planes.memory[i]) != VK_SUCCESS) {
            ALOGE("vkAllocateMemory failed for a plane");
            return false;
        }
        vk_.vkBindImageMemory(device, planes.images[i], planes.memory[i], 0);
        if (ownDevice() && !importPlane(planes, i, imageInfo, type, requirements.size)) {
            return false;
        }

        VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = planes.images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk_.vkCreateImageView(device, &viewInfo, nullptr, &planes.views[i]) != VK_SUCCESS) {
            ALOGE("vkCreateImageView failed for a plane");
            return false;
        }
    }

    // Sampled on the render device
    VkDescriptorSetAllocateInfo setInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    if (renderVk().vkAllocateDescriptorSets(renderDevice(), &setInfo, &planes.descriptorSet) != VK_SUCCESS) {
        ALOGE("vkAllocateDescriptorSets failed for planes");
        planes.descriptorSet = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo imageDescriptors[3] {};
    for (int i = 0; i < 3; i++) {
        imageDescriptors[i].imageView = ownDevice() ? planes.renderViews[i] : planes.views[i];
        imageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = planes.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = imageDescriptors;
    renderVk().vkUpdateDescriptorSets(renderDevice(), 1, &write, 0, nullptr);

    // Into GENERAL, where the planes stay. The next decode waits for the value this signals.
    // The fence only guards the command buffer, which the last new planes may still be using.
    const int64_t waitStartNs = nowNs();
    vk_.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
    waitedNs_ += nowNs() - waitStartNs;
    vk_.vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(commandBuffer_, &beginInfo);
    VkImageMemoryBarrier barriers[3] {};
    for (int i = 0; i < 3; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = 0;
        barriers[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = planes.images[i];
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vk_.vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 3, barriers);
    vk_.vkEndCommandBuffer(commandBuffer_);

    const uint64_t waitValue = lastValue_;
    const uint64_t signalValue = lastValue_ + 1;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timelineInfo {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = waitValue != 0 ? 1 : 0;
    timelineInfo.pWaitSemaphoreValues = &waitValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    // Without a timeline, the barrier's ALL_COMMANDS covers the decodes after it on the queue
    if (timeline_) {
        submit.pNext = &timelineInfo;
        submit.waitSemaphoreCount = waitValue != 0 ? 1 : 0;
        submit.pWaitSemaphores = &timeline_;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline_;
    }

    vk_.vkResetFences(device, 1, &fence_);
    VkResult result;
    {
        const int64_t lockStartNs = nowNs();
        std::lock_guard<std::mutex> lock(*info_.queueMutex);
        waitedNs_ += nowNs() - lockStartNs;
        result = vk_.vkQueueSubmit(info_.queue, 1, &submit, fence_);
    }
    if (result != VK_SUCCESS) {
        ALOGE("vkQueueSubmit failed for a plane transition: %d", result);
        // Nothing will signal the fence now, so start over with a signaled one
        vk_.vkDestroyFence(device, fence_, nullptr);
        VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vk_.vkCreateFence(device, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) {
            fence_ = VK_NULL_HANDLE;
        }
        return false;
    }
    lastValue_ = signalValue;
    return true;
}

bool PyrowaveDecoder::importPlane(PyrowavePlanes& planes, int plane, const VkImageCreateInfo& imageInfo,
                                  uint32_t memoryType, VkDeviceSize size) {
    if (!vk_.vkGetMemoryFdKHR) {
        ALOGE("vkGetMemoryFdKHR not available");
        return false;
    }
    VkMemoryGetFdInfoKHR getInfo {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    getInfo.memory = planes.memory[plane];
    getInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (vk_.vkGetMemoryFdKHR(info_.device, &getInfo, &fd) != VK_SUCCESS) {
        ALOGE("vkGetMemoryFdKHR failed for a plane");
        return false;
    }

    // The same image on the render device, over the same memory. A successful import takes the fd.
    const VkApi& rvk = renderVk();
    const VkDevice rdev = renderDevice();
    if (rvk.vkCreateImage(rdev, &imageInfo, nullptr, &planes.renderImages[plane]) != VK_SUCCESS) {
        ALOGE("vkCreateImage failed for a plane on the render device");
        close(fd);
        return false;
    }
    VkMemoryRequirements requirements {};
    rvk.vkGetImageMemoryRequirements(rdev, planes.renderImages[plane], &requirements);
    if (!(requirements.memoryTypeBits & (1u << memoryType))) {
        ALOGE("The render device can't take a plane's memory type");
        close(fd);
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = planes.renderImages[plane];
    VkImportMemoryFdInfoKHR importInfo {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importInfo.pNext = &dedicated;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    importInfo.fd = fd;
    VkMemoryAllocateInfo allocInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = size;
    allocInfo.memoryTypeIndex = memoryType;
    if (rvk.vkAllocateMemory(rdev, &allocInfo, nullptr, &planes.renderMemory[plane]) != VK_SUCCESS) {
        ALOGE("Importing a plane's memory into the render device failed");
        close(fd);
        return false;
    }
    rvk.vkBindImageMemory(rdev, planes.renderImages[plane], planes.renderMemory[plane], 0);

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = planes.renderImages[plane];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format_;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (rvk.vkCreateImageView(rdev, &viewInfo, nullptr, &planes.renderViews[plane]) != VK_SUCCESS) {
        ALOGE("vkCreateImageView failed for a plane on the render device");
        return false;
    }
    return true;
}

void PyrowaveDecoder::recordOwnership(VkCommandBuffer cmd, const PyrowavePlanes& planes, bool acquire) {
    VkImageMemoryBarrier barriers[3] {};
    for (int i = 0; i < 3; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = acquire ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[i].dstAccessMask = acquire ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : info_.queueFamily;
        barriers[i].dstQueueFamilyIndex = acquire ? info_.queueFamily : VK_QUEUE_FAMILY_EXTERNAL;
        barriers[i].image = planes.images[i];
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vk_.vkCmdPipelineBarrier(cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 3, barriers);
}

void PyrowaveDecoder::destroyPlanes(PyrowavePlanes& planes) {
    VkDevice device = info_.device;
    if (planes.descriptorSet) renderVk().vkFreeDescriptorSets(renderDevice(), descriptorPool_, 1, &planes.descriptorSet);
    for (int i = 0; i < 3; i++) {
        if (planes.renderViews[i]) renderVk().vkDestroyImageView(renderDevice(), planes.renderViews[i], nullptr);
        if (planes.renderImages[i]) renderVk().vkDestroyImage(renderDevice(), planes.renderImages[i], nullptr);
        if (planes.renderMemory[i]) renderVk().vkFreeMemory(renderDevice(), planes.renderMemory[i], nullptr);
    }
    for (int i = 0; i < 3; i++) {
        if (planes.views[i]) vk_.vkDestroyImageView(device, planes.views[i], nullptr);
        if (planes.images[i]) vk_.vkDestroyImage(device, planes.images[i], nullptr);
        if (planes.memory[i]) vk_.vkFreeMemory(device, planes.memory[i], nullptr);
    }
    planes = PyrowavePlanes {};
}

}  // namespace vkr
