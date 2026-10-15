#pragma once

// Motion vectors (and the motion choice) for the headset's frame interpolation with the codecs
// other than PyroWave, which sends them between its own packets (VideoEncoderPyroWave). Streamer
// settings "Send motion vectors" and "Send motion choice": the same PyroWaveMotion and the same
// packets (PyroWaveStripes::MotionHeader behind a PacketPrefix starting with 0xffff, which no
// H.264/HEVC start code or AV1 OBU header does), sent as extra video packets with the frame's
// timestamp, so the headset tells them from the frame by their first two bytes.
//
// The frame goes out as soon as it is encoded; its motion follows it as soon as it is ready (or
// goes ahead of it if it already is), and the headset holds the frame back until then. The frame
// used to wait for its vectors on the streamer (until 90 % of the frame period after its
// composition): in heavy scenes 13-18 ms after its encode at 50 Hz, the optical flow's own time,
// during which it could have been on its way (2026-10-13). The vectors are waited for after the
// frame just as long, the choice up to the frame period.
//
// The encoder calls Submit right after it handed the frame to the hardware encoder (so the
// encode never waits for the optical flow on the GPU), SendBeforeFrame before its first packet of
// the frame and SendAfterFrame after the last.

#ifdef ALVR_PYROWAVE

#include "PyroWaveMotion.h"

#include <chrono>
#include <d3d11.h>
#include <memory>
#include <stdint.h>
#include <vector>

class FrameMotion {
public:
    // False when the setting is off; true does not mean the GPU can do it (Submit finds out once).
    static bool Enabled();

    void Submit(
        ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, uint64_t timestampNs
    );
    void SendBeforeFrame(uint64_t timestampNs, std::chrono::steady_clock::time_point frameReady);
    void SendAfterFrame(uint64_t timestampNs, std::chrono::steady_clock::time_point frameReady);

private:
    bool SendVectors(uint64_t timestampNs, bool wait, double waitMs, bool beforeFrame);
    bool SendChoice(uint64_t timestampNs, bool wait, double waitMs, bool beforeFrame);
    void Report();

    std::unique_ptr<PyroWaveMotion> m_motion;
    bool m_unavailable = false;
    bool m_submitted = false;
    bool m_vectorsSent = false;
    bool m_choiceSent = false;
    std::vector<uint8_t> m_buffer;
    std::vector<uint32_t> m_sizes;

    // For the log every 5 s: how long frames waited for their vectors, and what went out when.
    std::chrono::steady_clock::time_point m_statStart = std::chrono::steady_clock::now();
    uint64_t m_statFrames = 0;
    uint64_t m_statVectorsBefore = 0;
    uint64_t m_statVectorsAfter = 0;
    uint64_t m_statChoiceBefore = 0;
    uint64_t m_statChoiceAfter = 0;
    uint64_t m_statBytes = 0;
    double m_statWaitMs = 0.0;
    double m_statWaitMaxMs = 0.0;
};

#endif
