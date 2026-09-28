#pragma once

#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frame_pacer.h"
#include "ndk_api.h"
#include "pacer_trace.h"
#include "present_scheduler.h"
#include "vk_api.h"

namespace vkr {

struct RendererConfig {
    int streamWidth = 0;
    int streamHeight = 0;
    int streamFps = 60;
    int framePacing = 0;       // PreferenceConfiguration.FRAME_PACING_*
    int jitterBuffer = 1;      // PreferenceConfiguration.JITTER_BUFFER_*
    int ditherMode = 0;        // 0 = off, 1 = low, 2 = high
    int colorspace = 1;        // MoonBridge.COLORSPACE_*
    bool fullRange = false;
    bool tenBit = false;       // The stream is 10-bit
    float displayRefreshHz = 60.0f;
    std::string traceDirectory;  // Where pacer traces go when enabled (see PacerTrace)
};

// Decoded frame held from the image reader until it is replaced on screen and the GPU is done
// with it
struct VideoFrame {
    VideoFrame(const NdkApi* ndk, AImage* image) : ndk(ndk), image(image) {}
    ~VideoFrame() { ndk->AImage_delete(image); }
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    const NdkApi* ndk;
    AImage* image;
    AHardwareBuffer* buffer = nullptr;  // Owned by image
    AImageCropRect crop {};
    FrameTiming timing;
};
using FramePtr = std::shared_ptr<VideoFrame>;

// Renders MediaCodec output with Vulkan.
//
// The decoder writes into an AImageReader. Each frame's AHardwareBuffer is imported into
// Vulkan without a copy, converted from YCbCr by the sampler, dithered, and drawn to a
// swapchain on the output surface. A dedicated thread presents on Choreographer vsyncs, with
// FramePacer choosing the frame for each one.
class VulkanRenderer {
public:
    // Whether this device can run the renderer at all
    static bool probe();

    // Returns null if the renderer can't run on this device or surface
    static std::unique_ptr<VulkanRenderer> create(ANativeWindow* output, const RendererConfig& config);

    ~VulkanRenderer();

    // Surface the decoder writes into. Owned by the renderer.
    ANativeWindow* decoderWindow() const { return decoderWindow_; }

    void setHdrMode(bool enabled, const uint8_t* metadata, size_t metadataLength);

    // Network timing of a frame, for pacer traces
    bool tracing();
    void noteReceived(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs);

    // Stops presenting and lets go of the output surface. Safe to call more than once.
    void stop();

    // Frames presented since the last call
    uint32_t takePresentedFrames() { return presentedFrames_.exchange(0); }

    std::string statsText();

private:
    static constexpr int kFramesInFlight = 2;

    struct ImportedBuffer {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t lastUsed = 0;
    };

    // Everything a VkSamplerYcbcrConversion is built from. A change means new conversion,
    // sampler, descriptor layout and pipeline.
    struct ConversionKey {
        uint64_t externalFormat = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool ycbcr = false;
        VkSamplerYcbcrModelConversion model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        VkComponentMapping components {};
        VkChromaLocation xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
        VkChromaLocation yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
        VkFilter filter = VK_FILTER_NEAREST;

        bool operator==(const ConversionKey& other) const;
    };

    struct HdrMetadata {
        VkHdrMetadataEXT vk {VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        float contentPeakNits = 1000.0f;
    };

    VulkanRenderer(const NdkApi* ndk, const RendererConfig& config);

    bool init(ANativeWindow* output);
    bool createInstance();
    bool pickDevice();
    bool createDevice();
    bool createFrameResources();
    bool createShaderModules();
    bool createImageReader();

    void renderThreadMain();
    static void onVsyncThunk(int64_t frameTimeNanos, void* data);
    static void onRefreshRateThunk(int64_t vsyncPeriodNanos, void* data);
    static int onWakeThunk(int fd, int events, void* data);
    void onVsync(int64_t frameTimeNanos);
    void onWake();
    void trackVsyncPeriod(int64_t frameTimeNanos);

    static void onImageAvailableThunk(void* context, AImageReader* reader);
    void onImageAvailable();
    void wake(uint32_t flags);

    // showVsyncNs is the vsync the frame is shown for (the current one if 0); ahead marks a
    // frame presented as it arrived rather than at its vsync
    bool renderFrame(const FramePtr& frame, uint64_t presentId = 0, int64_t showVsyncNs = 0, bool ahead = false);
    void presentAhead();
    void collectPresentTimings();
    bool ensureSwapchain();
    bool createSwapchain();
    void destroySwapchain();
    bool ensureConversion(const ConversionKey& key);
    void destroyConversion();
    bool ensurePipeline();
    ImportedBuffer* importBuffer(AHardwareBuffer* buffer);
    void destroyImport(AHardwareBuffer* buffer, ImportedBuffer& imported);
    void evictImports();
    void applyHdrMetadata();

    const NdkApi* ndk_;
    const RendererConfig config_;
    VkApi vk_;

    // Output
    ANativeWindow* outputWindow_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    bool hasColorspaceExt_ = false;
    bool hasHdrMetadataExt_ = false;
    bool hasDisplayTimingExt_ = false;
    uint64_t nextPresentId_ = 1;

    // Render thread: which vsync each present should reach the screen at
    PresentScheduler presentScheduler_;
    int64_t currentVsyncNs_ = 0;
    int64_t currentPeriodNs_ = 0;
    struct PendingPresent {
        int64_t vsyncNs;
        int delayVsyncs;
        bool ahead;
    };

    // Host frame timing with display timing: present frames as they arrive, asking for the
    // vsync they're due at. Rendering then happens while the frame would otherwise wait for its
    // vsync, so it's done well before the compositor needs it, and a shorter present delay holds.
    bool presentAhead_ = false;
    std::unordered_map<uint64_t, PendingPresent> pendingPresents_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_ {};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_ {};
    int outputBits_ = 8;
    bool outputPq_ = false;
    bool swapchainDirty_ = true;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderDone_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFormat renderPassFormat_ = VK_FORMAT_UNDEFINED;

    // Video sampling
    bool haveConversion_ = false;
    ConversionKey conversionKey_;
    VkSamplerYcbcrConversion conversion_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderModule vertShader_ = VK_NULL_HANDLE;
    VkShaderModule fragShader_ = VK_NULL_HANDLE;
    std::unordered_map<AHardwareBuffer*, ImportedBuffer> imports_;

    // Per frame in flight
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFramesInFlight] {};
    VkFence fences_[kFramesInFlight] {};
    VkSemaphore imageAcquired_[kFramesInFlight] {};
    FramePtr slotFrames_[kFramesInFlight];
    uint64_t frameCounter_ = 0;
    FramePtr current_;

    // Decoder side
    AImageReader* reader_ = nullptr;
    ANativeWindow* decoderWindow_ = nullptr;
    AImageReader_ImageListener imageListener_ {};

    // Render thread
    std::thread renderThread_;
    int wakeFd_ = -1;
    std::atomic<uint32_t> wakeFlags_ {0};
    std::atomic<bool> quit_ {false};
    bool stopped_ = false;
    std::mutex stopMutex_;
    AChoreographer* choreographer_ = nullptr;
    bool refreshRateCallbackRegistered_ = false;
    std::atomic<bool> presentOnArrival_ {false};  // Mailbox swapchain in lowest latency mode
    int64_t lastVsyncNs_ = 0;
    std::deque<int64_t> vsyncDeltas_;

    // Shared between the image reader callback and the render thread
    std::mutex mutex_;
    bool closing_ = false;
    FramePacer pacer_;
    PacerTrace trace_;
    std::deque<FramePtr> pending_;
    uint64_t queueOverflowDrops_ = 0;
    bool hdrEnabled_ = false;
    bool hdrChanged_ = false;
    HdrMetadata hdrMetadata_;
    std::string outputDescription_;

    // Render thread copy of the HDR state
    bool hdrActive_ = false;

    std::atomic<uint32_t> presentedFrames_ {0};
};

}  // namespace vkr
