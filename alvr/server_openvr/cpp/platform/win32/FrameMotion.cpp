#include "FrameMotion.h"

#ifdef ALVR_PYROWAVE

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"

#include <algorithm>

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double FramePeriodMs() {
    const int refreshRate = Settings::Instance().m_refreshRate;
    return 1000.0 / (refreshRate > 0 ? refreshRate : 90);
}

// The optical flow input takes the frame's values as they are (gamma encoded), like PyroWave's
// copy of the frame does: an sRGB texture is read through its UNORM twin.
DXGI_FORMAT CopyFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return format;
    }
}

} // namespace

bool FrameMotion::Enabled() {
    return Settings::Instance().m_pyroWaveMotionVectors
        && Settings::Instance().m_codec != ALVR_CODEC_PYROWAVE;
}

void FrameMotion::Submit(
    ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, uint64_t timestampNs
) {
    m_submitted = false;
    m_vectorsSent = false;
    m_choiceSent = false;
    if (m_unavailable) {
        return;
    }
    if (!m_motion) {
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        // Vectors every 16 pixels, as with PyroWave.
        m_motion = std::make_unique<PyroWaveMotion>();
        if (!m_motion->Initialize(device, context, CopyFormat(desc.Format), (int)desc.Width, (int)desc.Height, 16)) {
            m_motion.reset();
            m_unavailable = true;
            return;
        }
        Info("Frame motion: sending motion vectors with the %ux%u frames, ahead of each frame\n", desc.Width, desc.Height);
    }
    m_motion->Submit(frame, timestampNs);
    // Collect polls without flushing.
    context->Flush();
    m_submitted = true;
}

void FrameMotion::SendBeforeFrame(uint64_t timestampNs, Clock::time_point frameReady) {
    if (!m_submitted) {
        return;
    }
    // Only what is ready already: the frame does not wait for its motion any more.
    m_vectorsSent = SendVectors(timestampNs, false, 0.0, true);
    if (m_vectorsSent) {
        m_choiceSent = SendChoice(timestampNs, false, 0.0, true);
    }
}

void FrameMotion::SendAfterFrame(uint64_t timestampNs, Clock::time_point frameReady) {
    if (!m_submitted) {
        return;
    }
    m_submitted = false;
    // The frame is out already (and its texture released), so waiting costs it nothing, only this
    // thread's time until the next frame. The vectors as long as the frame used to wait for them
    // (90 % of the frame period after its composition), the choice up to the frame period.
    const Clock::time_point start = Clock::now();
    if (!m_vectorsSent) {
        const double budgetMs = std::max(0.9 * FramePeriodMs() - MsSince(frameReady), 1.0);
        m_vectorsSent = SendVectors(timestampNs, true, budgetMs, false);
    }
    if (m_vectorsSent && !m_choiceSent) {
        SendChoice(timestampNs, true, std::max(FramePeriodMs() - MsSince(frameReady), 1.0), false);
    }
    const double waitedMs = MsSince(start);
    m_statWaitMs += waitedMs;
    m_statWaitMaxMs = std::max(m_statWaitMaxMs, waitedMs);
    // Never sent with a later frame.
    m_motion->DiscardChoice();
    m_motion->ReportIfDue();
    m_statFrames++;
    Report();
}

bool FrameMotion::SendVectors(uint64_t timestampNs, bool wait, double waitMs, bool beforeFrame) {
    m_buffer.clear();
    m_sizes.clear();
    if (!m_motion->Collect(wait, waitMs, 0, Settings::Instance().m_pyroWavePacketBytes, m_buffer, m_sizes)) {
        return false;
    }
    // A piece of its own. Ahead of the frame as its first piece: the frame's own first piece
    // follows and starts the frame's byte count again, so the motion does not count as video.
    // Behind the frame neither first nor last, after the frame's count was reported.
    VideoSendPackets(timestampNs, m_buffer.data(), m_sizes.data(), (unsigned int)m_sizes.size(), false, beforeFrame, false);
    (beforeFrame ? m_statVectorsBefore : m_statVectorsAfter)++;
    m_statBytes += m_buffer.size();
    return true;
}

bool FrameMotion::SendChoice(uint64_t timestampNs, bool wait, double waitMs, bool beforeFrame) {
    m_buffer.clear();
    m_sizes.clear();
    const uint32_t packetBytes = Settings::Instance().m_pyroWavePacketBytes;
    if (beforeFrame) {
        // Polled without waiting: CollectChoice gives a choice up when its wait runs out, and this
        // one must stay for SendAfterFrame (it lost 5-12 % of the choices, 2026-10-12, which made
        // those in-betweens follow the raw vectors and smear text).
        const Clock::time_point start = Clock::now();
        while (!m_motion->CollectChoice(false, 0.0, 0, packetBytes, m_buffer, m_sizes)) {
            if (MsSince(start) > waitMs) {
                return false;
            }
            Sleep(0);
        }
    } else if (!m_motion->CollectChoice(wait, waitMs, 0, packetBytes, m_buffer, m_sizes)) {
        return false;
    }
    VideoSendPackets(
        timestampNs, m_buffer.data(), m_sizes.data(), (unsigned int)m_sizes.size(), false, beforeFrame, false
    );
    (beforeFrame ? m_statChoiceBefore : m_statChoiceAfter)++;
    m_statBytes += m_buffer.size();
    return true;
}

void FrameMotion::Report() {
    const double sinceMs = MsSince(m_statStart);
    if (sinceMs < 5000.0 || m_statFrames == 0) {
        return;
    }
    Info(
        "Frame motion: %llu frames, vectors ahead of %llu and after %llu, choice ahead of %llu and "
        "after %llu, waited for them after the frame avg %.2f max %.2f ms, %.1f Mbit/s\n",
        (unsigned long long)m_statFrames,
        (unsigned long long)m_statVectorsBefore,
        (unsigned long long)m_statVectorsAfter,
        (unsigned long long)m_statChoiceBefore,
        (unsigned long long)m_statChoiceAfter,
        m_statWaitMs / m_statFrames,
        m_statWaitMaxMs,
        m_statBytes * 8.0 / (sinceMs * 1000.0)
    );
    m_statStart = Clock::now();
    m_statFrames = m_statVectorsBefore = m_statVectorsAfter = m_statChoiceBefore = m_statChoiceAfter = m_statBytes = 0;
    m_statWaitMs = m_statWaitMaxMs = 0.0;
}

#endif
