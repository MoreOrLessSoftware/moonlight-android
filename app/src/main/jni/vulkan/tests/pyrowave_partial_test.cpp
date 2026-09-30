// Checks pushArrivedBlocks() (pyrowave_bitstream.h) against real PyroWave frames: encodes an
// image, cuts the frame into packets as Sunshine sends it, loses some, and decodes what's left.
// Not part of the app build; it needs a Vulkan GPU and a desktop build of PyroWave:
//   g++ -std=c++17 -O2 -I.. -I<pyrowave> -I<pyrowave>/Granite/third_party/khronos/vulkan-headers/include
//       pyrowave_partial_test.cpp -L<pyrowave build>/lib -lpyrowave-shared -o pyrowave_partial_test
// and run it with libpyrowave-shared on the PATH.

#include "pyrowave_bitstream.h"

#include <vulkan/vulkan_core.h>
#include "pyrowave.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace vkr;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// Sunshine's framing: a 1392 byte packet holds a 16 byte video packet header and 1376 bytes of
// the frame, which starts with an 8 byte frame header
constexpr size_t kShardPayload = 1392 - 16;
constexpr size_t kFrameHeader = 8;

struct Planes {
    std::vector<uint8_t> y, cb, cr;
};

Planes makeImage() {
    // Detail everywhere, so every block carries data: gradients, rings and fine noise
    Planes p;
    p.y.resize(kWidth * kHeight);
    p.cb.resize(kWidth * kHeight / 4);
    p.cr.resize(kWidth * kHeight / 4);
    std::mt19937 rng(1234);
    for (int yy = 0; yy < kHeight; yy++) {
        for (int x = 0; x < kWidth; x++) {
            const double r = std::hypot(x - kWidth / 2.0, yy - kHeight / 2.0);
            const double v = 128 + 60 * std::sin(r / 9.0) + 40 * std::sin(x / 23.0) * std::cos(yy / 17.0) +
                             static_cast<int>(rng() % 17) - 8;
            p.y[yy * kWidth + x] = static_cast<uint8_t>(std::fmin(255, std::fmax(0, v)));
        }
    }
    for (int yy = 0; yy < kHeight / 2; yy++) {
        for (int x = 0; x < kWidth / 2; x++) {
            p.cb[yy * kWidth / 2 + x] = static_cast<uint8_t>(128 + 50 * std::sin(x / 31.0));
            p.cr[yy * kWidth / 2 + x] = static_cast<uint8_t>(128 + 50 * std::cos(yy / 19.0));
        }
    }
    return p;
}

pyrowave_cpu_buffer cpuBuffer(Planes& p) {
    pyrowave_cpu_buffer b {};
    b.data[0] = p.y.data();
    b.data[1] = p.cb.data();
    b.data[2] = p.cr.data();
    b.row_stride_in_bytes[0] = kWidth;
    b.row_stride_in_bytes[1] = b.row_stride_in_bytes[2] = kWidth / 2;
    b.plane_size_in_bytes[0] = p.y.size();
    b.plane_size_in_bytes[1] = b.plane_size_in_bytes[2] = p.cb.size();
    b.width = kWidth;
    b.height = kHeight;
    b.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    return b;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const double d = static_cast<double>(a[i]) - b[i];
        se += d * d;
    }
    const double mse = se / static_cast<double>(a.size());
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

// The bytes of the frame (after the frame header) that packet `shard` held
PyrowaveGap shardRange(size_t shard, size_t frameSize) {
    size_t begin = shard == 0 ? 0 : kShardPayload - kFrameHeader + (shard - 1) * kShardPayload;
    size_t end = shard == 0 ? kShardPayload - kFrameHeader : begin + kShardPayload;
    if (end > frameSize) end = frameSize;
    return {begin, end > begin ? end - begin : 0};
}

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

}  // namespace

int main() {
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        fprintf(stderr, "No Vulkan device for PyroWave\n");
        return 2;
    }

    pyrowave_encoder encoder = nullptr;
    pyrowave_encoder_create_info encoderInfo {device, kWidth, kHeight, PYROWAVE_CHROMA_SUBSAMPLING_420};
    pyrowave_decoder decoder = nullptr;
    pyrowave_decoder_create_info decoderInfo {device, kWidth, kHeight, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
    if (pyrowave_encoder_create(&encoderInfo, &encoder) != PYROWAVE_SUCCESS ||
            pyrowave_decoder_create(&decoderInfo, &decoder) != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Couldn't create the encoder or decoder\n");
        return 2;
    }

    // One frame at about 150 Mbps and 60 fps, packetized as Sunshine does: one packet
    Planes source = makeImage();
    pyrowave_cpu_buffer sourceBuffer = cpuBuffer(source);
    pyrowave_rate_control rate {300 * 1024};
    if (pyrowave_encoder_encode_cpu_synchronous(encoder, &sourceBuffer, &rate) != PYROWAVE_SUCCESS) {
        fprintf(stderr, "Encoding failed\n");
        return 2;
    }
    const size_t boundary = rate.maximum_bitstream_size + 64 * 1024;
    size_t numPackets = 0;
    pyrowave_encoder_compute_num_packets(encoder, boundary, &numPackets);
    std::vector<pyrowave_packet> packets(numPackets);
    std::vector<uint8_t> bitstream(numPackets * boundary);
    size_t outPackets = 0;
    pyrowave_encoder_packetize(encoder, packets.data(), boundary, &outPackets, bitstream.data(), bitstream.size());
    std::vector<uint8_t> frame(bitstream.begin() + packets[0].offset, bitstream.begin() + packets[0].offset + packets[0].size);
    const size_t shards = (frame.size() + kFrameHeader + kShardPayload - 1) / kShardPayload;
    printf("Frame: %zu bytes in %zu packets\n", frame.size(), shards);

    // The whole frame, as the reference
    Planes reference = makeImage();
    pyrowave_cpu_buffer referenceBuffer = cpuBuffer(reference);
    pyrowave_decoder_clear(decoder);
    pyrowave_decoder_push_packet(decoder, frame.data(), frame.size());
    check(pyrowave_decoder_decode_is_ready(decoder, false), "the whole frame is ready");
    pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &referenceBuffer);
    printf("  whole frame: %.1f dB against the source\n", psnr(source.y, reference.y));

    // Loses the given packets, pushes what's left through pushArrivedBlocks(), and decodes it if
    // PyroWave will, asking as the renderer does
    struct Result {
        bool readGap = false;
        bool pushed = false;
        bool ready = false;
        size_t pushedBytes = 0;
        size_t lostBytes = 0;
        double againstWhole = 0;
        double againstSource = 0;
    };
    std::mt19937 rng(99);
    const auto run = [&](const std::vector<size_t>& lost) {
        Result r;

        // The frame as it arrives: lost stretches hold junk, which must never be read
        std::vector<uint8_t> received = frame;
        std::vector<PyrowaveGap> gaps;
        for (size_t shard : lost) {
            const PyrowaveGap gap = shardRange(shard, frame.size());
            if (gap.length == 0) continue;
            for (size_t b = gap.offset; b < gap.offset + gap.length; b++) {
                received[b] = static_cast<uint8_t>(rng());
            }
            gaps.push_back(gap);
            r.lostBytes += gap.length;
        }

        std::vector<uint32_t> lostBlocks;
        pyrowave_decoder_clear(decoder);
        r.pushed = pushArrivedBlocks(received.data(), received.size(), gaps.data(), gaps.size(), kWidth, kHeight,
                                     lostBlocks, [&](size_t offset, size_t length) {
            for (const PyrowaveGap& gap : gaps) {
                if (gap.offset < offset + length && offset < gap.offset + gap.length) {
                    r.readGap = true;
                }
            }
            r.pushedBytes += length;
            return pyrowave_decoder_push_packet(decoder, received.data() + offset, length) == PYROWAVE_SUCCESS;
        });

        r.ready = pyrowave_decoder_decode_is_ready(decoder, false) ||
                  pyrowave_decoder_decode_is_ready_with_sideband(decoder, true, 2, 0.9f, lostBlocks.data(),
                                                                 lostBlocks.size());
        if (r.ready) {
            Planes out = makeImage();
            pyrowave_cpu_buffer outBuffer = cpuBuffer(out);
            pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &outBuffer);
            r.againstWhole = psnr(reference.y, out.y);
            r.againstSource = psnr(source.y, out.y);
        }
        return r;
    };

    // PyroWave needs the two coarsest levels whole, and they come first: how many packets they
    // take depends on the image and the bitrate
    size_t critical = 0;
    while (critical < shards && !run({critical}).ready) {
        critical++;
    }
    printf("The coarsest levels take the first %zu packets: losing one of those loses the frame\n", critical);
    check(critical > 0 && critical < shards / 4, "the coarsest levels are a small part of the frame");

    struct Case {
        const char* name;
        std::vector<size_t> lost;
        bool decodable;  // Whether PyroWave should still decode it
    };
    std::vector<Case> cases = {
        {"no packets lost", {}, true},
        {"the first packet after the coarsest levels lost", {critical}, true},
        {"a middle packet lost", {shards / 2}, true},
        {"three scattered packets lost", {shards / 4, shards / 2, shards * 3 / 4}, true},
        {"five packets in a row lost", {shards / 2, shards / 2 + 1, shards / 2 + 2, shards / 2 + 3, shards / 2 + 4}, true},
        {"the last packet lost", {shards - 1}, true},
        {"the first packet lost (frame header, coarsest blocks)", {0}, false},
        {"the last packet of the coarsest levels lost", {critical - 1}, false},
        {"a quarter of the packets lost", {}, false},
    };
    for (size_t i = shards / 8; i < shards; i += 4) {
        cases.back().lost.push_back(i);
    }

    for (const Case& c : cases) {
        printf("%s:\n", c.name);
        const Result r = run(c.lost);
        check(!r.readGap, "nothing pushed from a lost stretch");
        check(r.pushed, "blocks were pushed");
        printf("        pushed %zu of %zu bytes (%zu lost)\n", r.pushedBytes, frame.size(), r.lostBytes);
        check(r.ready == c.decodable, c.decodable ? "PyroWave decodes it" : "PyroWave declines it, as expected");
        if (r.ready) {
            printf("        %.1f dB against the whole frame, %.1f dB against the source\n", r.againstWhole,
                   r.againstSource);
            if (c.lost.empty()) {
                check(r.againstWhole >= 99.0, "identical to the whole frame");
            }
            else {
                // Missing blocks only soften their area; anything else points to misplaced data
                check(r.againstWhole > 30.0, "close to the whole frame");
            }
        }
    }

    // The same frame in record framing, as the nonary host sends it: the frame header, the
    // blocks in another order, and padding records between them
    {
        printf("record framing (blocks out of order, padding records):\n");
        std::vector<std::pair<size_t, size_t>> blocks;
        for (size_t pos = pyrowave_bitstream::kHeaderSize; pos < frame.size();) {
            const size_t length = 4 * pyrowave_bitstream::readHeader(frame.data() + pos).payloadWords;
            blocks.emplace_back(pos, length);
            pos += length;
        }
        std::shuffle(blocks.begin(), blocks.end(), rng);

        std::vector<uint8_t> records(frame.begin(), frame.begin() + pyrowave_bitstream::kHeaderSize);
        const auto pad = [&](uint32_t words) {
            const uint32_t header[2] = {0xFFFFFFFFu, words};
            records.insert(records.end(), reinterpret_cast<const uint8_t*>(header),
                           reinterpret_cast<const uint8_t*>(header) + sizeof(header));
            records.insert(records.end(), 4 * static_cast<size_t>(words), 0);
        };
        pad(3);
        for (size_t i = 0; i < blocks.size(); i++) {
            records.insert(records.end(), frame.begin() + blocks[i].first,
                           frame.begin() + blocks[i].first + blocks[i].second);
            if (i % 7 == 0) pad(static_cast<uint32_t>(i % 5));
        }

        pyrowave_decoder_clear(decoder);
        const bool rawAccepted = pyrowave_decoder_push_packet(decoder, records.data(), records.size()) == PYROWAVE_SUCCESS &&
                                 pyrowave_decoder_decode_is_ready(decoder, false);
        check(!rawAccepted, "PyroWave doesn't take padding records itself");

        pyrowave_decoder_clear(decoder);
        size_t runs = 0;
        const bool pushed = pushRecords(records.data(), records.size(), [&](size_t offset, size_t length) {
            runs++;
            return pyrowave_decoder_push_packet(decoder, records.data() + offset, length) == PYROWAVE_SUCCESS;
        });
        check(pushed, "pushRecords() took the frame");
        printf("        %zu blocks in %zu runs\n", blocks.size(), runs);
        const bool ready = pyrowave_decoder_decode_is_ready(decoder, false);
        check(ready, "the frame is ready");
        if (ready) {
            Planes out = makeImage();
            pyrowave_cpu_buffer outBuffer = cpuBuffer(out);
            pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &outBuffer);
            check(psnr(reference.y, out.y) >= 99.0, "identical to the whole frame");
        }

        std::vector<uint8_t> truncated(records.begin(), records.end() - 4);
        check(!pushRecords(truncated.data(), truncated.size(), [](size_t, size_t) { return true; }),
              "a frame cut short is rejected");
    }

    pyrowave_decoder_destroy(decoder);
    pyrowave_encoder_destroy(encoder);
    pyrowave_device_destroy(device);

    printf(failures ? "\n%d checks failed\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
