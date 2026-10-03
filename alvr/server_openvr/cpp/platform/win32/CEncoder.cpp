#include "CEncoder.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// Keeps the encoder thread on the fastest cores and out of power throttling (EcoQoS), like the
// video send thread (server_core machine.rs). On a hybrid CPU Windows moves threads between core
// classes on its own; measured in the JPEG XS streamer, the same work took ~3x as long on an
// efficiency core and ~7x on a throttled one. The functions are looked up at runtime, so older
// Windows versions just skip it. Returns what was done, for the log.
std::string KeepOnPerformanceCores() {
    typedef BOOL(WINAPI * GetCpuSetsFn)(PVOID, ULONG, PULONG, HANDLE, ULONG);
    typedef BOOL(WINAPI * SetThreadCpuSetsFn)(HANDLE, const ULONG*, ULONG);
    typedef BOOL(WINAPI * SetThreadInformationFn)(HANDLE, int, LPVOID, DWORD);
    struct PowerThrottlingState {
        ULONG version;
        ULONG controlMask;
        ULONG stateMask;
    };
    const int kThreadPowerThrottling = 3;
    const ULONG kExecutionSpeed = 1;

    std::string notes;
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    auto setInfo = kernel32
        ? reinterpret_cast<SetThreadInformationFn>(GetProcAddress(kernel32, "SetThreadInformation"))
        : nullptr;
    auto getSets = kernel32
        ? reinterpret_cast<GetCpuSetsFn>(GetProcAddress(kernel32, "GetSystemCpuSetInformation"))
        : nullptr;
    auto setSets = kernel32
        ? reinterpret_cast<SetThreadCpuSetsFn>(GetProcAddress(kernel32, "SetThreadSelectedCpuSets"))
        : nullptr;

    // Control bit set, state bit clear: this thread opts out of EcoQoS.
    PowerThrottlingState state = { 1, kExecutionSpeed, 0 };
    bool ok = setInfo && setInfo(GetCurrentThread(), kThreadPowerThrottling, &state, sizeof(state));
    notes += ok ? "power throttling off ok" : "power throttling off FAILED";

    if (!getSets || !setSets) {
        return notes + ", cpu sets unavailable";
    }
    // SYSTEM_CPU_SET_INFORMATION entries: Size u32 @0, Type u32 @4, Id u32 @8,
    // EfficiencyClass u8 @18. A higher class is a faster core.
    ULONG length = 0;
    getSets(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<unsigned char> buffer(length);
    if (length == 0 || !getSets(buffer.data(), length, &length, GetCurrentProcess(), 0)) {
        return notes + ", cpu sets unavailable";
    }
    std::vector<std::pair<ULONG, unsigned char>> sets;
    for (size_t offset = 0; offset + 20 <= length;) {
        ULONG size, kind, id;
        memcpy(&size, &buffer[offset], 4);
        memcpy(&kind, &buffer[offset + 4], 4);
        memcpy(&id, &buffer[offset + 8], 4);
        if (size < 20) {
            break;
        }
        if (kind == 0) {
            sets.push_back({ id, buffer[offset + 18] });
        }
        offset += size;
    }
    unsigned char fastest = 0, slowest = 255;
    for (auto& set : sets) {
        fastest = set.second > fastest ? set.second : fastest;
        slowest = set.second < slowest ? set.second : slowest;
    }
    if (sets.empty() || fastest == slowest) {
        return notes + ", " + std::to_string(sets.size())
            + " logical cpus, all one class: no pinning";
    }
    std::vector<ULONG> ids;
    for (auto& set : sets) {
        if (set.second == fastest) {
            ids.push_back(set.first);
        }
    }
    ok = setSets(GetCurrentThread(), ids.data(), (ULONG)ids.size());
    return notes + ", pinned to " + std::to_string(ids.size()) + " of "
        + std::to_string(sets.size()) + " logical cpus (efficiency class " + std::to_string(fastest)
        + ") " + (ok ? "ok" : "FAILED");
}

} // namespace

CEncoder::CEncoder()
    : m_bExiting(false)
    , m_targetTimestampNs(0) {
    m_encodeFinished.Set();
}

CEncoder::~CEncoder() {
    if (m_videoEncoder) {
        m_videoEncoder->Shutdown();
        m_videoEncoder.reset();
    }
}

void CEncoder::Initialize(std::shared_ptr<CD3DRender> d3dRender) {
    m_FrameRender = std::make_shared<FrameRender>(d3dRender);
    m_FrameRender->Startup();
    uint32_t encoderWidth, encoderHeight;
    m_FrameRender->GetEncodingResolution(&encoderWidth, &encoderHeight);

    // The client was told the codec during connection negotiation, so PyroWave cannot fall back
    // to another encoder: the headset would get a stream it does not expect.
    if (Settings::Instance().m_codec == ALVR_CODEC_PYROWAVE) {
#ifdef ALVR_PYROWAVE
        Debug("Try to use VideoEncoderPyroWave.\n");
        m_videoEncoder
            = std::make_shared<VideoEncoderPyroWave>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
#else
        throw MakeException("PyroWave was selected, but this streamer was built without it. See "
                            "deps/windows/pyrowave/README.md.");
#endif
    }

    Exception vceException;
    Exception nvencException;
#ifdef ALVR_GPL
    Exception swException;
    if (Settings::Instance().m_force_sw_encoding) {
        try {
            Debug("Try to use VideoEncoderSW.\n");
            m_videoEncoder
                = std::make_shared<VideoEncoderSW>(d3dRender, encoderWidth, encoderHeight);
            m_videoEncoder->Initialize();
            return;
        } catch (Exception e) {
            swException = e;
        }
    }
#endif

    try {
        Debug("Try to use VideoEncoderAMF.\n");
        m_videoEncoder = std::make_shared<VideoEncoderAMF>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        vceException = e;
    }
    try {
        Debug("Try to use VideoEncoderNVENC.\n");
        m_videoEncoder
            = std::make_shared<VideoEncoderNVENC>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        nvencException = e;
    }
#ifdef ALVR_GPL
    try {
        Debug("Try to use VideoEncoderSW.\n");
        m_videoEncoder = std::make_shared<VideoEncoderSW>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        swException = e;
    }
    throw MakeException(
        "All VideoEncoder are not available. VCE: %s, NVENC: %s, SW: %s",
        vceException.what(),
        nvencException.what(),
        swException.what()
    );
#else
    throw MakeException(
        "All VideoEncoder are not available. VCE: %s, NVENC: %s",
        vceException.what(),
        nvencException.what()
    );
#endif
}

void CEncoder::SetViewsConfig(
    vr::HmdRect2_t projLeft,
    vr::HmdMatrix34_t eyeToHeadLeft,
    vr::HmdRect2_t projRight,
    vr::HmdMatrix34_t eyeToHeadRight
) {
    m_FrameRender->SetViewsConfig(projLeft, eyeToHeadLeft, projRight, eyeToHeadRight);
}

bool CEncoder::CopyToStaging(
    ID3D11Texture2D* pTexture[][2],
    vr::VRTextureBounds_t bounds[][2],
    vr::HmdMatrix34_t poses[],
    int layerCount,
    bool recentering,
    uint64_t presentationTime,
    uint64_t targetTimestampNs,
    const std::string& message,
    const std::string& debugText
) {
    m_presentationTime = presentationTime;
    m_targetTimestampNs = targetTimestampNs;
    m_FrameRender->Startup();

    if (m_videoEncoder) {
        m_videoEncoder->BeforeFrameRender();
    }
    m_FrameRender->RenderFrame(
        pTexture, bounds, poses, layerCount, recentering, message, debugText
    );
    return true;
}

void CEncoder::Run() {
    Debug("CEncoder: Start thread. Id=%d\n", GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_MOST_URGENT);
    Info("CEncoder: encoder thread %s\n", KeepOnPerformanceCores().c_str());
    if (m_videoEncoder) {
        m_videoEncoder->SetInputReleasedCallback([this] { ReleaseInput(); });
    }

    while (!m_bExiting) {
        m_newFrameReady.Wait();
        if (m_bExiting)
            break;

        m_inputReleased = false;
        if (m_FrameRender->GetTexture()) {
            m_videoEncoder->SetFrameReadyTime(m_frameReadyTime);
            m_videoEncoder->Transmit(
                m_FrameRender->GetTexture().Get(),
                m_presentationTime,
                m_targetTimestampNs,
                m_scheduler.CheckIDRInsertion()
            );
        }

        ReleaseInput();
    }
}

void CEncoder::ReleaseInput() {
    // Once per frame: m_encodeFinished is an auto-reset event, so a second Set would let the
    // present thread compose over a frame this thread has not copied yet.
    if (!m_inputReleased) {
        m_inputReleased = true;
        m_encodeFinished.Set();
    }
}

void CEncoder::Stop() {
    m_bExiting = true;
    m_newFrameReady.Set();
    Join();
    m_FrameRender.reset();
}

void CEncoder::NewFrameReady() {
    m_frameReadyTime = std::chrono::steady_clock::now();
    m_encodeFinished.Reset();
    m_newFrameReady.Set();
}

void CEncoder::WaitForEncode() { m_encodeFinished.Wait(); }

void CEncoder::OnStreamStart() { m_scheduler.OnStreamStart(); }

void CEncoder::OnPacketLoss() { m_scheduler.OnPacketLoss(); }

void CEncoder::InsertIDR() { m_scheduler.InsertIDR(); }

void CEncoder::CaptureFrame() { }
