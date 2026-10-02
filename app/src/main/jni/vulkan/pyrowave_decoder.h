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
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool inUse = false;
};

// Decodes PyroWave frames on the renderer's own VkDevice, into planes it samples.
//
// PyroWave (libpyrowave-shared.so, loaded at runtime) records its decode on the device's queue.
// Each decode signals a timeline semaphore that the render of that frame waits for.
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
    VkSemaphore timeline() const { return timeline_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool tenBit() const { return tenBit_; }
    bool fragmentPath() const { return fragmentPath_; }

    // The host packs frames in record framing (the nonary host): blocks in any order, with
    // padding records between them. Frames that lost packets are dropped, since finding the
    // blocks that arrived relies on this fork's Sunshine sending them in order.
    void setRecordFraming(bool recordFraming) { recordFraming_ = recordFraming; }
    bool recordFraming() const { return recordFraming_; }

    // The share of its blocks a frame that lost packets must still have to be decoded, rather
    // than dropped. PyroWave's own default is nine tenths.
    void setLostFrameMinBlocks(float fraction) { lostFrameMinBlocks_ = fraction; }

    using Gap = PyrowaveGap;

    // Whether a frame is whole. Matches MoonBridge.PARTIAL_KIND_*.
    enum class Partial : int {
        None = 0,
        Lost = 1,  // Packets were lost
        Cut = 2,   // Cut short at its deadline (LiSetPartialFrameDeadline())
    };

    // Decodes one frame into free planes. The render must wait for *readyValue on timeline()
    // before sampling them. Null if the frame couldn't be decoded, or every planes are held.
    // Not thread safe: frames come from one thread.
    //
    // A partial frame comes with its gaps, in order, if it has any. The blocks that arrived whole
    // are decoded, and the rest are left out, which blurs their area a little. PyroWave won't
    // decode it without the frame's lowest frequency blocks, or, for a frame that lost packets,
    // with fewer than setLostFrameMinBlocks() of them. A frame cut short is missing its finest
    // blocks, and showing it beats showing it late, so it needs only the lowest frequency ones.
    PyrowavePlanes* decode(const uint8_t* data, size_t size, const Gap* gaps, size_t gapCount, Partial partial,
                           uint64_t* readyValue);

    // Returns planes to the pool once nothing reads them any more. Thread safe.
    void release(PyrowavePlanes* planes);

private:
    PyrowaveDecoder(const VkApi& vk, const DeviceInfo& info);

    bool init(int width, int height, bool tenBit);
    PyrowavePlanes* acquirePlanes();
    bool createPlanes(PyrowavePlanes& planes);
    void destroyPlanes(PyrowavePlanes& planes);
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
    bool recordFraming_ = false;
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
