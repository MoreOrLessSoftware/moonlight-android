#pragma once

#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <vector>

#include "frame_pacer.h"
#include "ndk_api.h"
#include "pacer_trace.h"
#include "present_scheduler.h"
#include "pyrowave_decoder.h"
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
    bool pyrowave = false;     // The stream is PyroWave, decoded by the renderer rather than MediaCodec
    std::string traceDirectory;  // Where pacer traces go when enabled (see PacerTrace)
};

// Decoded frame held until it is replaced on screen and the GPU is done with it: an image from
// the image reader, or planes PyroWave decoded into
struct VideoFrame {
    VideoFrame(const NdkApi* ndk, AImage* image) : ndk(ndk), image(image) {}
    VideoFrame(PyrowaveDecoder* decoder, PyrowavePlanes* planes, uint64_t readyValue)
        : pyrowave(decoder), planes(planes), readyValue(readyValue) {}
    ~VideoFrame() {
        if (image) ndk->AImage_delete(image);
        if (planes) pyrowave->release(planes);
    }
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    const NdkApi* ndk = nullptr;
    AImage* image = nullptr;
    AHardwareBuffer* buffer = nullptr;  // Owned by image

    PyrowaveDecoder* pyrowave = nullptr;
    PyrowavePlanes* planes = nullptr;
    uint64_t readyValue = 0;  // Timeline value the decode into the planes signals
    int64_t decodeStartNs = 0;   // Handed to the renderer
    int64_t decodeQueuedNs = 0;  // Decode queued on the GPU

    AImageCropRect crop {};
    FrameTiming timing;
};
using FramePtr = std::shared_ptr<VideoFrame>;

// Renders MediaCodec output, or decodes and renders PyroWave, with Vulkan.
//
// MediaCodec writes into an AImageReader. Each frame's AHardwareBuffer is imported into
// Vulkan without a copy, converted from YCbCr by the sampler, dithered, and drawn to a
// swapchain on the output surface. A dedicated thread presents on Choreographer vsyncs, with
// FramePacer choosing the frame for each one.
//
// PyroWave frames are handed over whole instead, and decoded on the renderer's own device into
// Y, Cb and Cr planes that a second pipeline converts. They're paced and drawn like any other.
class VulkanRenderer {
public:
    // Whether this device can run the renderer at all
    static bool probe();

    // Whether the renderer can decode PyroWave here: the library loads, and the GPU can run it
    static bool probePyrowave();

    // Returns null if the renderer can't run on this device or surface
    static std::unique_ptr<VulkanRenderer> create(ANativeWindow* output, const RendererConfig& config);

    ~VulkanRenderer();

    // Surface MediaCodec writes into. Owned by the renderer. Null for PyroWave.
    ANativeWindow* decoderWindow() const { return decoderWindow_; }

    // Decodes a PyroWave frame, which may have lost some of its data (gaps), and queues it to be
    // shown. False if it couldn't be decoded.
    bool submitPyrowaveFrame(const uint8_t* data, size_t size, const PyrowaveDecoder::Gap* gaps, size_t gapCount,
                             int64_t hostPtsNs);

    void setHdrMode(bool enabled, const uint8_t* metadata, size_t metadataLength);

    // Network timing of a frame, for pacer traces
    bool tracing();
    void noteReceived(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs);

    // Stops presenting and lets go of the output surface. Safe to call more than once.
    void stop();

    // Frames presented since the last call
    uint32_t takePresentedFrames() { return presentedFrames_.exchange(0); }

    // For the performance overlay: what the renderer outputs, which only changes with HDR, and
    // its pacing numbers as a headline (the buffer) and details (lock state, skipped frames),
    // separated by a newline. Either part may be empty.
    std::string rendererText();
    std::string pacingText();

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

    // Create infos kept for the device's life, which PyroWave reads when it shares the device
    struct InstanceSetup {
        VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        std::vector<const char*> extensions;
        VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    };
    struct DeviceSetup {
        // With two queues, PyroWave decodes on the first (Granite, its Vulkan backend, takes
        // queue 0 for itself) and we render on the second, ahead of it
        float priorities[2] = {1.0f, 1.0f};
        VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        std::vector<const char*> extensions;
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
        PyrowaveDeviceFeatures pyrowave;
        VkDeviceCreateInfo info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    };

    static VkInstance createVkInstance(VkApi& vk, bool pyrowave, bool* hasColorspaceExt, InstanceSetup& setup);
    static bool prepareDevice(VkApi& vk, VkPhysicalDevice device, uint32_t queueFamily, bool pyrowave,
                              bool hdrMetadata, bool displayTiming, DeviceSetup& setup);

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
    bool createPyrowaveDecoder();
    void pyrowaveWaiterMain();

    void renderThreadMain();
    static void onVsyncThunk(int64_t frameTimeNanos, void* data);
    static void onRefreshRateThunk(int64_t vsyncPeriodNanos, void* data);
    static int onWakeThunk(int fd, int events, void* data);
    void onVsync(int64_t frameTimeNanos);
    void onWake();
    void trackVsyncPeriod(int64_t frameTimeNanos);

    static void onImageAvailableThunk(void* context, AImageReader* reader);
    void onImageAvailable();
    // Queues a decoded frame for the pacer, from the thread that decoded it
    void enqueueFrame(FramePtr frame);
    void wake(uint32_t flags);

    // showVsyncNs is the vsync the frame is shown for (the current one if 0); ahead marks a
    // frame presented as it arrived rather than at its vsync
    // commitNs: when a frame presented ahead was committed to its vsync, which its slack is
    // measured from (the pacer applies the present guard there)
    bool renderFrame(const FramePtr& frame, uint64_t presentId = 0, int64_t showVsyncNs = 0, bool ahead = false,
                     int64_t commitNs = 0);
    void presentAhead();
    void collectPresentTimings();
    bool ensureSwapchain();
    bool createSwapchain();
    void destroySwapchain();
    bool ensureConversion(const ConversionKey& key);
    void destroyConversion();
    bool ensurePipeline();
    bool ensurePlanarPipeline();
    bool createPipeline(VkPipelineLayout layout, VkShaderModule fragShader, VkPipeline* pipeline);
    void destroyPipelines();
    // Every use of the queue is under queueMutex_, since PyroWave submits to it from the
    // decoding thread
    void waitIdle();
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
    // PyroWave's queue, so a render doesn't wait behind the next frame's decode. Null when the
    // family has only one, which both then share. Either way queueMutex_ covers every queue.
    VkQueue decodeQueue_ = VK_NULL_HANDLE;
    std::mutex queueMutex_;
    InstanceSetup instanceSetup_;
    DeviceSetup deviceSetup_;
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
        int64_t slackNs;  // How long before its vsync a frame presented ahead was presented
        bool replaced = false;  // Another frame was presented for the same vsync
    };

    // Host frame timing with display timing: present frames as they arrive, asking for the
    // vsync they're due at. Rendering then happens while the frame would otherwise wait for its
    // vsync, so it's done well before the compositor needs it, and a shorter present delay holds.
    bool presentAhead_ = false;
    std::map<uint64_t, PendingPresent> pendingPresents_;  // Ordered: reports come in present order

    // The last frame presented ahead, so a frame replacing it at the same vsync is known
    uint64_t lastAheadPresentId_ = 0;
    int64_t lastAheadVsyncNs_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_ {};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_ {};
    // The display rotation we render in ourselves, so the compositor needn't rotate
    VkSurfaceTransformFlagBitsKHR preTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
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

    // PyroWave's planes, converted from YCbCr in the shader
    std::unique_ptr<PyrowaveDecoder> pyrowave_;
    VkShaderModule planarFragShader_ = VK_NULL_HANDLE;
    VkPipelineLayout planarPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline planarPipeline_ = VK_NULL_HANDLE;

    // PyroWave frames whose decode is queued on the GPU. A thread waits for each to finish
    // before it goes to the pacer, as MediaCodec's frames do, so the pacer times frames that can
    // be shown, and the thread handing frames over is free to decode the next.
    std::thread pyrowaveWaiter_;
    std::mutex decodingMutex_;
    std::condition_variable decodingCv_;
    std::deque<FramePtr> decoding_;
    bool decodingQuit_ = false;

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
    // Held for any Vulkan work: the render thread's, and frames the image reader presents ahead
    // itself. Taken before mutex_, never while holding it.
    std::mutex renderMutex_;
    bool renderStopped_ = false;  // Under renderMutex_: the swapchain and window are gone
    int64_t lastVsyncNs_ = 0;
    std::deque<int64_t> vsyncDeltas_;

    // Shared between the image reader callback and the render thread
    std::mutex mutex_;
    bool closing_ = false;
    FramePacer pacer_;
    PacerTrace trace_;
    std::deque<FramePtr> pending_;
    uint64_t queueOverflowDrops_ = 0;

    // Running totals of skipped frames as the overlay last sampled them, for a count over the
    // last kRecentSkipsNs
    struct SkipSample {
        int64_t timeNs;
        uint64_t total;
    };
    std::deque<SkipSample> skipSamples_;
    bool hdrEnabled_ = false;
    bool hdrChanged_ = false;
    HdrMetadata hdrMetadata_;
    std::string outputDescription_;

    // Render thread copy of the HDR state
    bool hdrActive_ = false;

    std::atomic<uint32_t> presentedFrames_ {0};
};

}  // namespace vkr
