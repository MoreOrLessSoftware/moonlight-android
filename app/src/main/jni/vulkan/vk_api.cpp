#include "vk_api.h"

#include <dlfcn.h>
#include <android/log.h>

#define LOG_TAG "VulkanRenderer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

bool VkApi::loadGlobal() {
    // The loader stays loaded for the life of the process
    static void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        ALOGE("libvulkan.so not available");
        return false;
    }

    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        ALOGE("vkGetInstanceProcAddr not found");
        return false;
    }

#define VK_LOAD_GLOBAL(name) \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    VK_GLOBAL_FUNCTIONS(VK_LOAD_GLOBAL)
#undef VK_LOAD_GLOBAL

    // vkEnumerateInstanceVersion is missing on Vulkan 1.0 loaders, which we don't support
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties || !vkEnumerateInstanceVersion) {
        ALOGE("Vulkan 1.1 loader not available");
        return false;
    }
    return true;
}

bool VkApi::loadInstance(VkInstance instance) {
    bool ok = true;
#define VK_LOAD_INSTANCE(name) \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name)); \
    if (!name) { ALOGE("Missing Vulkan function %s", #name); ok = false; }
    VK_INSTANCE_FUNCTIONS(VK_LOAD_INSTANCE)
#undef VK_LOAD_INSTANCE
    return ok;
}

bool VkApi::loadDevice(VkDevice device) {
    bool ok = true;
#define VK_LOAD_DEVICE(name) \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
    if (!name) { ALOGE("Missing Vulkan function %s", #name); ok = false; }
    VK_DEVICE_FUNCTIONS(VK_LOAD_DEVICE)
#undef VK_LOAD_DEVICE

#define VK_LOAD_OPTIONAL(name) \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    VK_OPTIONAL_DEVICE_FUNCTIONS(VK_LOAD_OPTIONAL)
#undef VK_LOAD_OPTIONAL
    return ok;
}
