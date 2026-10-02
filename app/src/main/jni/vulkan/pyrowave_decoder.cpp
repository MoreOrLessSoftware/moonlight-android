#include "pyrowave_decoder.h"

#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <type_traits>
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

    void lockQueue(void* userdata) {
        static_cast<std::mutex*>(userdata)->lock();
    }

    void unlockQueue(void* userdata) {
        static_cast<std::mutex*>(userdata)->unlock();
    }
}

bool PyrowaveDeviceFeatures::query(const VkApi& vk, VkPhysicalDevice device) {
    features2.pNext = &vk11;
    vk11.pNext = &vk12;
    vk12.pNext = &vk13;
    vk13.pNext = nullptr;
    vk.vkGetPhysicalDeviceFeatures2(device, &features2);

    // Robustness costs performance and nothing here needs it, and protected memory needs
    // protected queues we don't make
    features2.features.robustBufferAccess = VK_FALSE;
    vk13.robustImageAccess = VK_FALSE;
    vk11.protectedMemory = VK_FALSE;

    // What PyroWave's Vulkan backend and decoder can't do without
    return vk13.synchronization2 && vk13.subgroupSizeControl && vk13.computeFullSubgroups &&
           vk12.timelineSemaphore && vk11.samplerYcbcrConversion;
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

        // The API and ABI can change between minor versions until 1.0
        uint32_t major = 0, minor = 0, patch = 0;
        api.getApiVersion(&major, &minor, &patch);
        if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
            ALOGE("Library version %u.%u.%u doesn't match the %u.%u API we were built with", major, minor, patch,
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
    deviceInfo.queue_lock_callback = &lockQueue;
    deviceInfo.queue_unlock_callback = &unlockQueue;
    deviceInfo.userdata = info_.queueMutex;
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
    VkPhysicalDeviceVulkan12Properties vk12Props {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &vk12Props;
    vk_.vkGetPhysicalDeviceProperties2(info_.physicalDevice, &props2);
    bool prefersFragment = api.prefersFragmentPath(device_) ||
                           vk12Props.driverID == VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
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
    if (vk_.vkCreateSampler(info_.device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
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
    if (vk_.vkCreateDescriptorSetLayout(info_.device, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(kMaxPlanes * 3)};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kMaxPlanes;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vk_.vkCreateDescriptorPool(info_.device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
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
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk_.vkCreateFence(info_.device, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) {
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
    if (commandPool_) vk_.vkDestroyCommandPool(device, commandPool_, nullptr);
    if (timeline_) vk_.vkDestroySemaphore(device, timeline_, nullptr);
    if (descriptorPool_) vk_.vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (setLayout_) vk_.vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (sampler_) vk_.vkDestroySampler(device, sampler_, nullptr);
}

PyrowavePlanes* PyrowaveDecoder::decode(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount,
                                        Partial partial, uint64_t* readyValue) {
    // Each frame arrives on its own, so anything left from an earlier one is stale
    api.decoderClear(decoder_);

    if (partial != Partial::None && recordFraming_) {
        partialDropped_++;
        return nullptr;
    }
    else if (partial != Partial::None) {
        if (!pushPartialFrame(data, size, gaps, gapCount)) {
            partialDropped_++;
            return nullptr;
        }
    }
    else {
        pyrowave_result result = PYROWAVE_SUCCESS;
        if (recordFraming_) {
            const bool pushed = pushRecords(data, size, [&](size_t offset, size_t length) {
                result = api.decoderPushPacket(decoder_, data + offset, length);
                return result == PYROWAVE_SUCCESS;
            });
            if (!pushed && result == PYROWAVE_SUCCESS) {
                result = PYROWAVE_ERROR_INVALID_ARGUMENT;
            }
        }
        else {
            result = api.decoderPushPacket(decoder_, data, size);
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
        // on screen for another frame, worse than the late frame it was cut short to avoid.
        const bool ready = partial != Partial::None
                ? api.decoderDecodeIsReadyWithSideband(decoder_, true, 2, partial == Partial::Cut ? 0.0f : lostFrameMinBlocks_,
                                                       lostBlocks_.data(), lostBlocks_.size())
                : api.decoderDecodeIsReady(decoder_, true);
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

    PyrowavePlanes* planes = acquirePlanes();
    if (!planes) {
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

    // After the decode before it, which may have been into these planes
    pyrowave_gpu_sync_operation acquire {};
    if (lastValue_ != 0) {
        acquire.sync = {timeline_, lastValue_};
    }
    pyrowave_gpu_sync_operation release {};
    release.sync = {timeline_, lastValue_ + 1};

    const pyrowave_result result = api.decoderDecodeGpuBuffer(decoder_, &acquire, &release, &buffers);
    if (result != PYROWAVE_SUCCESS) {
        ALOGE("Decoding failed (%d)", result);
        this->release(planes);
        return nullptr;
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

    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
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

        VkMemoryAllocateInfo allocInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = type;
        if (vk_.vkAllocateMemory(device, &allocInfo, nullptr, &planes.memory[i]) != VK_SUCCESS) {
            ALOGE("vkAllocateMemory failed for a plane");
            return false;
        }
        vk_.vkBindImageMemory(device, planes.images[i], planes.memory[i], 0);

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

    VkDescriptorSetAllocateInfo setInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    if (vk_.vkAllocateDescriptorSets(device, &setInfo, &planes.descriptorSet) != VK_SUCCESS) {
        ALOGE("vkAllocateDescriptorSets failed for planes");
        planes.descriptorSet = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo imageDescriptors[3] {};
    for (int i = 0; i < 3; i++) {
        imageDescriptors[i].imageView = planes.views[i];
        imageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = planes.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = imageDescriptors;
    vk_.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Into GENERAL, where the planes stay. The next decode waits for the value this signals.
    // The fence only guards the command buffer, which the last new planes may still be using.
    vk_.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
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
    submit.pNext = &timelineInfo;
    submit.waitSemaphoreCount = waitValue != 0 ? 1 : 0;
    submit.pWaitSemaphores = &timeline_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &timeline_;

    vk_.vkResetFences(device, 1, &fence_);
    VkResult result;
    {
        std::lock_guard<std::mutex> lock(*info_.queueMutex);
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

void PyrowaveDecoder::destroyPlanes(PyrowavePlanes& planes) {
    VkDevice device = info_.device;
    if (planes.descriptorSet) vk_.vkFreeDescriptorSets(device, descriptorPool_, 1, &planes.descriptorSet);
    for (int i = 0; i < 3; i++) {
        if (planes.views[i]) vk_.vkDestroyImageView(device, planes.views[i], nullptr);
        if (planes.images[i]) vk_.vkDestroyImage(device, planes.images[i], nullptr);
        if (planes.memory[i]) vk_.vkFreeMemory(device, planes.memory[i], nullptr);
    }
    planes = PyrowavePlanes {};
}

}  // namespace vkr
