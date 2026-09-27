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

To make a new one: apply the existing ones to a clean checkout of the pinned commit, change the
code, and save `git diff` as the next number.
