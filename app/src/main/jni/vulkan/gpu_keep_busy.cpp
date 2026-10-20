#include "gpu_keep_busy.h"

#include <algorithm>
#include <android/log.h>
#include <time.h>
#include <vector>

#include "shaders_spv.h"

#define LOG_TAG "VulkanRenderer"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace vkr {

namespace {
    constexpr uint64_t kFenceTimeoutNs = 1'000'000'000;
    // Chunks are resized from the quickest quarter of this many: the slower ones waited for
    // decodes and draws
    constexpr size_t kSizingWindow = 32;
    constexpr uint32_t kMinIterations = 16;
    constexpr uint32_t kMaxIterations = 1u << 22;
    constexpr int64_t kStatsIntervalNs = 10'000'000'000;

    int64_t nowNs() {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }
}

GpuKeepBusy::~GpuKeepBusy() {
    stop();
}

bool GpuKeepBusy::start(const VkApi& vk, VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily,
                        const char* globalPriorityExt, int64_t chunkNs, uint32_t groups) {
    stop();
    vk_ = vk;
    instance_ = instance;
    chunkNs_ = chunkNs;
    groups_ = groups;
    iterations_ = 1000;
    if (!createDevice(physicalDevice, queueFamily, globalPriorityExt) || !createResources(physicalDevice, queueFamily)) {
        destroy();
        return false;
    }
    quit_ = false;
    thread_ = std::thread(&GpuKeepBusy::run, this);
    ALOGI("Keeping the GPU busy between decodes, %lld us at a time with %u workgroups, at %s priority",
          static_cast<long long>(chunkNs_ / 1000), groups_, lowPriority_ ? "low" : "the default");
    return true;
}

void GpuKeepBusy::stop() {
    if (thread_.joinable()) {
        quit_ = true;
        thread_.join();
    }
    destroy();
}

bool GpuKeepBusy::createDevice(VkPhysicalDevice physicalDevice, uint32_t queueFamily, const char* globalPriorityExt) {
    const float priority = 0.0f;
    VkDeviceQueueGlobalPriorityCreateInfoKHR globalPriority {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    globalPriority.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
    VkDeviceQueueCreateInfo queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (globalPriorityExt) {
        queueInfo.pNext = &globalPriority;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &globalPriorityExt;
        result = vk_.vkCreateDevice(physicalDevice, &info, nullptr, &device);
        lowPriority_ = result == VK_SUCCESS;
    }
    if (result != VK_SUCCESS) {
        queueInfo.pNext = nullptr;
        info.enabledExtensionCount = 0;
        info.ppEnabledExtensionNames = nullptr;
        result = vk_.vkCreateDevice(physicalDevice, &info, nullptr, &device);
    }
    if (result != VK_SUCCESS) {
        ALOGW("Couldn't make a device to keep the GPU busy on (%d)", result);
        return false;
    }
    // Only what's used here: VkApi::loadDevice() wants the swapchain's functions, which this
    // device, without its extension, doesn't have
    bool loaded = true;
#define KEEP_BUSY_LOAD(name) \
    vk_.name = reinterpret_cast<PFN_##name>(vk_.vkGetDeviceProcAddr(device, #name)); \
    loaded &= vk_.name != nullptr;
    KEEP_BUSY_LOAD(vkDestroyDevice)
    KEEP_BUSY_LOAD(vkGetDeviceQueue)
    KEEP_BUSY_LOAD(vkDeviceWaitIdle)
    KEEP_BUSY_LOAD(vkQueueSubmit)
    KEEP_BUSY_LOAD(vkCreateBuffer)
    KEEP_BUSY_LOAD(vkDestroyBuffer)
    KEEP_BUSY_LOAD(vkGetBufferMemoryRequirements)
    KEEP_BUSY_LOAD(vkBindBufferMemory)
    KEEP_BUSY_LOAD(vkAllocateMemory)
    KEEP_BUSY_LOAD(vkFreeMemory)
    KEEP_BUSY_LOAD(vkCreateDescriptorSetLayout)
    KEEP_BUSY_LOAD(vkDestroyDescriptorSetLayout)
    KEEP_BUSY_LOAD(vkCreateDescriptorPool)
    KEEP_BUSY_LOAD(vkDestroyDescriptorPool)
    KEEP_BUSY_LOAD(vkAllocateDescriptorSets)
    KEEP_BUSY_LOAD(vkUpdateDescriptorSets)
    KEEP_BUSY_LOAD(vkCreatePipelineLayout)
    KEEP_BUSY_LOAD(vkDestroyPipelineLayout)
    KEEP_BUSY_LOAD(vkCreateShaderModule)
    KEEP_BUSY_LOAD(vkDestroyShaderModule)
    KEEP_BUSY_LOAD(vkCreateComputePipelines)
    KEEP_BUSY_LOAD(vkDestroyPipeline)
    KEEP_BUSY_LOAD(vkCreateCommandPool)
    KEEP_BUSY_LOAD(vkDestroyCommandPool)
    KEEP_BUSY_LOAD(vkAllocateCommandBuffers)
    KEEP_BUSY_LOAD(vkResetCommandBuffer)
    KEEP_BUSY_LOAD(vkBeginCommandBuffer)
    KEEP_BUSY_LOAD(vkEndCommandBuffer)
    KEEP_BUSY_LOAD(vkCmdBindPipeline)
    KEEP_BUSY_LOAD(vkCmdBindDescriptorSets)
    KEEP_BUSY_LOAD(vkCmdPushConstants)
    KEEP_BUSY_LOAD(vkCmdDispatch)
    KEEP_BUSY_LOAD(vkCreateFence)
    KEEP_BUSY_LOAD(vkDestroyFence)
    KEEP_BUSY_LOAD(vkWaitForFences)
    KEEP_BUSY_LOAD(vkResetFences)
#undef KEEP_BUSY_LOAD
    if (!loaded) {
        ALOGW("Missing Vulkan functions to keep the GPU busy with");
        auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vk_.vkGetInstanceProcAddr(instance_, "vkDestroyDevice"));
        if (destroyDevice) {
            destroyDevice(device, nullptr);
        }
        return false;
    }
    device_ = device;
    vk_.vkGetDeviceQueue(device_, queueFamily, 0, &queue_);
    return true;
}

bool GpuKeepBusy::createResources(VkPhysicalDevice physicalDevice, uint32_t queueFamily) {
    VkBufferCreateInfo bufferInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = static_cast<VkDeviceSize>(groups_) * kGroupSize * sizeof(float);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk_.vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements {};
    vk_.vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties {};
    vk_.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
        if (!(requirements.memoryTypeBits & (1u << i))) {
            continue;
        }
        if (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryType = i;
            break;
        }
        if (memoryType == UINT32_MAX) {
            memoryType = i;
        }
    }
    if (memoryType == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo allocInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (vk_.vkAllocateMemory(device_, &allocInfo, nullptr, &memory_) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, buffer_, memory_, 0) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &binding;
    if (vk_.vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vk_.vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    if (vk_.vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorBufferInfo descriptorBuffer {buffer_, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &descriptorBuffer;
    vk_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    VkPushConstantRange pushRange {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vk_.vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        return false;
    }
    VkShaderModuleCreateInfo shaderInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kKeepBusyCompSpv);
    shaderInfo.pCode = kKeepBusyCompSpv;
    if (vk_.vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader_) != VK_SUCCESS) {
        return false;
    }
    VkComputePipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader_;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout_;
    if (vk_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS) {
        return false;
    }

    VkCommandPoolCreateInfo commandPoolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = queueFamily;
    if (vk_.vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo commandInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = kInFlight;
    if (vk_.vkAllocateCommandBuffers(device_, &commandInfo, commandBuffers_) != VK_SUCCESS) {
        return false;
    }
    for (VkFence& fence : fences_) {
        VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk_.vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

void GpuKeepBusy::destroy() {
    if (!device_) {
        return;
    }
    vk_.vkDeviceWaitIdle(device_);
    for (VkFence& fence : fences_) {
        if (fence) vk_.vkDestroyFence(device_, fence, nullptr);
        fence = VK_NULL_HANDLE;
    }
    if (commandPool_) vk_.vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (pipeline_) vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (shader_) vk_.vkDestroyShaderModule(device_, shader_, nullptr);
    if (pipelineLayout_) vk_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vk_.vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (setLayout_) vk_.vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (buffer_) vk_.vkDestroyBuffer(device_, buffer_, nullptr);
    if (memory_) vk_.vkFreeMemory(device_, memory_, nullptr);
    vk_.vkDestroyDevice(device_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

bool GpuKeepBusy::submit(int slot) {
    VkCommandBuffer cmd = commandBuffers_[slot];
    vk_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(cmd, &beginInfo);
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vk_.vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(iterations_), &iterations_);
    vk_.vkCmdDispatch(cmd, groups_, 1, 1);
    if (vk_.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }

    VkSubmitInfo submitInfo {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vk_.vkResetFences(device_, 1, &fences_[slot]);
    return vk_.vkQueueSubmit(queue_, 1, &submitInfo, fences_[slot]) == VK_SUCCESS;
}

void GpuKeepBusy::run() {
    // Two dispatches in flight, so the GPU has the next one when one ends. With nothing else to
    // run, they end a chunk apart; the quickest of them are sized to chunkNs_.
    for (int slot = 0; slot < kInFlight; slot++) {
        if (!submit(slot)) {
            ALOGW("Couldn't submit work to keep the GPU busy");
            return;
        }
    }

    std::vector<int64_t> intervals;
    intervals.reserve(kSizingWindow);
    int64_t lastDoneNs = nowNs();
    int64_t statsStartNs = lastDoneNs;
    uint64_t chunks = 0;
    int slot = 0;
    while (!quit_) {
        const VkResult result = vk_.vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kFenceTimeoutNs);
        if (result != VK_SUCCESS) {
            ALOGW("Work keeping the GPU busy didn't finish (%d); stopping it", result);
            return;
        }
        const int64_t now = nowNs();
        intervals.push_back(now - lastDoneNs);
        lastDoneNs = now;
        chunks++;

        if (intervals.size() >= kSizingWindow) {
            const auto quickest = intervals.begin() + intervals.size() / 4;
            std::nth_element(intervals.begin(), quickest, intervals.end());
            const int64_t chunkNs = std::max<int64_t>(*quickest, 1);
            const double scale = std::clamp(static_cast<double>(chunkNs_) / static_cast<double>(chunkNs), 0.25, 4.0);
            iterations_ = static_cast<uint32_t>(std::clamp(iterations_ * scale, static_cast<double>(kMinIterations),
                                                           static_cast<double>(kMaxIterations)));
            if (now - statsStartNs >= kStatsIntervalNs) {
                ALOGI("Kept the GPU busy: %llu chunks in %.1f s, the quickest quarter %lld us or quicker, now %u iterations",
                      static_cast<unsigned long long>(chunks), (now - statsStartNs) / 1e9,
                      static_cast<long long>(chunkNs / 1000), iterations_);
                statsStartNs = now;
                chunks = 0;
            }
            intervals.clear();
        }

        if (!submit(slot)) {
            ALOGW("Couldn't submit work to keep the GPU busy");
            return;
        }
        slot = (slot + 1) % kInFlight;
    }
}

}  // namespace vkr
