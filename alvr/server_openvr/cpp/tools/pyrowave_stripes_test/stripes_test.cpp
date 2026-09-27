// Checks PyroWaveStripes against PyroWave's own BlockLayout and against a model of what the
// iDWT tiles read, for every intermediate decode step a client can take.
#include "PyroWaveStripes.h"
#include "metal/pyrowave_bitstream.hpp"
#include <cstdio>
#include <cstdlib>

using namespace PyroWaveStripes;

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; if (failures < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// Highest coefficient row + 1 that iDWT tile rows [0, tiles) read at a level of `res` rows,
// following generate_mirror_uv(): reads [16t - 2, 16t + 18) mirrored into [0, res).
static int rows_read(int tiles, int res)
{
	int highest = 0;
	for (int t = 0; t < tiles; t++)
		for (int r = 16 * t - 2; r < 16 * t + 18; r++)
		{
			int m = r;
			if (m < 0) m = -m;             // mirrored at the top
			if (m >= res) m = 2 * res - 2 - m; // mirrored at the bottom
			if (m < 0) m = 0;
			highest = std::max(highest, m + 1);
		}
	return highest;
}

static void check(int width, int height, bool chroma444, int stripe_height)
{
	Geometry g;
	if (!g.init(width, height, chroma444, stripe_height)) { EXPECT(false, "init %dx%d", width, height); return; }

	PyroWave::BlockLayout layout;
	layout.init(width, height, chroma444 ? PyroWave::ChromaSubsampling::Chroma444 : PyroWave::ChromaSubsampling::Chroma420);
	EXPECT(g.block_count() == layout.block_count_32x32, "block count %d vs %d", g.block_count(), layout.block_count_32x32);
	for (int c = 0; c < Components; c++)
		for (int l = 0; l < Levels; l++)
		{
			if (!g.has_level(c, l)) continue;
			EXPECT(g.level_width(l) == layout.level_width(l) && g.level_height(l) == layout.level_height(l), "level size");
			for (int b = g.first_band(l); b < Bands; b++)
			{
				EXPECT(g.band_block_offset(c, l, b) == layout.block_meta[c][l][b].block_offset_32x32, "offset c%d l%d b%d", c, l, b);
				EXPECT(g.block_columns(l) == layout.block_meta[c][l][b].block_stride_32x32, "stride");
			}
		}

	const int S = g.stripe_count();
	std::vector<uint16_t> stripes = g.block_stripes();
	EXPECT((int)stripes.size() == g.block_count(), "stripe table size");

	// Per stripe block counts, for the report.
	std::vector<int> per_stripe(S, 0);
	for (uint16_t s : stripes) { EXPECT(s < S, "stripe out of range"); per_stripe[s]++; }

	int prev_tiles[Components][Levels] = {}, prev_rows[Components][Levels] = {};
	for (int k = 1; k <= S; k++)
	{
		const int luma_rows = g.luma_rows_through_stripe(k - 1);
		for (int c = 0; c < Components; c++)
		{
			int tiles[Levels], rows[Levels];
			g.requirements(c, luma_rows, tiles, rows);
			const int f = g.final_level(c);

			for (int l = 0; l < Levels; l++)
			{
				EXPECT(tiles[l] >= prev_tiles[c][l] && rows[l] >= prev_rows[c][l], "monotonic");
				prev_tiles[c][l] = tiles[l]; prev_rows[c][l] = rows[l];
				if (!g.has_level(c, l)) { EXPECT(tiles[l] == 0 && rows[l] == 0, "absent level"); continue; }

				// a) every block the decoder dequantizes now belongs to a complete stripe.
				int block_rows = Geometry::div_ceil(rows[l], BlockSize);
				int avail_coeff = std::min(block_rows * BlockSize, g.level_height(l));
				g.for_each_block([&](int index, int bc, int bl, int, int y, int) {
					if (bc == c && bl == l && y < block_rows)
						EXPECT(stripes[index] < k, "block %d of stripe %d needed at step %d", index, stripes[index], k);
				});

				// b) the tiles run now only read coefficient rows that are decoded ...
				EXPECT(rows_read(tiles[l], g.level_height(l)) <= avail_coeff,
				       "%dx%d c%d l%d step %d: tiles read %d rows, %d decoded", width, height, c, l, k,
				       rows_read(tiles[l], g.level_height(l)), avail_coeff);
				// ... and LL rows the coarser level has produced.
				if (l < Levels - 1)
				{
					int avail_ll = std::min(tiles[l + 1] * IdwtOutputRowsPerTile, g.level_height(l));
					EXPECT(rows_read(tiles[l], g.level_height(l)) <= avail_ll,
					       "%dx%d c%d l%d step %d: LL rows read %d, produced %d", width, height, c, l, k,
					       rows_read(tiles[l], g.level_height(l)), avail_ll);
				}
			}

			// c) the output rows promised for this step are written.
			int out_rows = std::min(tiles[f] * IdwtOutputRowsPerTile, g.output_rows(c));
			int promised = f == 0 ? luma_rows : Geometry::div_ceil(luma_rows, 2);
			EXPECT(out_rows >= std::min(promised, g.output_rows(c)), "c%d step %d: %d rows out, %d promised", c, k, out_rows, promised);

			// d) the last step decodes everything.
			if (k == S)
				for (int l = 0; l < Levels; l++)
					if (g.has_level(c, l))
					{
						EXPECT(tiles[l] == g.idwt_tile_rows(l), "final tiles c%d l%d", c, l);
						EXPECT(Geometry::div_ceil(rows[l], BlockSize) == g.block_rows(l), "final rows c%d l%d", c, l);
					}
		}
	}

	printf("%5dx%-5d %s stripe %3d: %2d stripes, %5d blocks; blocks per stripe:", width, height,
	       chroma444 ? "444" : "420", stripe_height, S, g.block_count());
	for (int s = 0; s < S && s < 12; s++) printf(" %d", per_stripe[s]);
	printf(S > 12 ? " ...\n" : "\n");
}

int main()
{
	int sizes[][2] = { { 4288, 1664 }, { 3216, 1248 }, { 2144, 832 }, { 1920, 1080 }, { 1000, 700 }, { 130, 130 }, { 128, 128 }, { 16384, 64 } };
	for (auto &s : sizes)
		for (bool c444 : { false, true })
			for (int sh : { 32, 64, 128, 256 })
				check(s[0], s[1], c444, sh);

	// The sequence header matches PyroWave's bitfield struct.
	uint32_t words[2];
	make_sequence_header(words, 4288, 1664, false, 5, 12345);
	PyroWave::BitstreamSequenceHeader h;
	memcpy(&h, words, sizeof(h));
	EXPECT(h.width_minus_1 == 4287 && h.height_minus_1 == 1663 && h.sequence == 5 && h.extended == 1 &&
	       h.total_blocks == 12345 && h.code == 0 && h.chroma_resolution == 0, "sequence header 420");
	make_sequence_header(words, 17, 9, true, 7, 1);
	memcpy(&h, words, sizeof(h));
	EXPECT(h.width_minus_1 == 16 && h.height_minus_1 == 8 && h.sequence == 7 && h.chroma_resolution == 1 && h.total_blocks == 1, "sequence header 444");
	PyroWave::BitstreamHeader bh = {};
	bh.sequence = 6; bh.extended = 0; bh.payload_words = 100;
	EXPECT(block_sequence(reinterpret_cast<const uint32_t *>(&bh)) == 6, "block sequence");

	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
