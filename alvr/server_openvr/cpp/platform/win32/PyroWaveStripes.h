// Stripe pipelining for PyroWave (ALVR extension, not part of upstream PyroWave).
//
// This header is identical in the streamer (platform/win32/PyroWaveStripes.h) and in the
// visionOS client (ALVRClient/PyroWave/pyrowave_stripes.hpp). Both sides must compute exactly
// the same schedule, so change it in both places or not at all.
//
// The idea: the picture is cut into horizontal stripes of `stripe_height` luma rows. The
// streamer sends the coded 32x32 blocks in stripe order instead of PyroWave's level order,
// so that once the packets of stripes 0..k have arrived, the decoder has every wavelet
// coefficient the inverse transform needs to produce the first rows of the picture, through
// all five levels. The client decodes stripe by stripe while the rest of the frame is still
// on the wire, and only the last stripe is left once the last packet arrives.
//
// What "needs" means comes from the Metal/Vulkan iDWT: one threadgroup ("tile") of the iDWT at
// input level L turns 16 coefficient rows of each band of level L into 32 rows of the next
// finer level, and reads 2 extra coefficient rows on each side (the apron of the 9/7 filter).
// Tile row t therefore reads coefficient rows [16t - 2, 16t + 18), mirrored at the edges, and
// the same rows of the LL band of level L, which the tiles of level L + 1 produce. Levels
// have the size PyroWave's BlockLayout gives them: (aligned size / 2) >> level, for every
// component; 4:2:0 chroma has no level 0 and is finished by the level 1 iDWT.
//
// Dependency free on purpose (C++11, standard headers only).

#ifndef PYROWAVE_STRIPES_H_
#define PYROWAVE_STRIPES_H_

#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace PyroWaveStripes {
static const int Levels = 5;
static const int Components = 3;
static const int Bands = 4;
// PyroWave pads the picture to a multiple of 1 << Levels and to at least 4 << Levels.
static const int Alignment = 1 << Levels;
static const int MinimumImageSize = 4 << Levels;
static const int BlockSize = 32;
static const int IdwtInputRowsPerTile = 16;
static const int IdwtOutputRowsPerTile = 32;
static const int IdwtApron = 2;

// Wire format of what the streamer sends, all little endian.
//
// Decoder config (the role SPS/PPS play for H.264), sent via SetVideoConfigNals.
struct StreamConfig {
    uint32_t magic; // 'P' 'Y' 'R' 'W' = 0x57525950
    uint32_t version; // 2
    uint32_t width;
    uint32_t height;
    uint32_t chroma; // 0 = 4:2:0, 1 = 4:4:4
    uint32_t color; // 0 = BT.709 full range SDR
    uint32_t stripe_height; // luma rows per stripe, a multiple of 32
    uint32_t max_frame_bytes; // rate control limit, bounds the coded data of one frame
};
static const uint32_t StreamConfigMagic = 0x57525950;
static const uint32_t StreamConfigVersion = 2;
static_assert(sizeof(StreamConfig) == 32, "StreamConfig layout");

// Precedes the PyroWave data in every video packet. The PyroWave data that follows starts
// with a start-of-frame sequence header and then carries whole coded blocks, so each packet
// can be parsed on its own.
struct PacketPrefix {
    uint16_t stripe; // all blocks in this packet belong to this stripe
    uint16_t stripe_count;
    uint32_t packet_index; // 0 .. packet_count - 1, packets are sent in stripe order
    uint32_t packet_count;
    // Bytes of PyroWave data after this prefix. Anything after them is zero padding, which makes
    // the packets the same size, so the streamer can hand many to the network stack in one call.
    uint32_t data_bytes;
};
static_assert(sizeof(PacketPrefix) == 16, "PacketPrefix layout");

// PyroWave's BitstreamSequenceHeader (bitstream.md, "Start of frame header"), built with
// explicit shifts so it does not depend on how a compiler lays out bitfields.
inline void make_sequence_header(
    uint32_t out[2], int width, int height, bool chroma444, uint32_t sequence, uint32_t total_blocks
) {
    out[0]
        = uint32_t(width - 1) | (uint32_t(height - 1) << 14) | ((sequence & 7u) << 28) | (1u << 31);
    // code = 0 (start of frame); color fields 0 (BT.709, full range, as the encoder produces).
    out[1] = (total_blocks & 0xffffffu) | ((chroma444 ? 1u : 0u) << 26);
}

// The sequence counter of a coded block: bits 28..30 of its first word.
inline uint32_t block_sequence(const uint32_t* block_words) { return (block_words[0] >> 28) & 7u; }

// Index of the highest set bit; v must not be 0.
inline int highest_bit(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse64(&index, v);
    return int(index);
#else
    return 63 - __builtin_clzll(v);
#endif
}

struct Geometry {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    int stripe_height = 64;
    int aligned_width = 0;
    int aligned_height = 0;

    bool init(int width_, int height_, bool chroma444_, int stripe_height_) {
        if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384)
            return false;
        if (stripe_height_ <= 0 || stripe_height_ % IdwtOutputRowsPerTile != 0)
            return false;
        width = width_;
        height = height_;
        chroma444 = chroma444_;
        stripe_height = stripe_height_;
        aligned_width = std::max(align(width, Alignment), MinimumImageSize);
        aligned_height = std::max(align(height, Alignment), MinimumImageSize);
        return true;
    }

    static int align(int value, int alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }
    static int div_ceil(int a, int b) { return (a + b - 1) / b; }

    int level_width(int level) const { return std::max(1, (aligned_width / 2) >> level); }
    int level_height(int level) const { return std::max(1, (aligned_height / 2) >> level); }

    bool has_level(int component, int level) const {
        return !(level == 0 && component != 0 && !chroma444);
    }
    // The input level whose iDWT writes the component's output plane.
    int final_level(int component) const { return has_level(component, 0) ? 0 : 1; }
    int first_band(int level) const { return level == Levels - 1 ? 0 : 1; }

    int block_columns(int level) const { return div_ceil(level_width(level), BlockSize); }
    int block_rows(int level) const { return div_ceil(level_height(level), BlockSize); }
    int idwt_tile_rows(int level) const {
        return div_ceil(level_height(level), IdwtInputRowsPerTile);
    }
    int idwt_tile_columns(int level) const {
        return div_ceil(level_width(level), IdwtInputRowsPerTile);
    }

    // Rows of the output plane of `component` (luma rows for Y, chroma rows for Cb/Cr),
    // counted in the padded plane the final iDWT writes.
    int output_rows(int component) const { return 2 * level_height(final_level(component)); }

    int stripe_count() const { return div_ceil(aligned_height, stripe_height); }
    // Luma rows [0, rows) are final once stripes 0..stripe are decoded.
    int luma_rows_through_stripe(int stripe) const {
        return std::min((stripe + 1) * stripe_height, aligned_height);
    }

    // What producing the first `luma_rows` rows of the picture takes, per level of one
    // component: iDWT tile rows to run at each input level, and coefficient rows (of every
    // band of that level, LL included at the top level) that must be decoded first.
    // Levels the component does not have get 0.
    void requirements(int component, int luma_rows, int tiles[Levels], int coefficient_rows[Levels])
        const {
        const int f = final_level(component);
        for (int level = 0; level < Levels; level++)
            tiles[level] = coefficient_rows[level] = 0;

        // Rows of the final plane: 4:2:0 chroma planes have half as many rows as luma.
        int needed = f == 0 ? luma_rows : div_ceil(luma_rows, 2);
        needed = std::min(needed, output_rows(component));

        for (int level = f; level < Levels; level++) {
            // `needed` rows of the level's output (the next finer LL band, or the plane).
            int t = std::min(div_ceil(needed, IdwtOutputRowsPerTile), idwt_tile_rows(level));
            tiles[level] = t;
            coefficient_rows[level]
                = t == 0 ? 0 : std::min(t * IdwtInputRowsPerTile + IdwtApron, level_height(level));
            // The LL band of this level is the output of the next coarser level's iDWT.
            needed = coefficient_rows[level];
        }
    }

    int block_rows_needed(int component, int level, int luma_rows) const {
        int tiles[Levels], rows[Levels];
        requirements(component, luma_rows, tiles, rows);
        return div_ceil(rows[level], BlockSize);
    }

    // Calls fn(block_index, component, level, band, block_row, block_column) for every 32x32
    // block in PyroWave's block index order (bitstream.md, "Block index ordering").
    template <typename Fn> void for_each_block(Fn fn) const {
        int index = 0;
        for (int level = Levels - 1; level >= 0; level--)
            for (int component = 0; component < Components; component++) {
                if (!has_level(component, level))
                    continue;
                for (int band = first_band(level); band < Bands; band++)
                    for (int y = 0; y < block_rows(level); y++)
                        for (int x = 0; x < block_columns(level); x++)
                            fn(index++, component, level, band, y, x);
            }
    }

    int block_count() const {
        int count = 0;
        for_each_block([&](int, int, int, int, int, int) { count++; });
        return count;
    }

    // Index of the first block of a band, and blocks per row (the dequant shader's
    // block_offset_32x32 / block_stride_32x32).
    int band_block_offset(int component, int level, int band) const {
        int offset = -1;
        for_each_block([&](int index, int c, int l, int b, int y, int x) {
            if (offset < 0 && c == component && l == level && b == band && y == 0 && x == 0)
                offset = index;
        });
        return offset;
    }

    // For every block (by block index), the first stripe that needs it. The streamer sends
    // each block with that stripe.
    std::vector<uint16_t> block_stripes() const {
        const int stripes = stripe_count();
        // rows_needed[c][level][s]: block rows of (c, level) needed through stripe s.
        std::vector<int> rows_needed[Components][Levels];
        for (int c = 0; c < Components; c++)
            for (int level = 0; level < Levels; level++) {
                rows_needed[c][level].resize(stripes);
                for (int s = 0; s < stripes; s++)
                    rows_needed[c][level][s]
                        = block_rows_needed(c, level, luma_rows_through_stripe(s));
            }

        std::vector<uint16_t> result;
        for_each_block([&](int, int c, int level, int, int y, int) {
            const std::vector<int>& needed = rows_needed[c][level];
            // needed is non-decreasing in s and covers every row at the last stripe.
            int s = int(std::upper_bound(needed.begin(), needed.end(), y) - needed.begin());
            result.push_back(uint16_t(std::min(s, stripes - 1)));
        });
        return result;
    }

    // Block indices of every stripe, in block index order.
    std::vector<std::vector<uint32_t>> stripe_blocks() const {
        std::vector<std::vector<uint32_t>> result(stripe_count());
        std::vector<uint16_t> stripes = block_stripes();
        for (size_t i = 0; i < stripes.size(); i++)
            result[stripes[i]].push_back(uint32_t(i));
        return result;
    }
};

// What the PyroWave encoder reports per 32x32 block (pyrowave_encoder_get_mapped_raw_bitstream):
// where the coded block starts in the bitstream and how long it is, both in 32-bit words, the
// block header included. A length of 0 means the block is all zero and not coded.
struct BlockMeta {
    uint32_t offset_words;
    uint32_t num_words;
};

// How one encoded frame is cut into packets for the stripe pipeline, decided before any packet
// is written: every packet is a PacketPrefix, a start-of-frame sequence header and whole coded
// blocks of one stripe, at most `packet_bytes` long unless a single block is longer. Packets are
// in stripe order and never span stripes. Within a stripe the order of blocks is free (each
// carries its index), so they are packed to fill the packets: largest first, topped up with the
// smallest. Knowing the packet count up front lets the streamer write and send the packets in
// pieces (write_packets), so the first stripes are on the wire while the rest is still written.
struct PacketPlan {
    uint32_t sequence_header[2] = {};
    uint16_t stripe_count = 0;
    // Per packet: its stripe, its blocks (blocks[block_begin[p] .. block_begin[p + 1])) and the
    // PyroWave data they add up to, in 32-bit words.
    std::vector<uint16_t> stripe;
    std::vector<uint32_t> block_begin;
    std::vector<uint32_t> blocks;
    std::vector<uint32_t> data_words;

    size_t packet_count() const { return stripe.size(); }
    static size_t overhead() { return sizeof(PacketPrefix) + 2 * sizeof(uint32_t); }
    // Bytes packet p takes in `out`, padding included.
    size_t packet_size(size_t p, size_t packet_bytes, bool pad) const {
        const size_t size = overhead() + size_t(data_words[p]) * sizeof(uint32_t);
        return pad && size < packet_bytes ? packet_bytes : size;
    }
};

// Plans the packets of one frame into `plan` (its vectors keep their capacity between frames).
// Returns nullptr, or what is wrong with the input.
inline const char* plan_packets(
    const Geometry& g,
    const std::vector<std::vector<uint32_t>>& stripe_blocks,
    const uint32_t* words,
    size_t word_count,
    const BlockMeta* meta,
    size_t meta_count,
    size_t packet_bytes,
    PacketPlan& plan
) {
    plan.stripe.clear();
    plan.block_begin.clear();
    plan.blocks.clear();
    plan.data_words.clear();

    const size_t block_count = size_t(g.block_count());
    if (meta_count < block_count)
        return "fewer block entries than blocks";
    if (int(stripe_blocks.size()) != g.stripe_count())
        return "stripe table does not match the geometry";

    uint32_t coded_blocks = 0;
    uint32_t sequence = 0;
    bool have_sequence = false;
    for (size_t i = 0; i < block_count; i++) {
        if (meta[i].num_words == 0)
            continue;
        if (meta[i].num_words < 2 || size_t(meta[i].offset_words) + meta[i].num_words > word_count)
            return "a block lies outside the bitstream";
        if (!have_sequence) {
            sequence = block_sequence(words + meta[i].offset_words);
            have_sequence = true;
        }
        coded_blocks++;
    }

    make_sequence_header(
        plan.sequence_header, g.width, g.height, g.chroma444, sequence, coded_blocks
    );
    plan.stripe_count = uint16_t(g.stripe_count());

    uint32_t open_words = 0;
    auto open_packet = [&](uint16_t stripe) {
        plan.stripe.push_back(stripe);
        plan.block_begin.push_back(uint32_t(plan.blocks.size()));
        open_words = 0;
    };
    auto add_block = [&](uint32_t block) {
        plan.blocks.push_back(block);
        open_words += meta[block].num_words;
    };
    auto close_packet = [&]() { plan.data_words.push_back(open_words); };

    // Best fit packing within a stripe: each packet starts with the biggest block left and is
    // then filled with the biggest block that still fits, until none does. Blocks are bucketed by
    // size in words, with a bitmask of the non-empty buckets and a second one of its non-zero
    // words, so "biggest that fits" is a few bit scans whatever the packet size. (With one level,
    // 32 KB packets meant scanning ~120 empty words down to the small blocks, for every block.)
    const size_t capacity_words = packet_bytes > PacketPlan::overhead()
        ? (packet_bytes - PacketPlan::overhead()) / sizeof(uint32_t)
        : 0;
    // Kept between frames: the streamer packetizes every frame on the same thread.
    thread_local std::vector<std::vector<uint32_t>> tls_buckets;
    thread_local std::vector<uint64_t> tls_occupied;
    thread_local std::vector<uint64_t> tls_occupied_words;
    thread_local std::vector<uint32_t> tls_oversized;
    // Plain references for the loops below: with MSVC, every access to a thread_local with a
    // constructor goes through the TLS slot and an initialization check.
    std::vector<std::vector<uint32_t>>& buckets = tls_buckets;
    std::vector<uint64_t>& occupied = tls_occupied;
    std::vector<uint64_t>& occupied_words = tls_occupied_words;
    std::vector<uint32_t>& oversized = tls_oversized;
    if (buckets.size() < capacity_words + 1)
        buckets.resize(capacity_words + 1);
    occupied.assign((capacity_words + 64) / 64, 0);
    occupied_words.assign((occupied.size() + 63) / 64, 0);
    oversized.clear();

    // Bits 0..last of a 64-bit word.
    auto up_to = [](size_t last) {
        return last % 64 == 63 ? ~uint64_t(0) : ((uint64_t(1) << (last % 64 + 1)) - 1);
    };
    auto add_to_bucket = [&](size_t n, uint32_t block) {
        buckets[n].push_back(block);
        occupied[n / 64] |= uint64_t(1) << (n % 64);
        occupied_words[n / 4096] |= uint64_t(1) << (n / 64 % 64);
    };
    auto take_largest_fitting = [&](size_t max_words, uint32_t& block) {
        if (max_words > capacity_words)
            max_words = capacity_words;
        size_t w = max_words / 64;
        uint64_t bits = occupied[w] & up_to(max_words);
        if (bits == 0) {
            // The highest non-empty word below w.
            if (w == 0)
                return false;
            const size_t below = w - 1;
            bool found = false;
            for (size_t s = below / 64 + 1; s-- > 0;) {
                uint64_t words_bits = occupied_words[s];
                if (s == below / 64)
                    words_bits &= up_to(below);
                if (words_bits != 0) {
                    w = s * 64 + size_t(highest_bit(words_bits));
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
            bits = occupied[w];
        }
        const int top = highest_bit(bits);
        const size_t size = w * 64 + size_t(top);
        block = buckets[size].back();
        buckets[size].pop_back();
        if (buckets[size].empty()) {
            occupied[w] &= ~(uint64_t(1) << top);
            if (occupied[w] == 0)
                occupied_words[w / 64] &= ~(uint64_t(1) << (w % 64));
        }
        return true;
    };

    for (uint16_t stripe = 0; stripe < plan.stripe_count; stripe++) {
        size_t remaining = 0;
        // Pushed in reverse, so equal sized blocks come out in block order.
        const std::vector<uint32_t>& blocks = stripe_blocks[stripe];
        for (size_t k = blocks.size(); k-- > 0;) {
            const uint32_t block = blocks[k];
            const size_t n = meta[block].num_words;
            if (n == 0)
                continue;
            if (n > capacity_words) {
                oversized.push_back(block);
                continue;
            }
            add_to_bucket(n, block);
            remaining++;
        }

        // A block larger than a packet goes alone; the socket splits it into datagrams.
        for (size_t k = oversized.size(); k-- > 0;) {
            open_packet(stripe);
            add_block(oversized[k]);
            close_packet();
        }
        oversized.clear();

        while (remaining > 0) {
            open_packet(stripe);
            size_t free_words = capacity_words;
            uint32_t block;
            while (free_words > 0 && take_largest_fitting(free_words, block)) {
                add_block(block);
                free_words -= meta[block].num_words;
                remaining--;
            }
            // Packets never span stripes, so the client can tell when a stripe is complete.
            close_packet();
        }
    }

    // A frame with nothing coded still needs one packet, so the client finishes it.
    if (plan.stripe.empty()) {
        open_packet(uint16_t(plan.stripe_count - 1));
        close_packet();
    }
    plan.block_begin.push_back(uint32_t(plan.blocks.size()));
    return nullptr;
}

// Writes packets [first, end) of `plan` back to back into `out` and their sizes into `sizes`
// (both replaced). With `pad`, packets are zero padded to exactly `packet_bytes` (all but those
// holding one oversized block). `words` and `meta` are what the plan was made from.
inline void write_packets(
    const PacketPlan& plan,
    const uint32_t* words,
    const BlockMeta* meta,
    size_t packet_bytes,
    bool pad,
    size_t first,
    size_t end,
    std::vector<uint8_t>& out,
    std::vector<uint32_t>& sizes
) {
    sizes.clear();
    size_t total = 0;
    for (size_t p = first; p < end; p++) {
        sizes.push_back(uint32_t(plan.packet_size(p, packet_bytes, pad)));
        total += sizes.back();
    }
    // Only the padding needs zeros; everything else is overwritten below.
    out.resize(total);

    const uint32_t packet_count = uint32_t(plan.packet_count());
    uint8_t* dst = out.data();
    for (size_t p = first; p < end; p++) {
        const size_t data_bytes = size_t(plan.data_words[p]) * sizeof(uint32_t);
        PacketPrefix prefix = {};
        prefix.stripe = plan.stripe[p];
        prefix.stripe_count = plan.stripe_count;
        prefix.packet_index = uint32_t(p);
        prefix.packet_count = packet_count;
        prefix.data_bytes = uint32_t(sizeof(plan.sequence_header) + data_bytes);
        memcpy(dst, &prefix, sizeof(prefix));
        memcpy(dst + sizeof(prefix), plan.sequence_header, sizeof(plan.sequence_header));
        uint8_t* data = dst + PacketPlan::overhead();
        for (uint32_t k = plan.block_begin[p]; k < plan.block_begin[p + 1]; k++) {
            const BlockMeta& m = meta[plan.blocks[k]];
            const size_t bytes = size_t(m.num_words) * sizeof(uint32_t);
            memcpy(data, words + m.offset_words, bytes);
            data += bytes;
        }
        const size_t size = sizes[p - first];
        const size_t used = PacketPlan::overhead() + data_bytes;
        if (size > used)
            memset(dst + used, 0, size - used);
        dst += size;
    }
}

// plan_packets and write_packets for the whole frame at once.
inline const char* build_packets(
    const Geometry& g,
    const std::vector<std::vector<uint32_t>>& stripe_blocks,
    const uint32_t* words,
    size_t word_count,
    const BlockMeta* meta,
    size_t meta_count,
    size_t packet_bytes,
    bool pad,
    std::vector<uint8_t>& out,
    std::vector<uint32_t>& sizes
) {
    PacketPlan plan;
    const char* problem
        = plan_packets(g, stripe_blocks, words, word_count, meta, meta_count, packet_bytes, plan);
    if (problem) {
        out.clear();
        sizes.clear();
        return problem;
    }
    write_packets(plan, words, meta, packet_bytes, pad, 0, plan.packet_count(), out, sizes);
    return nullptr;
}

// What the client parses one frame's packets into: an offset per 32x32 block into a flat array
// of block words (header included), UINT32_MAX for blocks not received. This is the layout
// PyroWave's dequant shader reads.
struct ParseTarget {
    uint32_t* offsets = nullptr; // block_count entries, all UINT32_MAX at the start of a frame
    uint32_t* payload = nullptr;
    size_t payload_capacity_words = 0;
    size_t payload_words = 0;
    uint32_t sequence = UINT32_MAX; // of the frame, taken from its first packet
    int received_blocks = 0;
    int total_blocks = 0;
};

enum ParseResult { ParseOk = 0, ParseCorrupt, ParseOtherFrame, ParseOverflow };

// Parses the PyroWave data of one packet (after the PacketPrefix). Same rules as PyroWave's own
// parser, except that a different frame sequence means "not this frame" rather than "start
// over". Blocks parsed before an error are kept.
inline ParseResult parse_packet(
    const Geometry& g, int block_count, ParseTarget& target, const void* data_, size_t size
) {
    const uint8_t* data = static_cast<const uint8_t*>(data_);
    while (size >= 2 * sizeof(uint32_t)) {
        uint32_t words[2];
        memcpy(words, data, sizeof(words));
        const uint32_t sequence = (words[0] >> 28) & 7u;
        if (target.sequence == UINT32_MAX)
            target.sequence = sequence;
        else if (sequence != target.sequence)
            return ParseOtherFrame;

        if ((words[0] >> 31) != 0) {
            // Start-of-frame sequence header.
            const uint32_t width = (words[0] & 0x3fffu) + 1;
            const uint32_t height = ((words[0] >> 14) & 0x3fffu) + 1;
            const uint32_t code = (words[1] >> 24) & 3u;
            const bool chroma444 = ((words[1] >> 26) & 1u) != 0;
            if (code != 0 || width != uint32_t(g.width) || height != uint32_t(g.height)
                || chroma444 != g.chroma444)
                return ParseCorrupt;
            target.total_blocks = int(words[1] & 0xffffffu);
            data += sizeof(words);
            size -= sizeof(words);
            continue;
        }

        // Block header: ballot:16, payload_words:12, sequence:3, extended:1; quant:8,
        // block_index:24.
        const size_t block_words = (words[0] >> 16) & 0xfffu;
        const uint32_t block_index = words[1] >> 8;
        if (block_words < 2 || block_words * sizeof(uint32_t) > size
            || block_index >= uint32_t(block_count))
            return ParseCorrupt;

        // Duplicates are allowed by the bitstream (crude error correction) and ignored.
        if (target.offsets[block_index] == UINT32_MAX) {
            if (target.payload_words + block_words > target.payload_capacity_words)
                return ParseOverflow;
            memcpy(target.payload + target.payload_words, data, block_words * sizeof(uint32_t));
            target.offsets[block_index] = uint32_t(target.payload_words);
            target.payload_words += block_words;
            target.received_blocks++;
        }
        data += block_words * sizeof(uint32_t);
        size -= block_words * sizeof(uint32_t);
    }
    return size == 0 ? ParseOk : ParseCorrupt;
}

// The client side of the stripe pipeline's bookkeeping: given which packets of a frame have
// arrived, how many stripes are complete. Packets are sent in stripe order, so every stripe
// before the one of the last packet of the contiguous received prefix is complete.
inline int complete_stripes(
    const std::vector<bool>& received,
    const std::vector<uint16_t>& stripe_of_packet,
    int stripe_count
) {
    size_t contiguous = 0;
    while (contiguous < received.size() && received[contiguous])
        contiguous++;
    if (contiguous == received.size())
        return stripe_count;
    return contiguous == 0 ? 0 : stripe_of_packet[contiguous - 1];
}
}

#endif
