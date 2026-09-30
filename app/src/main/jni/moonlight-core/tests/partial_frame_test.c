// Checks moonlight-common-c's partial frames (CAPABILITY_PARTIAL_FRAMES): sends PyroWave frames
// through the real RTP video queue and depacketizer as Sunshine packetizes them, loses some
// packets, and checks what the decoder gets. Not part of the app build; build and run on the
// host from this folder with (see partial_frame_test.sh):
//   gcc -std=c11 -O1 -DHAS_SOCKLEN_T -DNDEBUG -I../moonlight-common-c/src -I../moonlight-common-c/enet/include
//       -I../moonlight-common-c/nanors -I../moonlight-common-c/nanors/deps -I../moonlight-common-c/nanors/deps/obl
//       partial_frame_test.c ../moonlight-common-c/src/RtpVideoQueue.c ../moonlight-common-c/src/VideoDepacketizer.c
//       ../moonlight-common-c/src/ByteBuffer.c ../moonlight-common-c/nanors/rs.c
//       ../moonlight-common-c/nanors/deps/obl/oblas_common.c ../moonlight-common-c/nanors/deps/obl/oblas_lite.c

#include "Limelight-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Globals and functions of the parts of moonlight-common-c this doesn't build
int AppVersionQuad[4] = {7, 1, 431, -1};  // Sunshine
STREAM_CONFIGURATION StreamConfig;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
int NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;  // Limelog() is quiet without logMessage
uint64_t PltGetMicroseconds(void) { static uint64_t t = 1000; return t += 10; }
bool isReferenceFrameInvalidationEnabled(void) { return false; }
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) { (void)startFrame; (void)endFrame; }
void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) { (void)frameIndex; (void)frameIsLTR; }
void connectionSawFrame(uint32_t frameIndex) { (void)frameIndex; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) { (void)fecStatus; }
void notifyKeyFrameReceived(void) {}
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
void LiRequestIdrFrame(void) {}
int LbqInitializeLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE queueHead, int sizeBound) { (void)queueHead; (void)sizeBound; return 0; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqDestroyLinkedBlockingQueue(PLINKED_BLOCKING_QUEUE queueHead) { (void)queueHead; return NULL; }
PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE queueHead) { (void)queueHead; return NULL; }
int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE queueHead, void* data, PLINKED_BLOCKING_QUEUE_ENTRY entry) { (void)queueHead; (void)data; (void)entry; return 0; }
int LbqWaitForQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) { (void)queueHead; (void)data; return 0; }
int LbqPollQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) { (void)queueHead; (void)data; return 0; }
int LbqPeekQueueElement(PLINKED_BLOCKING_QUEUE queueHead, void** data) { (void)queueHead; (void)data; return 0; }
void LbqSignalQueueShutdown(PLINKED_BLOCKING_QUEUE queueHead) { (void)queueHead; }
void LbqSignalQueueUserWake(PLINKED_BLOCKING_QUEUE queueHead) { (void)queueHead; }
int LbqGetItemCount(PLINKED_BLOCKING_QUEUE queueHead) { (void)queueHead; return 0; }

#define PACKET_SIZE 1392
#define PAYLOAD_SIZE (PACKET_SIZE - (int)sizeof(NV_VIDEO_PACKET))
#define FRAME_HEADER_SIZE 8
#define RTP_SIZE ((int)sizeof(RTP_PACKET) + 4)

static int failures;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// What the decoder got for the last frame, flattened: data, and which bytes were missing
static unsigned char got[512 * 1024];
static bool gotMissing[512 * 1024];
static int gotLength;
static int gotFrames;
static bool gotPartial;
static int gotFrameNumber;
static int gotGaps;

static int submitDecodeUnit(PDECODE_UNIT du) {
    gotFrames++;
    gotPartial = du->partialFrame;
    gotFrameNumber = du->frameNumber;
    gotLength = 0;
    gotGaps = 0;
    int total = 0;
    for (PLENTRY entry = du->bufferList; entry != NULL; entry = entry->next) {
        if (entry->bufferType == BUFFER_TYPE_MISSING) {
            memset(&gotMissing[gotLength], 1, entry->length);
            gotGaps++;
        }
        else {
            memcpy(&got[gotLength], entry->data, entry->length);
            memset(&gotMissing[gotLength], 0, entry->length);
        }
        gotLength += entry->length;
        total += entry->length;
    }
    if (total != du->fullLength) {
        printf("  FAIL fullLength %d doesn't match the entries' %d\n", du->fullLength, total);
        failures++;
    }
    return DR_OK;
}

static RTP_VIDEO_QUEUE queue;
static uint16_t nextSequence = 100;
static uint32_t nextStreamPacket = 1;

// Sends a frame of `length` bytes of codec data as Sunshine does, leaving out the packets whose
// index is set in `lost`
static void sendFrame(uint32_t frameIndex, const unsigned char* data, int length, const bool* lost) {
    const int payloadLength = FRAME_HEADER_SIZE + length;
    const int shards = (payloadLength + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    unsigned char* payload = calloc(shards, PAYLOAD_SIZE);

    // Sunshine's short frame header: type, host latency, frame type (IDR), last payload length
    payload[0] = 0x01;
    payload[1] = 0x34;
    payload[2] = 0x12;
    payload[3] = 2;
    uint16_t lastPayloadLength = (uint16_t)(payloadLength % PAYLOAD_SIZE ? payloadLength % PAYLOAD_SIZE : PAYLOAD_SIZE);
    memcpy(&payload[4], &lastPayloadLength, 2);
    memcpy(&payload[FRAME_HEADER_SIZE], data, length);

    const uint16_t firstSequence = nextSequence;
    for (int i = 0; i < shards; i++) {
        const uint16_t sequence = nextSequence++;
        const uint32_t streamPacket = nextStreamPacket++;
        if (lost != NULL && lost[i]) {
            continue;
        }

        const int received = RTP_SIZE + (int)sizeof(NV_VIDEO_PACKET) +
                             (i == shards - 1 ? lastPayloadLength : PAYLOAD_SIZE);
        char* buffer = calloc(1, RTP_SIZE + PACKET_SIZE + sizeof(RTPV_QUEUE_ENTRY));
        PRTP_PACKET rtp = (PRTP_PACKET)buffer;
        rtp->header = 0x80 | FLAG_EXTENSION;
        rtp->sequenceNumber = sequence;
        rtp->timestamp = frameIndex * 1500;
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + RTP_SIZE);
        nv->streamPacketIndex = streamPacket << 8;
        nv->frameIndex = frameIndex;
        nv->flags = FLAG_CONTAINS_PIC_DATA | (i == 0 ? FLAG_SOF : 0) | (i == shards - 1 ? FLAG_EOF : 0);
        nv->multiFecFlags = 0x10;
        nv->multiFecBlocks = 0;
        // 20% FEC, whose parity packets never come, so lost packets can't be recovered
        nv->fecInfo = ((uint32_t)shards << 22) | ((uint32_t)(sequence - firstSequence) << 12) | (20 << 4);
        memcpy(buffer + RTP_SIZE + sizeof(NV_VIDEO_PACKET), &payload[i * PAYLOAD_SIZE],
               received - RTP_SIZE - sizeof(NV_VIDEO_PACKET));

        if (RtpvAddPacket(&queue, rtp, received, (PRTPV_QUEUE_ENTRY)&buffer[RTP_SIZE + PACKET_SIZE]) != RTPF_RET_QUEUED) {
            free(buffer);
        }
    }

    // The parity packets' sequence numbers go by too
    nextSequence += (uint16_t)((shards * 20 + 99) / 100);
    free(payload);
}

// Checks the decoder got `data` for frame `frameIndex`, with exactly the bytes of the lost
// packets missing
static void checkFrame(uint32_t frameIndex, const unsigned char* data, int length, const bool* lost, int shards) {
    char what[160];
    snprintf(what, sizeof(what), "frame %u was delivered%s", frameIndex, lost ? " as a partial frame" : "");
    check(gotFrameNumber == (int)frameIndex && gotPartial == (lost != NULL), what);

    bool dataOk = true;
    bool gapsOk = true;
    for (int i = 0; i < shards; i++) {
        const int begin = i == 0 ? 0 : PAYLOAD_SIZE - FRAME_HEADER_SIZE + (i - 1) * PAYLOAD_SIZE;
        int end = begin + (i == 0 ? PAYLOAD_SIZE - FRAME_HEADER_SIZE : PAYLOAD_SIZE);
        if (end > length) end = length;
        for (int b = begin; b < end; b++) {
            if (lost != NULL && lost[i]) {
                if (b < gotLength && !gotMissing[b]) gapsOk = false;
            }
            else if (b >= gotLength || gotMissing[b] || got[b] != data[b]) {
                dataOk = false;
            }
        }
    }
    check(dataOk, "every byte that arrived is in its place");
    check(gapsOk, "the lost packets' bytes are marked missing");
    if (lost == NULL || !lost[shards - 1]) {
        check(gotLength == length, "the frame ends where it should");
    }
}

int main(void) {
    StreamConfig.packetSize = PACKET_SIZE;
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PARTIAL_FRAMES;
    VideoCallbacks.submitDecodeUnit = submitDecodeUnit;

    initializeVideoDepacketizer(PACKET_SIZE);
    RtpvInitializeQueue(&queue);

    // Frames of arbitrary bytes, a bit over 100 packets each
    const int length = 140000;
    unsigned char* frames[8];
    for (int f = 0; f < 8; f++) {
        frames[f] = malloc(length);
        for (int b = 0; b < length; b++) frames[f][b] = (unsigned char)(rand() >> 3);
    }
    const int shards = (FRAME_HEADER_SIZE + length + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    bool lost[256];

    printf("A whole frame:\n");
    sendFrame(1, frames[0], length, NULL);
    check(gotFrames == 1, "delivered as soon as it's complete");
    checkFrame(1, frames[0], length, NULL, shards);

    // Only the next frame's first packet, which is what makes the queue give up on the one before
    bool first[256];
    memset(first, 1, sizeof(first));
    first[0] = false;

    printf("Scattered middle packets lost:\n");
    memset(lost, 0, sizeof(lost));
    lost[5] = lost[20] = lost[50] = lost[51] = lost[52] = true;
    sendFrame(2, frames[1], length, lost);
    check(gotFrames == 1, "held until the next frame arrives");
    sendFrame(3, frames[2], length, first);
    check(gotFrames == 2, "then delivered");
    checkFrame(2, frames[1], length, lost, shards);

    printf("A frame after a partial one:\n");
    sendFrame(4, frames[3], length, NULL);
    check(gotFrames == 4, "the partial next frame, then this whole one, are delivered");
    checkFrame(4, frames[3], length, NULL, shards);

    printf("The first packet (frame header) lost:\n");
    memset(lost, 0, sizeof(lost));
    lost[0] = true;
    sendFrame(6, frames[5], length, lost);
    sendFrame(7, frames[6], length, first);
    checkFrame(6, frames[5], length, lost, shards);

    printf("The last packet lost:\n");
    memset(lost, 0, sizeof(lost));
    lost[shards - 1] = true;
    sendFrame(8, frames[7], length, lost);
    sendFrame(9, frames[0], length, first);
    checkFrame(8, frames[7], length, lost, shards);

    printf("Without the capability, a frame that lost packets is dropped:\n");
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    const int before = gotFrames;
    memset(lost, 0, sizeof(lost));
    lost[30] = true;
    sendFrame(10, frames[1], length, lost);
    sendFrame(11, frames[2], length, NULL);
    check(gotFrames == before + 1 && gotFrameNumber == 11 && !gotPartial, "only the next, whole frame is delivered");

    RtpvCleanupQueue(&queue);
    printf(failures ? "\n%d checks failed\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
