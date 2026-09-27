# PyroWave streaming for ALVR on Apple Vision Pro — project overview

This file is identical in all three repositories of the project, so that whoever lands in
one of them sees the whole picture first.

## What this is

[ALVR](https://github.com/alvr-org/ALVR) streams VR from a Windows PC to a headset. This
project adds [PyroWave](https://github.com/Themaister/pyrowave) as a video codec for the
Apple Vision Pro client on a 4 Gbps wired LAN. PyroWave is an intra-only wavelet codec that
runs entirely on the GPU (Vulkan compute on the PC, Metal on the headset). It trades
bandwidth for latency: encode and decode take well under a millisecond, at bitrates in the
gigabits per second.

Target format: 4288x1664, 90 fps, 4:2:0, default 3 bits per pixel (2.7 MB per frame,
about 1.9 Gbps).

## The three repositories

| Repository | Forked from | What it holds |
|---|---|---|
| `ALVR-PyroWave-private` (streamer) | `ALVR-JPEGXS-private` at `a9f6542` (ALVR 20.14.1) | Windows streamer: the encoder, settings, codec negotiation |
| `alvr-visionos-pyrowave-private` (client app) | `alvr-visionos-private` at `fb2576a` | visionOS app: vendored PyroWave Metal decoder, glue to the renderer |
| `alvr-client-core-pyrowave-private` (client core) | `e3fd448`, the submodule commit of `fb2576a` | ALVR's Rust client core, the `ALVR` submodule of the app |

Streamer and client core are both ALVR 20.14.1, so the protocol matches. The `session` and
`packets` crates carry byte-identical changes in both, because the settings and
capabilities are serialized between the two sides.

The streamer and the app must use PyroWave from the same upstream commit (the bitstream is
still a draft upstream): `89f7e47d4abbf650c91fae766728af866c5e32a0`.

## Release pyrowave-2026.09.27 (internal)

First internal release. The three repositories belong together at these commits (tag
`pyrowave-2026.09.27` in each): streamer and client core as of this overview, the app with the
client core as its `ALVR` submodule. All of it is ALVR 20.14.1 underneath.

Tested on an RTX 5090 streamer and an Apple Vision Pro over the 4 Gbps wired link, runs 1-13 of
2026-09-26/27, at 4288x1664, 90 fps, 4 bpp, 32 KB packets:

| | light load (SteamVR home) | game load |
|---|---|---|
| total latency p50 | 40-42 ms | 47-51 ms |
| encoding (streamer's own part) | 3.5-4 ms | 3.5-4 ms + waiting for the game's GPU work |
| network (transfer of ~3.5 MB) | 6.3 ms | 6.3 ms |
| frame buffering p50 | ~1 ms | 1-3.5 ms |
| packets lost / frames dropped | 0 | 0 |

Defaults that matter (all settings are in the dashboard or the app's Advanced Settings):
- Streamer: preferred codec PyroWave; "Phase-lock frame pacing to headset" on; "aim frames at
  the headset pickup" off (experimental, and it only moves waiting from frame buffering into game
  time); UDP send segmentation on.
- App: Single Frame Buffer, Late Frame Pickup and Tracking Send Phase on.

Known limits:
- visionOS sometimes runs the display at 100 Hz under the 90 fps stream (seen in the morning runs
  of 2026-09-27, not in the last six runs). The app notices and logs it ("Display rate: ...") and
  asks for 90 Hz again, which so far did not help; frames then drift through the display cycle and
  ~10% of display frames show no new video frame. Workaround: stream at 100 fps.
- Tracking Send Phase still retreats from its best point in calm phases (it counts frames landing
  within 0.25 ms of the pickup as misses), and a big step during a load change can make a few
  percent of frames miss the pickup for one window. Next change: a smaller margin.
- Under game load a large part of the remaining spread is decode (p50 ~1, p95 ~4 ms).
- No HDR, no Linux streamer.
- Diagnostics stay on: the streamer logs its timing split, the D3D11 GPU work and latency
  percentiles every 5 s to session_log.txt; the app writes Documents/pyrowave_debug.log (32 MB
  cap).

## Data path

PyroWave upstream decodes a frame only once all of it has arrived. This project adds stripe
pipelining on both ends, so the headset decodes a frame while the rest of it is still on the
wire:

```
SteamVR frame (D3D11, RGBA8)
  -> FrameRender (composition, color correction, foveated encoding)
  -> VideoEncoderPyroWave: CopyResource into a texture shared with Vulkan (NT handle),
     signal a shared D3D11 fence (odd values)
  -> PyroWave Vulkan: waits on the fence, RGB -> YCbCr BT.709 full range, DWT, rate control,
     packing; signals the fence (even values); D3D11 waits on it before the next copy
  -> build_packets (PyroWaveStripes.h): the coded 32x32 blocks, re-ordered by stripe, cut into
     packets of one UDP datagram each; every packet = prefix + frame header + whole blocks
  -> one ALVR video packet per PyroWave packet, all with the frame's timestamp, is_idr = true
  -> stream socket (Windows): ~46 datagrams per send call via UDP segmentation offload; the
     packets are padded to full size so consecutive ones can share a call
  -> client core reads each datagram with one syscall and hands every packet to the app
  -> PyroWaveDecoder.swift, network thread: parse the packet straight into the frame's GPU
     buffers; when the received packets complete more stripes, queue a decode step
  -> worker queue: pyrowave_frame_decode dequantizes the newly complete block rows and runs
     the iDWT, level by level, on exactly the tile rows that are now computable; luma goes
     straight into a 10-bit biplanar CVPixelBuffer, chroma is interleaved stripe by stripe
  -> after the last packet only the last stripe is left; the finished frame takes the
     existing frame queue, reprojection and renderer, like a VideoToolbox frame
```

### Stripes

The picture is cut into stripes of `stripe_height` luma rows (default 64: 26 stripes at
4288x1664). One tile of the iDWT at level L turns 16 coefficient rows of each band into 32 rows
of the next finer level and reads 2 more rows on each side. From that, `PyroWaveStripes.h`
computes, for the first N rows of the picture, how many coefficient rows every level needs, and
assigns each block to the first stripe that needs it. Stripe 0 carries the coarse levels (about
11% of the blocks at 4288x1664); later stripes 3-5% each. The same header, byte for byte, is
`pyrowave_stripes.hpp` in the client.

### Wire format

- Codec: `CodecType::PyroWave = 3` (`ALVR_CODEC_PYROWAVE` in the streamer's C++,
  `ALVR_CODEC_PYRO_WAVE` in the client's C header). The streamer only picks it when the
  client sets the capability `encoder_pyrowave`; otherwise it falls back to HEVC.
- Decoder config (what SPS/PPS are for H.264): `PyroWaveStripes::StreamConfig`, 32 bytes little
  endian: magic `PYRW` (0x57525950), version 2, width, height, chroma (0 = 4:2:0,
  1 = 4:4:4), color (0 = BT.709 full range SDR), stripe height, frame size limit. Sent before
  the first frame and again with every IDR the streamer's scheduler asks for.
- Video packets: `PyroWaveStripes::PacketPrefix` (stripe, stripe count, packet index, packet
  count, data bytes; 16 bytes), then a PyroWave start-of-frame header, then whole coded blocks
  of that stripe, then zero padding up to the full packet size. Packets are in stripe order and
  never span stripes; within a stripe the blocks are packed best fit (largest first, then the
  largest that still fits), which leaves about 0.5% padding. The packet size is Connection >
  Packet size minus the socket's shard prefix and the video header, so each is one datagram
  (a single coded block larger than that is still one packet, which the socket splits).
- Every frame is intra coded and reported as an IDR. A lost packet stops the stripe pipeline at
  that point; the frame is finished when its last packet or the next frame arrives, with the
  missing blocks decoded as zero (blurred). Frames missing more than 10% of their packets are
  dropped.

## Building

### Streamer (Windows)

`build_windows.ps1` in the streamer repository does all of it, by default under `C:\Temp`:

```
powershell -ExecutionPolicy Bypass -File build_windows.ps1
```

It clones or updates the streamer and PyroWave (at the pinned commit), builds PyroWave's DLL
with Visual Studio 2022, fills `deps\windows\pyrowave` (header, Vulkan headers, DLL, see
`deps/windows/pyrowave/README.md`) and runs `cargo xtask build-streamer --release --keep-config`.
Needs Git for Windows, Visual Studio 2022 with C++, CMake 3.27+ and Rust. On a machine that
never built ALVR add `-PrepareDeps` (installs LLVM for bindgen via Chocolatey; it wipes
`deps\windows`, so the script runs it before filling the PyroWave folder).

The build log must contain `Building with the PyroWave encoder`; the streamer is in
`C:\Temp\ALVR-PyroWave-private\build\alvr_streamer_windows`. In the dashboard: Video >
Preferred codec > PyroWave; Video > Encoder > PyroWave for bits per pixel and stripe height.
HDR must be off.

### Client (macOS, for Apple Vision Pro)

`deploy_pyrowave.sh` in the app repository, in a GUI Terminal window (code signing needs the
unlocked login keychain), like `deploy_test.sh` for JPEG XS:

```
bash deploy_pyrowave.sh
```

It clones or updates the app to `~/dev/alvr-visionos-pyrowave` with its `ALVR` submodule (the
client core repository), rebuilds the client core framework with `build_and_repack.sh`
(`SKIP_CORE=1` reuses the last one), checks that its header knows `ALVR_CODEC_PYRO_WAVE`, builds
the app for visionOS, installs and launches it. DEVICE_UDID and BUNDLE_ID default to the ones
of `deploy_test.sh`.

## What has been checked, and what has not

Checked in the environment the port was written in (Linux, no GPU, no Apple toolchain):

- Rust: `cargo check` of `alvr_session`, `alvr_packets`, `alvr_server_core`,
  `alvr_client_core`, `alvr_client_mock` and `alvr_xtask` in the streamer tree, and of
  `alvr_client_core` in the client core tree.
- C++: `VideoEncoderPyroWave.cpp`, `CEncoder.cpp` and `alvr_server.cpp` pass a syntax check
  against the MinGW Windows headers, the Khronos Vulkan headers and PyroWave's `pyrowave.h`.
  Not built with MSVC, not run.
- Stripe schedule (`PyroWaveStripes.h`), against PyroWave's own `BlockLayout`: block count and
  band offsets agree, and for every intermediate decode step the iDWT tiles only read
  coefficient rows already received and LL rows the coarser level already produced, apron and
  edge mirroring included, and the last step covers everything. 8 resolutions x 4:2:0/4:4:4 x
  stripe heights 32-256.
- Stream socket (`cargo test -p alvr_sockets`): a frame of PyroWave-like packets plus a large
  one sent segmented, one by one, and with Windows refusing, gives the same datagrams on the
  wire, in less than a tenth of the calls when segmented, every call obeying the equal size rule;
  the single-call receive path reassembles every packet and header from them.
- Packet path, with synthetic encoder output: the streamer's packetizer and the client's parser
  (the same code the app runs), against PyroWave's own `BitstreamParser`: every packet parses
  on its own, every block arrives once and in its stripe, packets fit the datagram, the parsed
  data matches, and under random packet loss "stripes complete" never includes a stripe with a
  lost block. Also: another frame's packets, truncated and random data, payload overflow, an
  all-zero frame.
- Both tests are in the streamer repository, `alvr/server_openvr/cpp/tools/pyrowave_stripes_test`
  (`bash run.sh /path/to/pyrowave`, no GPU needed). Run them after touching the schedule.
- Client C header: generated with cbindgen; the new names are `ALVR_CODEC_PYRO_WAVE` and
  `encoder_pyrowave`. Xcode project: parses, and the sources are in the app target.

Not checked: nothing here has been compiled for or run on Windows with a GPU, or on
visionOS: in particular the Objective-C++ and Metal parts of the progressive decoder
(`pyrowave_decoder.mm`, the patched shaders) and the Swift code. First things to watch on a
real run:

- the streamer log lines starting with `PyroWave:` (device, queue priority, stripes and packet
  size; per 5 s: fps, Mbps, frame size, encode time, packets per frame),
- the app log lines starting with `PyroWave:` (per 5 s: frames shown, dropped, incomplete and
  packets lost, receive time and "last packet to decoded", the tail the stripe pipeline is
  meant to keep short),
- colors, and seams between stripes: a seam would mean a decode step ran before its inputs
  were complete,
- PyroWave frames go through the shader's BT.709 full range transform, not the private YCbCr
  texture formats VideoToolbox frames use.

## Latency: settings and diagnostics

First real run: everything worked; the dashboard showed Encoding ~5 ms and Frame Buffering ~11 ms.

**Frame Buffering** (`video_decoder_queue`: frame decoded -> renderer picks it up). 11 ms is one
display period: the frame waited a whole cycle. Two causes, both fixed as in the JPEG XS client:

- The renderers took the *oldest* of up to two queued frames and even held frames back to let the
  queue refill, so with two queued the frame shown was always one behind. App setting **Single
  Frame Buffer** (Advanced Settings, on by default): always the newest frame, older ones dropped.
  Measured with JPEG XS: Frame Buffering 12.3 -> 1.1 ms p50, total latency 61.6 -> 50.5 ms.
  PyroWave frames are all intra coded, so dropping one is harmless.
- App setting **Late Frame Pickup** (on by default, Metal renderer): picks the video frame just
  before visionOS's rendering deadline instead of at `optimalInputTime`, with a self-adjusting
  margin. Frames that arrive in that window are shown one cycle earlier. JPEG XS: total
  61.6 -> 58.5 ms. The app logs `frame timing` and `late frame pickup` lines every 450 frames.
- Streamer: **Phase-lock frame pacing to headset** (Video, on by default) aligns SteamVR's virtual
  vsync with the tracking arrival, so the phase between the two 90 Hz clocks no longer varies by
  session (JPEG XS: 50.5 vs 61.6 ms total, depending on the session). **Phase-lock: aim frames at
  the headset pickup** (off by default, experimental) closes the loop on Frame Buffering itself;
  it logs each step to `C:\Temp\alvr_phase_lock.log`.

**Encoding** (frame composed -> packets queued) covers more than PyroWave: the D3D11 composition
(FrameRender: layers, color correction, foveated encoding) runs on the GPU only after the frame is
reported composed, and shares the GPU with the game's next frame. The streamer now logs, every
5 s, where the time goes:

```
PyroWave timing, ms avg/max: wake | copy submit | encode submit | D3D11 GPU | Vulkan GPU | packetize | send | total
PyroWave GPU passes: DWT | Quant | Analyze | Resolve | Packing (GPU timestamps, ms per frame)
```

`D3D11 GPU` is the composition and the copy (the graphics queue, contended by the game);
`Vulkan GPU` is PyroWave's encode on its compute queue; `send` is the hand-over to Rust and the
send thread. If `D3D11 GPU` dominates, running SteamVR/the streamer as administrator lets ALVR
raise the process's GPU scheduling priority to realtime (vrserver.txt says `[GPU PRIO FIX]` when
it cannot). If `Vulkan GPU` dominates, check the `queue priority` line at stream start.

First measurement (streamer 2c20d008, render target 7296x3200, encoded 4288x1664, ~4.4 MB per
frame, 32 KB packets), ms avg, without / with a GPU-heavy game running:

| phase | no game | game | cause | fix |
|---|---|---|---|---|
| encode submit | 1.6-1.9 | 1.9-2.1 | PyroWave created 4 buffers per frame (2 of frame size) | `pyrowave-patches/0001`: reuse them |
| packetize | 2.0-2.3 | 2.3-3.1 | read of freshly mapped memory every frame | same patch |
| D3D11 GPU | 0.01 | 3.3-4.1 | composition waits behind the game on the graphics queue | device GPU thread priority 7; run as admin for the realtime class |
| Vulkan GPU | 1.2 | 1.2-1.45 | GPU passes 0.3 ms, the rest readback copy and queue hand-over | - |
| send | 0.6-0.9 | 0.8-1.05 | one allocation per packet | packet buffers recycled from the send thread |
| total | 5.3-6.5 | 10.3-10.9 | | |

The packets were also padded to 32 KB for UDP segmentation, ~9% of the bandwidth; padding is now
only used up to 9216 byte packets. The frame size line now also shows the coded size without
padding.

Second run (02ec7ef0): padding gone (coded = sent), encode submit 0.4-0.6 ms. Still open:
packetize 2.1-2.7 ms and, with a game, D3D11 GPU 3.3-5.5 ms (GPU thread priority 7 was granted
and made no difference). Found since:
- packetize: the best-fit search scanned the bucket bitmask from the packet's free space down.
  At 32 KB packets that is ~120 empty 64-bit words per block, for ~10000 blocks. A second bitmask
  level (which words are non-empty) makes it a few bit scans: 0.89 -> 0.49 ms on this machine
  for a 3.7 MB frame, output byte-identical (checked over 432 frame/packet-size combinations).
- D3D11 GPU: new line `PyroWave D3D11 GPU work: composition ... copy ...` from timestamp
  queries around FrameRender and the copy. D3D11 GPU minus these is waiting: SteamVR reports the
  frame composed when the work is submitted, not done, so the tail of the game's own rendering
  of that frame lands in "Encoding".
- Client latencies: the streamer now logs `Latency, ms p50/p95 over N frames: total | game |
  server compositor | encoding | network | decode | frame buffering | client compositor | vsync
  queue` every 5 s (the dashboard graph's numbers; session_log.txt only gets empty [GRAPH]
  lines). The app writes its `PyroWave:`, `frame timing`, `late frame pickup` and connection
  lines to Documents/pyrowave_debug.log (previous run: pyrowave_debug.prev.log).

Third run (4fbdac34, first one with client latencies), p50 at 5 bpp, 32 KB packets: total
48-60 ms, game 2.7-9.8, encoding 3.6-9.0, network 6.8-8.8, decode 0.6-2.9, frame buffering
3.8-8.9, client compositor 3.4-5.1, vsync queue 19.9-23.7.
- packetize 1.2-1.4 ms (was 2.1-2.7). D3D11 composition 0.13 ms + copy 0.02 ms on the GPU: the
  3-5 ms of "D3D11 GPU" in some windows are the game's own rendering of that frame, which
  SteamVR reports as composed before the GPU is done. In those windows encoding (8.7 ms p50) and
  the encoder's own total (8.8 ms) agree: there is no hidden queue between compose and encode.
- network is the transfer itself: at the 5 bpp limit (4.36 MB, which heavy scenes hit) the
  headset receives for 7.3 ms (first to last packet), ~4.7 Gbit/s. Bits per pixel is the lever:
  each 1 bpp at 4288x1664 is ~0.9 MB, ~1.5 ms on the wire.
- The send thread copied the whole frame into socket buffers before the first datagram left;
  it now hands them over in 256 KB chunks. At 32 KB packets a send call holds one datagram, so
  the segmentation staging copy is skipped there.
- visionOS switched the display to 100 Hz under the 90 fps stream mid-session (deadline-optimal
  10.00 ms; ~10% of display frames without a new video frame): the PyroWave path never asked for
  a display rate. It now does, as the VideoToolbox path always did ("Default" = the stream's rate).
- vsync queue is visionOS's fixed 17.7 ms from rendering deadline to photons plus the slack of the
  late pickup. Frame buffering depends on where frames land against the pickup:
  "Phase-lock: aim frames at the headset pickup" is the setting for that, now that the client
  shows the newest frame.

## Known limits and next steps

- **Display at 100 Hz under a 90 fps stream (open).** In every run with a client log visionOS ran
  the display at 100 Hz (deadline-optimal 10.00 ms), often only after a while; ~10% of display
  frames then have no new video frame, and frames drift through the display cycle (the streamer's
  pickup phase lock shows it as a lead that creeps up without end). While that lasts, frame
  buffering is spread over a whole period whatever either phase control does, which also made the
  4-bpp runs look like the pickup phase lock halved frame buffering. Suspected cause: the rate
  preference (AVDisplayCriteria) is set on the Entry window's display manager, and with "Dismiss
  Window on Enter" (default on) that window is gone before a video format exists, so the preference
  is never applied at all. The app now logs where the preference went ("Display criteria ..."),
  notices a mismatched display period ("Display rate: ...") and asks again every 10 s; Tracking Send
  Phase holds while the rates differ. With "Dismiss Window on Enter" off the preference does reach
  the window (matching enabled), and run 6 stayed at 90 Hz; but in run 7 visionOS switched to 100 Hz
  anyway, ~30 s in, right after a window with missed render deadlines, and asking again did not
  bring 90 Hz back. Runs 8-10 (window dismissed or not) stayed at 90 Hz and even with the window
  dismissed the preference reached a key window, so the window is not the cause; the switch looks
  sporadic. Workaround to try: stream at 100 fps (app: Stream refresh rate 100, streamer:
  preferred fps 100), so the stream matches the display.
- **Tracking Send Phase (app setting, off by default, Metal renderer).** The headset sends one
  pose per display cycle, and the total latency counts from that sample. Frame buffering (a
  decoded frame waiting for the pickup, 4-8 ms p50 in the third run) means the chain finished that
  much too early; only the headset can turn it into lower latency, by sampling the pose later.
  The streamer's "aim frames at the headset pickup" cannot: it starts rendering later but from the
  same pose, so frame buffering falls by what game time rises. With the setting on, the render
  thread sends the pose at optimalInputTime + a phase, while it waits anyway (before
  optimalInputTime or before the late pickup). With Late Frame Pickup, phase 0 is 3-6 ms EARLIER
  than the old send point (that was after the pickup and the command encoding). The phase is
  circular and moves either way. It minimises the expected wait: sending delta later turns each
  measured wait q into (q - delta) mod period, a miss costing nearly a whole period, and the total
  latency changes by exactly that; over 90 frames it tries every shift around the circle and moves
  up to 0.5 ms towards the best one, up to 2 ms when the gain is clear (after a load change).
  Run 12 confirmed it follows the load: in every settled window phase + (game + encoding) was
  25.2 ms mod the period, and frame buffering p50 came back to ~3 ms after each load change (in
  10-15 s with 0.5 ms steps, hence the bigger steps). Under game load ~3 ms p50 is the floor the
  spread sets (p10 ~0.5); a large part of that spread is decode (p50 ~1, p95 ~4 ms).
  Needs the streamer's "Phase-lock frame pacing to headset" and the pickup phase lock off (both
  would steer the same thing). Logs `tracking send phase: ...` every ~5 s.
  Measured 2026-09-27 at 90 Hz (first controller version, stuck at phase 0, i.e. just the earlier
  send): run 6 without it had frame buffering p50 0.3 / p95 10.9 ms (frames right on the pickup,
  half of them missing it) and total 46.6 / 57.9; run 7 with it 0.9 / 2.0 and total 39.4 / 39.5,
  the best so far. Game load differed between the runs, so only the spread is a fair comparison.
- **Network throughput.** About 2000 datagrams per frame at the default 3 bpp. The send and
  receive paths from the JPEG XS project are ported: on Windows the streamer hands the stack
  ~46 datagrams per call (UDP segmentation offload, Connection > "UDP: hand Windows many packets
  per send call", on by default; falls back to one call per datagram if Windows refuses), and
  the client reads each datagram with one syscall instead of peek plus read. The JPEG XS project
  measured the one-call-per-datagram send path capping at 0.4-2.7 Gbit/s. Not measured here;
  the streamer logs "UDP send segmentation: on" (or why not) after its first frame. Jumbo frames
  (a larger Connection > Packet size on a network that supports it) would cut the datagram
  count further and are independent of this.
- **Copies on the send path.** The frame is copied four times between encoder and socket
  (packetizer, FFI, socket buffer, segmentation staging), about 11 MB per frame at 3 bpp. On the
  send thread that is a fraction of a millisecond; worth removing if the send thread shows up.
- **The encoder still finishes a frame before the first packet leaves.** The encode is short
  (well under a millisecond), so the stripe pipeline only overlaps transfer and decode.
- The client runs a GPU submission per completed stripe (26 per frame by default). Larger
  stripes mean fewer submissions and a longer tail.
- HDR, and the Linux streamer, are not supported yet (both refuse PyroWave explicitly).
