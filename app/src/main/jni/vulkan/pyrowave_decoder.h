#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "pyrowave_bitstream.h"
#include "vk_api.h"

struct pyrowave_device_opaque;
struct pyrowave_decoder_opaque;

namespace vkr {

// Features for a device PyroWave shares: everything the device supports except robustness, in
// the chain PyroWave's Vulkan backend reads them from (VkPhysicalDeviceFeatures2 first). Points
// into itself, so it stays put.
struct PyrowaveDeviceFeatures {
    VkPhysicalDeviceFeatures2 features2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features vk11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vk12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vk13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

    PyrowaveDeviceFeatures() = default;
    PyrowaveDeviceFeatures(const PyrowaveDeviceFeatures&) = delete;
    PyrowaveDeviceFeatures& operator=(const PyrowaveDeviceFeatures&) = delete;

    // Fills in what the device supports. False if it's short of what PyroWave's decoder needs.
    bool query(const VkApi& vk, VkPhysicalDevice device);
};

// A decoded picture: a full size Y plane and half size Cb and Cr planes, which stay in the
// GENERAL layout
struct PyrowavePlanes {
    VkImage images[3] {};
    VkDeviceMemory memory[3] {};
    VkImageView views[3] {};
    // With a decode device of its own (DeviceInfo::renderDevice), the same memory imported into
    // the render device, which samples these
    VkImage renderImages[3] {};
    VkDeviceMemory renderMemory[3] {};
    VkImageView renderViews[3] {};
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;  // On the render device
    bool inUse = false;
};

// Decodes PyroWave frames on the renderer's own VkDevice, into planes it samples.
//
// PyroWave (libpyrowave-shared.so, loaded at runtime) records its decode into our commands, which
// we submit to the device's queue, timed when the queue can be. Each decode signals a timeline
// semaphore that the render of that frame waits for.
class PyrowaveDecoder {
public:
    // Loads the library and checks it has the API version we were built against. Only tried once.
    static bool loadLibrary();

    // The renderer's Vulkan objects PyroWave shares. The create infos must stay valid for the
    // decoder's life. Every submission to the queue, PyroWave's too, happens under queueMutex.
    struct DeviceInfo {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        const VkInstanceCreateInfo* instanceInfo = nullptr;
        const VkDeviceCreateInfo* deviceInfo = nullptr;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        std::mutex* queueMutex = nullptr;
        // For timing decodes on the GPU; no timing if timestampValidBits is 0
        uint32_t timestampValidBits = 0;
        float timestampPeriodNs = 0.0f;
        // Set when the decodes have a device of their own (above), at a lower GPU priority than
        // the render device here, which samples the planes: their memory is exported from the
        // decode device and imported into this one, and the planes' descriptor sets are made on
        // it. Both devices need VK_KHR_external_memory_fd. The render must take the planes over
        // from VK_QUEUE_FAMILY_EXTERNAL (and give them back) rather than wait for timeline(),
        // which is on the decode device, and only once the decode is done.
        VkDevice renderDevice = VK_NULL_HANDLE;
        const VkApi* renderVk = nullptr;
    };

    // Null if PyroWave can't decode on this device. width and height must be even.
    static std::unique_ptr<PyrowaveDecoder> create(const VkApi& vk, const DeviceInfo& info, int width, int height,
                                                   bool tenBit);

    // The GPU must be done with the planes
    ~PyrowaveDecoder();

    PyrowaveDecoder(const PyrowaveDecoder&) = delete;
    PyrowaveDecoder& operator=(const PyrowaveDecoder&) = delete;

    // Three combined image samplers, Y, Cb and Cr, in the planes' descriptor sets
    VkDescriptorSetLayout setLayout() const { return setLayout_; }
    // On the decode device
    VkSemaphore timeline() const { return timeline_; }
    // The decodes have a device of their own (DeviceInfo::renderDevice)
    bool ownDevice() const { return info_.renderDevice != VK_NULL_HANDLE; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool tenBit() const { return tenBit_; }
    bool fragmentPath() const { return fragmentPath_; }

    // The share of its blocks a frame that lost packets must still have to be decoded, rather
    // than dropped. PyroWave's own default is nine tenths.
    void setLostFrameMinBlocks(float fraction) { lostFrameMinBlocks_ = fraction; }

    using Gap = PyrowaveGap;

    // Where a decode() call's time went. The parts leave out the waits, and the rest of the call
    // (taking planes, and readying our commands) isn't broken out.
    struct DecodeTimes {
        int64_t waitedNs = 0;  // Waiting, for the queue or for the GPU, rather than decoding
        int64_t pushNs = 0;    // Pushing the frame's packets into PyroWave, and checking it can decode
        int64_t recordNs = 0;  // PyroWave recording the decode (pyrowave_decoder_decode_gpu_buffer())
        int64_t submitNs = 0;  // Submitting it
        // Of recordNs, how long PyroWave held the queue (to submit), and how many times. Taken
        // over the whole call, but only recording takes the queue.
        int64_t pyrowaveSubmitNs = 0;
        int pyrowaveSubmits = 0;
    };

    // Whether a frame is whole. Matches MoonBridge.PARTIAL_KIND_*.
    enum class Partial : int {
        None = 0,
        Lost = 1,  // Packets were lost
        Cut = 2,   // Cut short at its deadline (LiSetPartialFrameDeadline())
    };

    // Decodes one frame into free planes. The render must wait for *readyValue on timeline()
    // before sampling them. Null if the frame couldn't be decoded, or every planes are held.
    // Not thread safe: frames come from one thread. *times says where the call's time went.
    //
    // A partial frame comes with its gaps, in order, if it has any. The blocks that arrived whole
    // are decoded, and the rest are left out, which blurs their area a little. PyroWave won't
    // decode it without the frame's lowest frequency blocks, or, for a frame that lost packets,
    // with fewer than setLostFrameMinBlocks() of them. A frame cut short is missing its finest
    // blocks, and showing it beats showing it late, so it needs only the lowest frequency ones.
    //
    // Frames are in record framing, which we ask hosts for: padding records between the blocks
    // may need stripping. A partial frame whose host laid its records out on packets comes with
    // the offsets where records start in it (see pushArrivedRecords()), as its blocks needn't be
    // in order. moonlight-common-c only delivers one if the packets holding its lowest frequency
    // blocks arrived, so it's decoded with whatever else did. Otherwise recordStarts is null, and
    // its blocks are in order.
    PyrowavePlanes* decode(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount,
                           const std::vector<size_t>* recordStarts, Partial partial, uint64_t* readyValue,
                           DecodeTimes* times);

    // When the decode that signals readyValue ran on the GPU, on the GPU's clock, once the
    // timeline reaches it. False if decodes aren't timed, or its commands were reused since.
    // Thread safe.
    bool gpuTimes(uint64_t readyValue, int64_t* startNs, int64_t* endNs);

    // Returns planes to the pool once nothing reads them any more. Thread safe.
    void release(PyrowavePlanes* planes);

private:
    PyrowaveDecoder(const VkApi& vk, const DeviceInfo& info);

    bool init(int width, int height, bool tenBit);
    PyrowavePlanes* acquirePlanes();
    bool createPlanes(PyrowavePlanes& planes);
    void destroyPlanes(PyrowavePlanes& planes);
    // The queue lock PyroWave takes for its submissions, counting the wait in waitedNs_
    static void lockQueue(void* userdata);
    static void unlockQueue(void* userdata);
    // Where the planes are sampled: the render device if the decodes have their own
    const VkApi& renderVk() const { return info_.renderVk ? *info_.renderVk : vk_; }
    VkDevice renderDevice() const { return info_.renderDevice ? info_.renderDevice : info_.device; }
    // The decode's commands, with a device of its own: taking the planes from the render device
    // before PyroWave records, and handing them back after
    void recordOwnership(VkCommandBuffer cmd, const PyrowavePlanes& planes, bool acquire);
    // The memory of a plane made on the decode device, imported into the render device
    bool importPlane(PyrowavePlanes& planes, int plane, const VkImageCreateInfo& imageInfo, uint32_t memoryType,
                     VkDeviceSize size);
    uint32_t planeWidth(int plane) const { return plane == 0 ? width_ : width_ / 2; }
    uint32_t planeHeight(int plane) const { return plane == 0 ? height_ : height_ / 2; }

    const VkApi& vk_;
    const DeviceInfo info_;

    pyrowave_device_opaque* device_ = nullptr;
    pyrowave_decoder_opaque* decoder_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool tenBit_ = false;
    bool fragmentPath_ = false;
    float lostFrameMinBlocks_ = 0.9f;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    // Moves new planes into the GENERAL layout
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    // Each decode's commands, which PyroWave records into and we submit, between two timestamps
    // when they're timed. Reused in turn, once the GPU is done with them.
    struct DecodeSlot {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        uint64_t value = 0;  // Timeline value its submitted decode signals, 0 if none
    };
    static constexpr size_t kDecodeSlots = 8;
    DecodeSlot decodeSlots_[kDecodeSlots];
    size_t nextSlot_ = 0;
    VkQueryPool timestampPool_ = VK_NULL_HANDLE;  // Two per slot
    // Guards the slots' values, so a slot's timestamps aren't read as it's reused
    std::mutex timestampMutex_;

    // Time the decode in progress has spent waiting
    int64_t waitedNs_ = 0;
    // How long and how many times PyroWave has held the queue in the decode in progress
    int64_t queueLockedNs_ = 0;
    int64_t queueHeldNs_ = 0;
    int queueHolds_ = 0;

    // Signaled with each decode's value. Decodes wait for the one before, which also keeps one
    // from writing planes an earlier, dropped frame is still being decoded into.
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t lastValue_ = 0;

    // Made as needed, up to kMaxPlanes
    std::mutex poolMutex_;
    std::vector<std::unique_ptr<PyrowavePlanes>> planes_;

    // Pushes the blocks of a frame that arrived whole, skipping the gaps
    bool pushPartialFrame(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount);

    bool loggedFailure_ = false;
    int64_t lastStatsNs_ = 0;

    // Blocks of the last partial frame that may have been lost (see pushArrivedBlocks())
    std::vector<uint32_t> lostBlocks_;

    // Partial frames, since the last stats
    uint32_t partialDecoded_ = 0;
    uint32_t partialDropped_ = 0;
};

}  // namespace vkr
