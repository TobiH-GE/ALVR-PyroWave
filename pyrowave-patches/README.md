# Patches to PyroWave

`build_windows.ps1` applies every `*.patch` here, in name order, to PyroWave at the pinned
commit (`89f7e47d4abbf650c91fae766728af866c5e32a0`) before building its DLL. They only touch
the encoder's C API; the bitstream is unchanged, so the client's Metal decoder needs nothing.

- `0001-reuse-encoder-readback-buffers.patch`: `pyrowave_encoder_encode_gpu_synchronous_inner`
  created four buffers per frame (bitstream and block metadata, each as a GPU buffer and a
  host readback copy, the bitstream ones the size of the frame limit). On Windows/NVIDIA that
  showed up as ~1.8 ms of CPU in the encode call ("encode submit" in the streamer's timing log)
  and, because the bitstream was read from freshly mapped memory every frame, as ~2-3 ms for
  the packetizer's pass over it ("packetize"). The buffers are now kept and reused while their
  size stays the same; the previous frame's fence is waited for first (it already is, in the
  synchronous use the streamer makes of it).
- `0002-encoder-gpu-occupancy-timestamps.patch`: two more GPU timestamp intervals in the scaled
  encode, reported with the existing per-pass ones: "encode total" (from the first command of the
  encode, i.e. once the queue has waited for D3D11's fence, to the end of the readback copy: the
  GPU time the encode occupies, gaps between passes included) and "readback copy". The streamer
  turns them into a "PyroWave GPU share" log line.

- `0003-readback-copy-on-transfer-queue.patch` and `.granite.patch`: the readback copy of the
  bitstream to host memory (the whole ~4 MB buffer at 4 bpp) took 0.6-0.7 ms of the encode's
  ~1.0 ms GPU time in run 20, twice all compute passes together, on the compute queue. It now runs
  on a transfer queue of its own (the DMA engine) when the GPU has one: the compute submission
  signals a semaphore, the transfer queue waits for it, copies and signals the fence
  `get_mapped_raw_bitstream` waits for; the input image goes back to D3D11 right after the compute
  part. Granite used to put the transfer queue on the compute queue whenever a high priority compute
  queue was requested (our default, "High priority GPU queue"); `.granite.patch` still takes a
  transfer-only queue family if there is one, which gets no priority request, so the compute
  queue's is unaffected. Without such a queue, without host query reset or without timestamps on
  it, the copy stays where it was. The performance stats name it "readback copy (transfer queue)"
  when it moved, and "encode total" then ends with the compute passes. Checked on lavapipe
  (software Vulkan, one queue, the new path forced with `PYROWAVE_TEST_FORCE_TRANSFER_READBACK`):
  the coded blocks differ from the unpatched library no more than two runs of the unpatched one
  differ from each other, and not like a stale or partly copied frame would.

`*.granite.patch` apply to PyroWave's Granite checkout (made by `checkout_granite.sh`, not a
submodule), the others to PyroWave itself; `build_windows.ps1` does both.

To make a new one: apply the existing ones to a clean checkout of the pinned commit, change the
code, and save `git diff` as the next number.
