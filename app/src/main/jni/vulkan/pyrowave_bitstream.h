#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Finds the parts of a PyroWave frame that arrived whole when some of it was lost. No Android
// or Vulkan dependencies, so it can be tested on its own (tests/pyrowave_partial_test.cpp).

namespace vkr {

// A stretch of a frame that didn't arrive
struct PyrowaveGap {
    size_t offset;
    size_t length;
};

namespace pyrowave_bitstream {
    // PyroWave's bitstream (docs/bitstream.md in PyroWave): an 8 byte header starts the frame,
    // then each 32x32 block of coefficients is a whole number of 32-bit words with an 8 byte
    // header of its own. Both headers share the first word's top half: payload words (12 bits,
    // blocks only), a 3 bit frame sequence number, and whether it's the frame header.
    struct BlockHeader {
        uint32_t payloadWords;
        uint32_t sequence;
        bool frameHeader;
        uint32_t blockIndex;
    };

    inline BlockHeader readHeader(const uint8_t* p) {
        uint16_t word;
        uint32_t second;
        memcpy(&word, p + 2, sizeof(word));
        memcpy(&second, p + 4, sizeof(second));
        return {word & 0xFFFu, (word >> 12) & 0x7u, (word >> 15) != 0, second >> 8};
    }

    constexpr size_t kHeaderSize = 8;
}

// Calls push(offset, length) for each run of whole blocks in a frame with gaps (in order), for
// the decoder to take. A block that lost any of its data is left out. Its header gives its
// length, so the next block is still found; if the header itself was lost, the next block is
// found by trying each following word as a header, which counts once it's followed by another
// that looks right (or by a gap, or the end). width and height are the frame's. Returns whether
// anything was pushed.
//
// lostBlocks gets a bit set for each block that may have been lost, by block index (bit i of
// word i / 32). The encoder leaves out blocks that are all zero, and blocks come in increasing
// order, so a block missing between two that arrived next to each other was never sent. PyroWave
// needs that to tell when a partial frame still has all of its coarsest blocks.
template <typename Push>
bool pushArrivedBlocks(const uint8_t* data, size_t size, const PyrowaveGap* gaps, size_t gapCount, uint32_t width,
                       uint32_t height, std::vector<uint32_t>& lostBlocks, Push push) {
    using namespace pyrowave_bitstream;

    // Whether any of [begin, end) is in a gap
    const auto missing = [&](size_t begin, size_t end) {
        for (size_t i = 0; i < gapCount; i++) {
            if (gaps[i].offset < end && begin < gaps[i].offset + gaps[i].length) {
                return true;
            }
        }
        return false;
    };

    // More blocks than a frame of this size can have, to reject garbage
    const uint32_t maxBlocks = (width / 32 + 2) * (height / 32 + 2) * 2;

    lostBlocks.assign((maxBlocks + 31) / 32, 0);
    const auto markLost = [&](int64_t first, int64_t last) {
        for (int64_t i = first < 0 ? 0 : first; i <= last && i < static_cast<int64_t>(maxBlocks); i++) {
            lostBlocks[static_cast<size_t>(i) / 32] |= 1u << (i % 32);
        }
    };
    // Set once track of the blocks was lost, until the next block is found
    bool lostTrack = false;

    int sequence = -1;           // The frame's, from its header or first block
    int64_t lastBlock = -1;      // Blocks come in increasing order
    const auto plausible = [&](const BlockHeader& h, size_t offset) {
        return !h.frameHeader && h.payloadWords * 4 >= kHeaderSize && offset + h.payloadWords * 4 <= size &&
               (sequence < 0 || h.sequence == static_cast<uint32_t>(sequence)) &&
               static_cast<int64_t>(h.blockIndex) > lastBlock && h.blockIndex < maxBlocks;
    };
    const auto headerArrived = [&](size_t offset) {
        return offset + kHeaderSize <= size && !missing(offset, offset + kHeaderSize);
    };

    const auto resync = [&](size_t from) -> size_t {
        for (size_t offset = (from + 3) & ~size_t(3); offset + kHeaderSize <= size; offset += 4) {
            if (!headerArrived(offset)) {
                continue;
            }
            const BlockHeader h = readHeader(data + offset);
            if (!plausible(h, offset)) {
                continue;
            }
            const size_t next = offset + h.payloadWords * 4;
            if (next + kHeaderSize > size || !headerArrived(next)) {
                return offset;
            }
            const BlockHeader after = readHeader(data + next);
            const bool padding = after.payloadWords == 0 && after.blockIndex == 0;
            if (padding || (!after.frameHeader && after.payloadWords * 4 >= kHeaderSize &&
                            after.sequence == h.sequence && after.blockIndex > h.blockIndex &&
                            after.blockIndex < maxBlocks)) {
                return offset;
            }
        }
        return size;
    };

    // Runs of whole blocks, pushed as they end
    size_t runStart = SIZE_MAX;
    size_t runEnd = 0;
    bool pushed = false;
    const auto flush = [&]() {
        if (runStart != SIZE_MAX) {
            if (push(runStart, runEnd - runStart)) {
                pushed = true;
            }
            runStart = SIZE_MAX;
        }
    };
    const auto extend = [&](size_t begin, size_t end) {
        if (runStart == SIZE_MAX) {
            runStart = begin;
        }
        runEnd = end;
    };

    size_t offset = 0;
    while (offset + kHeaderSize <= size) {
        if (!headerArrived(offset)) {
            flush();
            lostTrack = true;
            offset = resync(offset + 4);
            continue;
        }

        const BlockHeader h = readHeader(data + offset);
        if (h.frameHeader) {
            // Only the frame starts with one
            if (offset == 0) {
                sequence = static_cast<int>(h.sequence);
                extend(0, kHeaderSize);
                offset = kHeaderSize;
            }
            else {
                flush();
                lostTrack = true;
                offset = resync(offset + 4);
            }
            continue;
        }

        if (!plausible(h, offset)) {
            // Padding after the last block, or data that isn't what it should be
            flush();
            lostTrack = true;
            offset = resync(offset + 4);
            continue;
        }

        const size_t end = offset + h.payloadWords * 4;
        if (sequence < 0) {
            sequence = static_cast<int>(h.sequence);
        }
        if (lostTrack) {
            // Any block since the last one found may have been in what was lost
            markLost(lastBlock + 1, static_cast<int64_t>(h.blockIndex) - 1);
            lostTrack = false;
        }
        lastBlock = h.blockIndex;
        if (missing(offset, end)) {
            // Its header arrived, so the next block's place is still known
            markLost(h.blockIndex, h.blockIndex);
            flush();
        }
        else {
            extend(offset, end);
        }
        offset = end;
    }
    flush();
    if (lostTrack) {
        markLost(lastBlock + 1, static_cast<int64_t>(maxBlocks) - 1);
    }
    return pushed;
}

}  // namespace vkr
