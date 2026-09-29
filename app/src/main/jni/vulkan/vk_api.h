#pragma once

// Vulkan is loaded at runtime (libvulkan.so is not in the API 21 sysroot we link against),
// so every entry point is called through these function pointers.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceVersion)

#define VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkCreateAndroidSurfaceKHR) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindImageMemory) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetAndroidHardwareBufferPropertiesANDROID) \
    X(vkCreateSamplerYcbcrConversion) \
    X(vkDestroySamplerYcbcrConversion) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdDraw) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore)

// Present only when the device extension that provides them is enabled, or (timeline
// semaphores) on Vulkan 1.2 devices
#define VK_OPTIONAL_DEVICE_FUNCTIONS(X) \
    X(vkSetHdrMetadataEXT) \
    X(vkGetPastPresentationTimingGOOGLE) \
    X(vkWaitSemaphores)

#define VK_DECLARE_FUNCTION(name) PFN_##name name = nullptr;

struct VkApi {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    VK_GLOBAL_FUNCTIONS(VK_DECLARE_FUNCTION)
    VK_INSTANCE_FUNCTIONS(VK_DECLARE_FUNCTION)
    VK_DEVICE_FUNCTIONS(VK_DECLARE_FUNCTION)
    VK_OPTIONAL_DEVICE_FUNCTIONS(VK_DECLARE_FUNCTION)

    // Loads libvulkan.so and the global functions
    bool loadGlobal();
    bool loadInstance(VkInstance instance);
    bool loadDevice(VkDevice device);
};
