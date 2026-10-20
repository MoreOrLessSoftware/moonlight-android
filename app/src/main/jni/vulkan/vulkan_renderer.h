#pragma once

#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <algorithm>
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
#include "gpu_keep_busy.h"
#include "ndk_api.h"
#include "pacer_trace.h"
#include "perf_hint.h"
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
    bool pyrowaveCenterChroma = false;  // From the nonary host: 4:2:0 chroma sited at the center
    int pyrowaveLateFrames = 2;  // PreferenceConfiguration.PYROWAVE_LATE_FRAMES_*
    bool frontBuffer = false;    // A front buffer aligned to the scan, where the device has one
    bool pyrowaveGpuBoost = false;  // Keep the GPU busy between PyroWave decodes (GpuKeepBusy)
    std::string codec;           // h264, hevc, av1 or pyrowave, for pacer traces
    std::string decoderName;     // The MediaCodec decoder, empty for PyroWave, for pacer traces
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
    PyrowaveDecoder::DecodeTimes decodeTimes;  // Where the time until then went
    int64_t lastPacketUs = 0;    // When its last packet arrived, on moonlight-common-c's clock

    // Where the capture latency statistics count from, on our clock: the host's capture with clock
    // sync, or else its first packet arriving less the host's processing time. Zero when the host
    // gave no processing time (the frame repeats an earlier one).
    int64_t originNs = 0;
    bool originSynced = false;
    bool presentCounted = false;  // Its capture to present is counted, or will be when reported

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

    // Decodes a PyroWave frame, which may have lost some of its data (gaps) or been cut short,
    // and queues it to be shown. lastPacketUs is when its last packet arrived, on
    // moonlight-common-c's clock. False if it couldn't be decoded.
    //
    // Also tells moonlight-common-c when to cut short frames still arriving (see
    // updatePartialDeadline()). It's called on moonlight-common-c's decoder thread, which stops
    // before the video stream does.
    bool submitPyrowaveFrame(const uint8_t* data, size_t size, const PyrowaveDecoder::Gap* gaps, size_t gapCount,
                             const std::vector<size_t>* recordStarts, PyrowaveDecoder::Partial partial,
                             int64_t hostPtsNs, int64_t lastPacketUs);

    void setHdrMode(bool enabled, const uint8_t* metadata, size_t metadataLength);

    // Switches the jitter buffer preset for the rest of the stream
    void setJitterBuffer(JitterBuffer jitterBuffer);

    // How long after the display's vsync Choreographer's frame times are (Display's app vsync
    // offset), which a front buffer has to allow for to know when the scan starts
    void setVsyncOffsetNs(int64_t offsetNs) { vsyncOffsetNs_ = offsetNs; }

    // Network timing of a frame, for pacer traces and the capture latency statistics
    void noteReceived(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs, int64_t hostProcessingNs);

    // Stops presenting and lets go of the output surface. Safe to call more than once.
    void stop();

    // Frames presented since the last call
    uint32_t takePresentedFrames() { return presentedFrames_.exchange(0); }
    // A front buffer was asked for but the swapchain couldn't have one, so the normal one is used
    bool frontBufferUnavailable() const { return frontBufferUnavailable_; }
    // The swapchain is a front buffer
    bool frontBufferActive() const { return frontBuffer_; }

    // Total PyroWave decode time since the last call, leaving out waits: each frame's CPU work,
    // then its run on the GPU. Where the GPU can't time it, from handing the frame to the renderer
    // until the GPU was done with it.
    int64_t takeDecodedNs() { return decodedNs_.exchange(0); }

    // Capture latency since the last call: the time from each frame's origin (see
    // VideoFrame::originNs) until it was decoded, and until it reached the screen. Frames measured
    // without clock sync are counted apart, as their time leaves out the network transit.
    struct LatencySpan {
        int64_t totalNs = 0;
        uint32_t frames = 0;
        uint32_t unsynced = 0;
    };
    struct CaptureLatency {
        LatencySpan decoded;
        LatencySpan presented;
    };
    CaptureLatency takeCaptureLatency();

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
        // Chained to `queue` when the queues are created at a higher global priority
        VkDeviceQueueGlobalPriorityCreateInfoKHR globalPriority {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    };

    static VkInstance createVkInstance(VkApi& vk, bool pyrowave, bool* hasColorspaceExt, InstanceSetup& setup,
                                       bool frontBuffer = false);
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
    static int onAlignedTimerThunk(int fd, int events, void* data);
    void onVsync(int64_t frameTimeNanos);
    void onWake();
    // Front buffer aligned to the scan (debug.moonlight.front_buffer 2): sets the timer for the
    // draw that finishes as the next scan it can make starts
    void scheduleAlignedDraw();
    int64_t alignedLeadNs() const;
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
    // onArrival marks a frame shown as soon as it's decoded (presentNewest(), keepAlive()), not paced
    bool renderFrame(const FramePtr& frame, uint64_t presentId = 0, int64_t showVsyncNs = 0, bool ahead = false,
                     int64_t commitNs = 0, bool onArrival = false);
    void presentAhead();
    // Shows the newest waiting frame now, dropping any older ones: lowest latency mode with a
    // mailbox swapchain, or a front buffer. targetVsyncNs is the scan an aligned front buffer
    // drew it for, for the trace.
    void presentNewest(int64_t targetVsyncNs = 0);
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
    // PyroWave decoding on a device of its own, with a front buffer (or as
    // debug.moonlight.pyrowave_device says). A device's queues share one GPU priority, and the GPU
    // finishes a decode before starting a draw queued behind it at the same priority: on an
    // iPlay 70 mini Ultra at 144 fps, front buffer draws queued while a decode ran started
    // p50 3.2 ms (p90 4.5 ms) late and tore anywhere on the screen. With the decodes on a device
    // at the default priority, rendering has high priority, and the GPU interrupts a decode for
    // a draw: those draws started p50 0.3 ms (p90 0.6 ms) after being handed over, as on an idle
    // GPU, and nothing tore. Decodes took 1.2 ms longer, being interrupted, but frames reached
    // the screen as soon after their last packet (p50 14.6 ms against 14.4 ms). Decodes stay at
    // the default priority, so they don't hold up the compositor either. The planes' memory is
    // shared between the devices (PyrowaveDecoder::DeviceInfo).
    bool pyrowaveOwnDeviceRequested_ = false;
    bool hasExternalMemoryFdExt_ = false;
    VkDevice decodeDevice_ = VK_NULL_HANDLE;
    VkApi decodeVk_;
    DeviceSetup decodeDeviceSetup_;
    VkQueue decodeDeviceQueue_ = VK_NULL_HANDLE;
    std::mutex decodeDeviceQueueMutex_;
    // Makes decodeDevice_, or leaves it null if it can't
    void createDecodeDevice();
    std::mutex queueMutex_;
    InstanceSetup instanceSetup_;
    DeviceSetup deviceSetup_;
    uint32_t queueFamily_ = 0;
    bool hasColorspaceExt_ = false;
    bool hasHdrMetadataExt_ = false;
    bool hasDisplayTimingExt_ = false;
    // Front-buffer rendering, asked for with adb shell setprop debug.moonlight.front_buffer 1: a
    // single swapchain image the display reads from as we draw into it
    // (VK_KHR_shared_presentable_image), shown on arrival without waiting for the compositor to
    // take a new buffer. Frames tear where the display is reading as they're drawn. Requested is
    // read once at creation; frontBuffer_ is whether the swapchain got it.
    //
    // With 2, aligned to the scan: each frame is drawn to finish as the display starts a new
    // scan, so the draw stays ahead of it and any tear is in the first rows. Frames wait for the
    // next scan they can make, and the newest is drawn then (scheduleAlignedDraw()).
    bool frontBufferRequested_ = false;
    bool frontBufferAligned_ = false;
    bool hasSharedPresentExt_ = false;
    std::atomic<bool> frontBuffer_ {false};
    std::atomic<bool> frontBufferUnavailable_ {false};
    VkImageLayout renderPassFinalLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    // VK_KHR_global_priority or VK_EXT_global_priority if the device has either, and whether
    // our queues got high priority with it (createDevice())
    const char* globalPriorityExt_ = nullptr;
    bool highPriorityQueue_ = false;
    // Work keeping the GPU busy between PyroWave decodes, if switched on (gpu_keep_busy.h)
    GpuKeepBusy keepBusy_;
    uint32_t keepBusyGroups_ = 0;  // Its workgroups, 0 when off
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
        int64_t originNs = 0;   // For the capture to present statistic, if it's counted at this present
        bool originSynced = false;
        bool onArrival = false;  // Shown on arrival (mailbox), tagged only for its report
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

    // While tracing, timestamps of each frame's rendering on the GPU: its start, after taking
    // the decoder's buffer, after drawing, and its end (after handing the buffer back). Also the
    // present each frame in flight belongs to (0 if none).
    static constexpr uint32_t kTimestampsPerFrame = 4;
    VkQueryPool timestampPool_ = VK_NULL_HANDLE;
    bool gpuTiming_ = false;
    uint32_t timestampValidBits_ = 0;
    double timestampPeriodNs_ = 0;
    uint64_t slotPresentIds_[kFramesInFlight] {};
    FramePtr current_;

    // Keep-alive presents (keepAlive()): how often the frame on screen is presented again while
    // the host sends nothing (0 for never), and when a new frame and any frame were last
    // presented. Render thread, under renderMutex_.
    int64_t keepAliveNs_ = 0;
    int64_t lastNewFrameNs_ = 0;
    int64_t lastPresentNs_ = 0;
    static constexpr int64_t kKeepAliveAfterNs = 50'000'000;
    void keepAlive();

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
    std::atomic<bool> presentOnArrival_ {false};  // Mailbox swapchain in lowest latency mode, or a front buffer

    // The Instant jitter buffer, with host frame timing pacing: no buffer, each frame presented
    // ahead for the next vsync it can make (FramePacer::frameVsyncNs()). For the overlay's text.
    std::atomic<bool> instant_ {false};
    // Held for any Vulkan work: the render thread's, and frames the image reader presents ahead
    // itself. Taken before mutex_, never while holding it.
    std::mutex renderMutex_;
    bool renderStopped_ = false;  // Under renderMutex_: the swapchain and window are gone
    int64_t lastVsyncNs_ = 0;
    std::deque<int64_t> vsyncDeltas_;

    // Render thread, aligned front buffer: the timer for the next draw, the scan it's for, and
    // how long recent draws took from starting to being submitted to the GPU
    int alignedTimerFd_ = -1;
    bool alignedTimerSet_ = false;
    int64_t alignedVsyncNs_ = 0;
    // With host frame timing pacing, the pacer still chooses each frame's scan, as when frames
    // are presented ahead; the frame is then drawn at that scan less the lead, in place of
    // presenting it with a time. Under renderMutex_, in scan order.
    std::atomic<bool> frontBufferPaced_ {false};
    struct AlignedDraw {
        FramePtr frame;
        uint64_t presentId;
        int64_t vsyncNs;
        int64_t commitNs;
    };
    std::deque<AlignedDraw> alignedDraws_;
    // Paced: queues a frame the pacer committed to a scan, draws the ones whose time has come,
    // and sets the timer for the next
    void queueAlignedDraw(AlignedDraw draw);
    void drawDueAligned();
    void setAlignedTimer();
    std::deque<int64_t> drawSubmitNs_;
    // How long before the scan to start drawing, if fixed with
    // adb shell setprop debug.moonlight.front_buffer_lead_us <us>; otherwise learned
    int64_t fixedAlignedLeadNs_ = -1;
    // Choreographer's frame times are when apps are woken, the display's vsync plus this (with
    // SurfaceFlinger's app phase on an iPlay 70 mini Ultra 0.15 ms at 144 Hz but 1.0 ms at 120 Hz,
    // where a front buffer tore at the edge the scan starts until it was allowed for). Leads
    // count from the scan, so each has this added.
    std::atomic<int64_t> vsyncOffsetNs_ {0};
    // The vsync period, copied for alignedLeadNs() to take the offset's phase within
    std::atomic<int64_t> offsetPeriodNs_ {0};
    // Learned, the lead is the 90th percentile of recent draws' time to submit plus this, for
    // the GPU to start on it and a little to spare. The GPU writes the screen in scan order
    // several times faster than the display reads it, so a draw starting a little early only
    // writes rows the last scan has passed, and one starting a little late still overtakes the
    // scan within the first rows. On an iPlay 70 mini Ultra at 144 Hz (draws taking 1.43 ms on
    // the GPU and 0.3 ms to submit), leads of 0.8 ms and, at 144 fps, 1.25 ms tore a sliver at
    // the edge where the scan starts, on every frame (the GPU started within 0.1 ms of the same
    // time each draw), and a lead of 1.8 ms didn't tear.
    //
    // A PyroWave stream decodes on the same GPU, which finishes a decode before starting a draw
    // queued behind it: with 4.5 ms decodes at 144 fps, draws started up to 5 ms late and tore
    // anywhere on the screen. Holding decodes back until the draws were done cost 4-8 ms of
    // latency, since the GPU clocks down to stay about 65% busy decoding whatever the frame
    // rate (decodes took 8.9 ms at 72 fps), so PyroWave can tear with a front buffer.
    static constexpr int64_t kAlignedGpuStartNs = 1'500'000;
    static constexpr int64_t kAlignedDefaultLeadNs = 1'500'000;
    static constexpr size_t kAlignedLeadSamples = 64;
    // A draw this close to its time when the timer runs is drawn then rather than waited for
    static constexpr int64_t kAlignedTimerSlackNs = 200'000;

    // Shared between the image reader callback and the render thread
    std::mutex mutex_;
    bool closing_ = false;
    FramePacer pacer_;
    PacerTrace trace_;
    std::deque<FramePtr> pending_;
    uint64_t queueOverflowDrops_ = 0;

    // The deadline frames still arriving are cut short at (LiSetPartialFrameDeadline()), worked
    // out as frames arrive and handed to moonlight-common-c with the next frame submitted
    void updatePartialDeadline(const VideoFrame& frame);
    bool partialDeadlineEnabled_ = false;
    int64_t partialDeadlineOffsetUs_ = 0;
    // What moonlight-common-c was last told. Only on its decoder thread. Taken as set at first,
    // since a renderer before this one may have set a deadline, so the first frame clears it.
    bool partialDeadlineSent_ = true;
    int64_t partialDeadlineSentUs_ = 0;
    // From the PyroWave late frames setting (config.pyrowaveLateFrames): whether frames are cut,
    // only with at least this share of their packets in, and how much earlier than they must
    // be. debug.moonlight.partial (0 off, 1 on), debug.moonlight.partial_min_pct and
    // debug.moonlight.partial_margin_us override it, with adb shell setprop. Read when the
    // renderer is made.
    bool partialEnabled_ = true;
    int partialMinPercent_ = 30;
    int64_t partialMarginNs_ = 2'000'000;
    // Also from the setting, or debug.moonlight.partial_lost_pct: the share of its blocks a frame
    // that lost packets must still have to be shown (PyrowaveDecoder::setLostFrameMinBlocks())
    int lostFrameMinPercent_ = 75;

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
    std::atomic<int64_t> decodedNs_ {0};

    // Capture latency statistics. Frames' origins wait here, by host timestamp, until they're
    // decoded.
    struct FrameOrigin {
        int64_t ns;
        bool synced;
    };
    void noteDecoded(VideoFrame& frame);
    void countLatency(LatencySpan& span, int64_t latencyNs, bool synced);
    std::mutex latencyMutex_;
    std::map<int64_t, FrameOrigin> origins_;
    CaptureLatency captureLatency_;

    // The PyroWave waiting thread's: when the last decode it timed ended on the GPU
    int64_t lastDecodeGpuEndNs_ = 0;

    // Whether trace_ is recording, read without mutex_. Writes are still made under it.
    std::atomic<bool> traceRecording_ {false};

    // The threads that render, present and pass frames on, which renders report to (perf_hint.h).
    // The image reader and PyroWave decode threads join on their first frame, checked by thread id
    // since the decode thread can outlive a renderer.
    PerfHint perfHint_;
    std::atomic<pid_t> imageReaderTid_ {0};
    std::atomic<pid_t> pyrowaveDecodeTid_ {0};
};

}  // namespace vkr
