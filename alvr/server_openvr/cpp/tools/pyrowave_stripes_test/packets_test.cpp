// End to end check of the stripe packet path with synthetic encoder output:
// streamer build_packets -> packets -> client parse_packet (and PyroWave's own parser),
// plus the client's "stripes complete" bookkeeping under packet loss.
#include "PyroWaveStripes.h"
#include "build_packets_reference.h"
#include "metal/pyrowave_bitstream.hpp"
#include <cstdio>
#include <random>

using namespace PyroWaveStripes;

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; if (failures < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void run(int width, int height, bool c444, int stripe_height, size_t packet_bytes, bool pad, uint32_t seed)
{
	std::mt19937 rng(seed);
	Geometry g;
	g.init(width, height, c444, stripe_height);
	const int blocks = g.block_count();
	auto stripe_blocks = g.stripe_blocks();
	auto block_stripe = g.block_stripes();
	const uint32_t sequence = rng() % 8;

	// Synthetic encoder output: blocks stored in a shuffled order, ~20% not coded, sizes from
	// tiny to larger than a packet.
	std::vector<uint32_t> words;
	std::vector<BlockMeta> meta(blocks);
	std::vector<int> order(blocks);
	for (int i = 0; i < blocks; i++) order[i] = i;
	std::shuffle(order.begin(), order.end(), rng);
	int coded = 0;
	for (int i : order)
	{
		if (rng() % 5 == 0) { meta[i] = { uint32_t(rng() % 1000), 0 }; continue; }
		uint32_t n = 2 + (rng() % 50 == 0 ? rng() % 1200 : rng() % 200);
		meta[i] = { uint32_t(words.size()), n };
		words.push_back((rng() & 0xffff) | (n << 16) | (sequence << 28));
		words.push_back((rng() & 0xff) | (uint32_t(i) << 8));
		for (uint32_t w = 2; w < n; w++) words.push_back(rng());
		coded++;
	}

	std::vector<uint8_t> out;
	std::vector<uint32_t> sizes;
	const char *problem = build_packets(g, stripe_blocks, words.data(), words.size(), meta.data(), meta.size(), packet_bytes, pad, out, sizes);
	EXPECT(!problem, "build_packets: %s", problem ? problem : "");
	if (problem) return;

	size_t total = 0;
	for (uint32_t s : sizes) total += s;
	EXPECT(total == out.size(), "sizes add up");

	// Same bytes as the packer before the plan/write split ...
	{
		std::vector<uint8_t> ref_out;
		std::vector<uint32_t> ref_sizes;
		const char *ref_problem = build_packets_reference(g, stripe_blocks, words.data(), words.size(), meta.data(), meta.size(), packet_bytes, pad, ref_out, ref_sizes);
		EXPECT(!ref_problem && ref_out == out && ref_sizes == sizes, "differs from the reference packer");
	}
	// ... and the same when written in pieces, as the streamer sends them.
	{
		PacketPlan plan;
		EXPECT(!plan_packets(g, stripe_blocks, words.data(), words.size(), meta.data(), meta.size(), packet_bytes, plan), "plan");
		EXPECT(plan.packet_count() == sizes.size(), "plan packet count");
		std::vector<uint8_t> joined, piece;
		std::vector<uint32_t> joined_sizes, piece_sizes;
		for (size_t first = 0; first < plan.packet_count();)
		{
			const size_t end = std::min(plan.packet_count(), first + 1 + rng() % 9);
			write_packets(plan, words.data(), meta.data(), packet_bytes, pad, first, end, piece, piece_sizes);
			size_t piece_total = 0;
			for (size_t p = first; p < end; p++) piece_total += plan.packet_size(p, packet_bytes, pad);
			EXPECT(piece.size() == piece_total, "packet_size matches what is written");
			joined.insert(joined.end(), piece.begin(), piece.end());
			joined_sizes.insert(joined_sizes.end(), piece_sizes.begin(), piece_sizes.end());
			first = end;
		}
		EXPECT(joined == out && joined_sizes == sizes, "pieces differ from the whole frame");
	}

	PyroWave::BlockLayout layout;
	layout.init(width, height, c444 ? PyroWave::ChromaSubsampling::Chroma444 : PyroWave::ChromaSubsampling::Chroma420);
	PyroWave::BitstreamParser upstream_all;
	upstream_all.init(&layout);

	std::vector<uint32_t> offsets(blocks, UINT32_MAX);
	std::vector<uint32_t> payload(words.size() + 16);
	ParseTarget target;
	target.offsets = offsets.data();
	target.payload = payload.data();
	target.payload_capacity_words = payload.size();

	std::vector<uint16_t> stripe_of_packet(sizes.size());
	std::vector<int> packet_of_block(blocks, -1);
	size_t pos = 0;
	size_t data_bytes_total = 0;
	int oversized = 0;
	int prev_stripe = 0;
	for (size_t p = 0; p < sizes.size(); p++)
	{
		const uint8_t *packet = out.data() + pos;
		PacketPrefix prefix;
		memcpy(&prefix, packet, sizeof(prefix));
		EXPECT(prefix.packet_index == p && prefix.packet_count == sizes.size() && prefix.stripe_count == g.stripe_count(), "prefix fields");
		EXPECT(prefix.stripe >= prev_stripe, "stripe order");
		prev_stripe = prefix.stripe;
		stripe_of_packet[p] = prefix.stripe;

		const uint8_t *data = packet + sizeof(prefix);
		const size_t size = prefix.data_bytes;
		EXPECT(sizeof(prefix) + size <= sizes[p], "data_bytes within the packet");
		// Padding: zeros, and the packet then is exactly the datagram size.
		for (size_t b = sizeof(prefix) + size; b < sizes[p]; b++)
			EXPECT(packet[b] == 0, "padding is zero");
		if (pad)
			EXPECT(sizes[p] == packet_bytes || sizes[p] == sizeof(prefix) + size, "padded size");
		else
			EXPECT(sizes[p] == sizeof(prefix) + size, "no padding without pad");
		data_bytes_total += sizeof(prefix) + size;

		// Each packet parses on its own with PyroWave's parser ...
		PyroWave::BitstreamParser upstream_one;
		upstream_one.init(&layout);
		EXPECT(upstream_one.push_packet(data, size), "packet %zu does not parse on its own", p);
		EXPECT(upstream_one.get_total_blocks_in_sequence() == coded, "frame header total blocks");
		EXPECT(upstream_all.push_packet(data, size), "packet %zu in sequence", p);

		// ... and with the client's.
		int before = target.received_blocks;
		EXPECT(parse_packet(g, blocks, target, data, size) == ParseOk, "client parse of packet %zu", p);
		int in_packet = target.received_blocks - before;
		EXPECT(in_packet == upstream_one.get_decoded_blocks(), "block count per packet");
		// Size limit: only a packet with a single block may exceed it.
		EXPECT(sizes[p] <= packet_bytes || in_packet == 1, "packet %zu is %u bytes with %d blocks", p, sizes[p], in_packet);
		if (pad && sizes[p] > packet_bytes)
			oversized++;

		for (int i = 0; i < blocks; i++)
			if (upstream_one.dequant_offsets()[i] != UINT32_MAX)
			{
				EXPECT(packet_of_block[i] == -1, "block %d sent twice", i);
				packet_of_block[i] = int(p);
				EXPECT(block_stripe[i] == prefix.stripe, "block %d of stripe %d sent in stripe %d", i, block_stripe[i], prefix.stripe);
			}
		pos += sizes[p];
	}

	EXPECT(target.received_blocks == coded && target.total_blocks == coded && target.sequence == sequence, "client parser totals");
	EXPECT(upstream_all.decode_is_ready(false), "upstream parser complete");
	for (int i = 0; i < blocks; i++)
	{
		if (meta[i].num_words == 0) { EXPECT(offsets[i] == UINT32_MAX && packet_of_block[i] == -1, "uncoded block %d", i); continue; }
		EXPECT(offsets[i] != UINT32_MAX && memcmp(&payload[offsets[i]], &words[meta[i].offset_words], meta[i].num_words * 4) == 0, "block %d data", i);
	}

	// Loss: whatever complete_stripes() reports, every coded block of those stripes arrived.
	for (int trial = 0; trial < 20; trial++)
	{
		std::vector<bool> received(sizes.size(), false);
		const int loss_per_mille = trial == 0 ? 0 : int(rng() % 50);
		int last_complete = 0;
		for (size_t p = 0; p < sizes.size(); p++)
		{
			received[p] = int(rng() % 1000) >= loss_per_mille;
			int complete = complete_stripes(received, stripe_of_packet, g.stripe_count());
			EXPECT(complete >= last_complete, "complete stripes never go back");
			last_complete = complete;
			for (int i = 0; i < blocks; i++)
				if (packet_of_block[i] >= 0 && block_stripe[i] < complete)
					EXPECT(received[packet_of_block[i]], "stripe %d reported complete but block %d missing", block_stripe[i], i);
		}
		if (loss_per_mille == 0) EXPECT(last_complete == g.stripe_count(), "lossless frame completes");
	}

	// Parser robustness: another frame's sequence, truncation, garbage.
	{
		ParseTarget t2 = target;
		std::vector<uint32_t> o2(blocks, UINT32_MAX);
		t2.offsets = o2.data(); t2.payload_words = 0; t2.received_blocks = 0;
		t2.sequence = (sequence + 1) % 8;
		PacketPrefix first;
		memcpy(&first, out.data(), sizeof(first));
		EXPECT(parse_packet(g, blocks, t2, out.data() + sizeof(PacketPrefix), first.data_bytes) == ParseOtherFrame, "other frame");
		t2.sequence = UINT32_MAX;
		EXPECT(parse_packet(g, blocks, t2, out.data() + sizeof(PacketPrefix), first.data_bytes - 3) == ParseCorrupt, "truncated");
		std::vector<uint8_t> junk(64);
		for (auto &b : junk) b = uint8_t(rng());
		ParseTarget t3 = t2; t3.sequence = UINT32_MAX;
		(void)parse_packet(g, blocks, t3, junk.data(), junk.size()); // must not crash
		t2.payload_capacity_words = 4;
		t2.sequence = UINT32_MAX;
		std::fill(o2.begin(), o2.end(), UINT32_MAX);
		ParseResult r = parse_packet(g, blocks, t2, out.data() + sizeof(PacketPrefix), first.data_bytes);
		EXPECT(r == ParseOverflow || r == ParseOk, "overflow detected");
	}

	printf("%5dx%-5d %s stripe %3d packet %4zu%s: %6zu packets, %8zu bytes, %5d coded of %5d blocks, %.2f%% padding, %d oversized\n",
	       width, height, c444 ? "444" : "420", stripe_height, packet_bytes, pad ? " padded" : "       ", sizes.size(), out.size(),
	       coded, blocks, 100.0 * (out.size() - data_bytes_total) / out.size(), oversized);
}

int main()
{
	uint32_t seed = 1;
	for (auto s : std::vector<std::pair<int, int>>{ { 4288, 1664 }, { 1920, 1080 }, { 1000, 700 }, { 130, 130 } })
		for (bool c444 : { false, true })
			for (int sh : { 32, 64, 128 })
				for (bool pad : { false, true })
					run(s.first, s.second, c444, sh, 1400 - 18 - 13, pad, seed++);
	run(4288, 1664, false, 64, 8000, true, seed++);
	run(4864, 1728, false, 64, 32737, false, seed++);
	run(4864, 1728, false, 64, 9216 - 18 - 13, true, seed++);
	run(4288, 1664, false, 64, 300, true, seed++);

	// An all-zero frame still yields one packet.
	{
		Geometry g; g.init(256, 256, false, 64);
		std::vector<BlockMeta> meta(g.block_count(), BlockMeta{ 0, 0 });
		std::vector<uint8_t> out; std::vector<uint32_t> sizes;
		const char *p = build_packets(g, g.stripe_blocks(), nullptr, 0, meta.data(), meta.size(), 1369, false, out, sizes);
		EXPECT(!p && sizes.size() == 1 && sizes[0] == sizeof(PacketPrefix) + 8, "empty frame");
		std::vector<uint8_t> ref_out; std::vector<uint32_t> ref_sizes;
		build_packets_reference(g, g.stripe_blocks(), nullptr, 0, meta.data(), meta.size(), 1369, false, ref_out, ref_sizes);
		EXPECT(ref_out == out && ref_sizes == sizes, "empty frame differs from the reference packer");
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
