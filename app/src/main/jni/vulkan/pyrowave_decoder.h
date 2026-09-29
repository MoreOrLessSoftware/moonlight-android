#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

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

    // Decodes one whole frame into free planes. The render must wait for *readyValue on
    // timeline() before sampling them. Null if the frame couldn't be decoded, or every planes
    // are held. Not thread safe: frames come from one thread.
    PyrowavePlanes* decode(const void* data, size_t size, uint64_t* readyValue);

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

    bool loggedFailure_ = false;
    int64_t lastStatsNs_ = 0;
};

}  // namespace vkr
