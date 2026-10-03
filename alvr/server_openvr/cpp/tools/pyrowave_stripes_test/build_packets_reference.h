// The packer as it was before plan_packets/write_packets (streamer commit 4d735407), kept
// only to check that the split one produces the same bytes.
#include "PyroWaveStripes.h"

namespace PyroWaveStripes {
inline const char* build_packets_reference(
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
    out.clear();
    sizes.clear();

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

    uint32_t sequence_header[2];
    make_sequence_header(sequence_header, g.width, g.height, g.chroma444, sequence, coded_blocks);

    std::vector<size_t> starts;
    const uint16_t stripe_count = uint16_t(g.stripe_count());
    const size_t packet_overhead = sizeof(PacketPrefix) + sizeof(sequence_header);
    size_t start = 0;

    auto append = [&](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    // Every packet carries the frame header, so each one can be parsed on its own.
    auto open_packet = [&](uint16_t stripe) {
        start = out.size();
        starts.push_back(start);
        PacketPrefix prefix = {};
        prefix.stripe = stripe;
        prefix.stripe_count = stripe_count;
        append(&prefix, sizeof(prefix));
        append(sequence_header, sizeof(sequence_header));
    };
    auto close_packet = [&]() {
        const size_t size = out.size() - start;
        PacketPrefix prefix;
        memcpy(&prefix, &out[start], sizeof(prefix));
        prefix.data_bytes = uint32_t(size - sizeof(prefix));
        memcpy(&out[start], &prefix, sizeof(prefix));
        if (pad && size < packet_bytes)
            out.resize(start + packet_bytes, 0);
        sizes.push_back(uint32_t(out.size() - start));
    };

    // Best fit packing within a stripe: each packet starts with the biggest block left and is
    // then filled with the biggest block that still fits, until none does. Blocks are bucketed by
    // size in words, with a bitmask of the non-empty buckets and a second one of its non-zero
    // words, so "biggest that fits" is a few bit scans whatever the packet size. (With one level,
    // 32 KB packets meant scanning ~120 empty words down to the small blocks, for every block.)
    const size_t capacity_words
        = packet_bytes > packet_overhead ? (packet_bytes - packet_overhead) / sizeof(uint32_t) : 0;
    // Kept between frames: the streamer packetizes every frame on the same thread.
    thread_local std::vector<std::vector<uint32_t>> ref_tls_buckets;
    thread_local std::vector<uint64_t> ref_tls_occupied;
    thread_local std::vector<uint64_t> ref_tls_occupied_words;
    thread_local std::vector<uint32_t> ref_tls_oversized;
    // Plain references for the loops below: with MSVC, every access to a thread_local with a
    // constructor goes through the TLS slot and an initialization check.
    std::vector<std::vector<uint32_t>>& buckets = ref_tls_buckets;
    std::vector<uint64_t>& occupied = ref_tls_occupied;
    std::vector<uint64_t>& occupied_words = ref_tls_occupied_words;
    std::vector<uint32_t>& oversized = ref_tls_oversized;
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

    for (uint16_t stripe = 0; stripe < stripe_count; stripe++) {
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
            const uint32_t block = oversized[k];
            open_packet(stripe);
            append(words + meta[block].offset_words, meta[block].num_words * sizeof(uint32_t));
            close_packet();
        }
        oversized.clear();

        while (remaining > 0) {
            open_packet(stripe);
            size_t free_words = capacity_words;
            uint32_t block;
            while (free_words > 0 && take_largest_fitting(free_words, block)) {
                append(words + meta[block].offset_words, meta[block].num_words * sizeof(uint32_t));
                free_words -= meta[block].num_words;
                remaining--;
            }
            // Packets never span stripes, so the client can tell when a stripe is complete.
            close_packet();
        }
    }

    // A frame with nothing coded still needs one packet, so the client finishes it.
    if (sizes.empty()) {
        open_packet(uint16_t(stripe_count - 1));
        close_packet();
    }

    const uint32_t packet_count = uint32_t(sizes.size());
    for (uint32_t i = 0; i < packet_count; i++) {
        PacketPrefix prefix;
        memcpy(&prefix, &out[starts[i]], sizeof(prefix));
        prefix.packet_index = i;
        prefix.packet_count = packet_count;
        memcpy(&out[starts[i]], &prefix, sizeof(prefix));
    }
    return nullptr;
}

} // namespace PyroWaveStripes
