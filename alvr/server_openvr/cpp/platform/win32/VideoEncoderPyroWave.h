#pragma once

// PyroWave encoder (https://github.com/Themaister/pyrowave): an intra-only wavelet codec that
// runs entirely in Vulkan compute shaders, meant for multi-gigabit wired links where latency
// matters and bandwidth does not.
//
// The frame SteamVR hands us lives in a D3D11 texture. It is copied into a texture that is
// shared with Vulkan (NT handle, D3D11_TEXTURE_BIT), and a shared D3D11 fence, imported as a
// Vulkan timeline semaphore, orders the two APIs on the GPU. PyroWave converts RGB to YCbCr
// (BT.709, full range) itself, so the frame never goes through system memory.
//
// Only the headers are needed to build this (see deps/windows/pyrowave/README.md). The
// PyroWave DLL is loaded at runtime, so a missing DLL is a clear error at stream start rather
// than a driver that fails to load.

#ifdef ALVR_PYROWAVE

#include "VideoEncoder.h"
#include "shared/d3drender.h"

#include <chrono>
#include <d3d11_4.h>
#include <stdint.h>
#include <vector>
#include <wrl.h>

#include <vulkan/vulkan_core.h>

#include <pyrowave/pyrowave.h>

// Stripe schedule and wire format shared with the client (PyroWaveStripes::StreamConfig is
// the decoder config, PyroWaveStripes::PacketPrefix starts every packet).
#include "PyroWaveStripes.h"

class VideoEncoderPyroWave : public VideoEncoder {
public:
    VideoEncoderPyroWave(std::shared_ptr<CD3DRender> d3dRender, int width, int height);
    ~VideoEncoderPyroWave();

    void Initialize() override;
    void Shutdown() override;

    void Transmit(
        ID3D11Texture2D* pTexture,
        uint64_t presentationTime,
        uint64_t targetTimestampNs,
        bool insertIDR
    ) override;

    void SetFrameReadyTime(std::chrono::steady_clock::time_point time) override {
        m_frameReadyTime = time;
    }

    void BeforeFrameRender() override;

private:
    // The entry points of the PyroWave DLL that this encoder uses.
    struct Api {
        HMODULE module = nullptr;
        decltype(&pyrowave_get_api_version) get_api_version = nullptr;
        decltype(&pyrowave_create_device_by_compat2) create_device_by_compat2 = nullptr;
        decltype(&pyrowave_device_get_global_priority) device_get_global_priority = nullptr;
        decltype(&pyrowave_device_destroy) device_destroy = nullptr;
        decltype(&pyrowave_device_report_performance_stats) device_report_performance_stats
            = nullptr;
        decltype(&pyrowave_sync_object_create) sync_object_create = nullptr;
        decltype(&pyrowave_sync_object_get_semaphore) sync_object_get_semaphore = nullptr;
        decltype(&pyrowave_sync_object_destroy) sync_object_destroy = nullptr;
        decltype(&pyrowave_image_create) image_create = nullptr;
        decltype(&pyrowave_image_get_image_view) image_get_image_view = nullptr;
        decltype(&pyrowave_image_destroy) image_destroy = nullptr;
        decltype(&pyrowave_encoder_create) encoder_create = nullptr;
        decltype(&pyrowave_encoder_encode_gpu_scaled_synchronous
        ) encoder_encode_gpu_scaled_synchronous
            = nullptr;
        decltype(&pyrowave_encoder_get_mapped_raw_bitstream) encoder_get_mapped_raw_bitstream
            = nullptr;
        decltype(&pyrowave_encoder_destroy) encoder_destroy = nullptr;
    };

    void LoadApi();
    void CreateSharedTexture(const D3D11_TEXTURE2D_DESC& inputDesc);
    void SendStreamConfig();
    // Cuts the coded blocks of the frame just encoded (as mapped by
    // get_mapped_raw_bitstream) into packets, stripe by stripe, into m_sendBuffer /
    // m_packetSizes. Returns false if the bitstream does not look right.
    bool BuildPackets(
        const void* mappedBitstream, size_t bitstreamBytes, const void* mappedMeta, size_t metaBytes
    );
    void ReleaseResources();
    // D3D11 timestamp queries around the composition and the copy, read back a few frames later.
    void CreateGpuTimers();
    void CollectGpuTimers();
    // Adds one frame's split to the statistics and logs them every 5 s.
    void ReportTiming(const double (&phaseMs)[8], size_t frameBytes, size_t packetCount);

    std::shared_ptr<CD3DRender> m_d3dRender;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_device5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;

    const int m_width;
    const int m_height;
    const pyrowave_chroma_subsampling m_chroma;
    const size_t m_maxFrameBytes;

    Api m_api;
    pyrowave_device m_device = nullptr;
    pyrowave_encoder m_encoder = nullptr;

    // The frame is copied here, where Vulkan can see it.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTexture;
    pyrowave_image m_image = nullptr;
    pyrowave_image_view m_imageView = {};

    // Shared timeline. Odd values are signaled by D3D11 when a copy is done, even values by
    // Vulkan when it has read the copy.
    Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
    pyrowave_sync_object m_sync = nullptr;
    uint64_t m_fenceValue = 0;
    // Fires when D3D11 has finished the copy (and with it the composition before it), so the
    // time the GPU spends on each API shows up separately in the log.
    HANDLE m_copyDoneEvent = nullptr;

    // What the D3D11 GPU time is made of: timestamps before the composition (FrameRender), before
    // the copy and after it. Several frames in flight, since results are only read once ready.
    struct GpuTimer {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> composeStart;
        Microsoft::WRL::ComPtr<ID3D11Query> copyStart;
        Microsoft::WRL::ComPtr<ID3D11Query> copyEnd;
        bool pending = false;
        // False for a frame that never reached Transmit.
        bool valid = false;
    };
    static constexpr int kGpuTimerCount = 4;
    GpuTimer m_gpuTimers[kGpuTimerCount];
    int m_gpuTimerNext = 0;
    // The timer BeforeFrameRender started for the frame Transmit gets next, or -1.
    int m_gpuTimerActive = -1;
    uint64_t m_statGpuTimed = 0;
    double m_statComposeGpuMs = 0.0;
    double m_statComposeGpuMaxMs = 0.0;
    double m_statCopyGpuMs = 0.0;

    PyroWaveStripes::Geometry m_geometry;
    // Block indices of each stripe, in PyroWave's block order.
    std::vector<std::vector<uint32_t>> m_stripeBlocks;
    // Largest packet (prefix included) that fits one datagram; a single coded block larger
    // than this still goes out as one packet, which the socket then splits.
    size_t m_packetBytes = 0;
    std::vector<uint8_t> m_sendBuffer;
    std::vector<uint32_t> m_packetSizes;
    bool m_configSent = false;
    // Set when a frame could not be encoded for good (e.g. an input format the encoder does
    // not take), so the log gets one error instead of one per frame.
    bool m_failed = false;

    std::chrono::steady_clock::time_point m_frameReadyTime;
    std::chrono::steady_clock::time_point m_statStart;
    uint64_t m_statFrames = 0;
    uint64_t m_statBytes = 0;
    uint64_t m_statCodedBytes = 0;
    uint64_t m_statPackets = 0;
    // Per phase of Transmit, see kPhaseNames in the .cpp.
    double m_statPhaseSumMs[8] = {};
    double m_statPhaseMaxMs[8] = {};
};

#endif
