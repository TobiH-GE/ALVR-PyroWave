#pragma once

#include "FrameMotion.h"
#include "NvEncoderD3D11.h"
#include "VideoEncoder.h"
#include "shared/d3drender.h"
#include <memory>

enum AdaptiveQuantizationMode { SpatialAQ = 1, TemporalAQ = 2 };

// Video encoder for NVIDIA NvEnc.
class VideoEncoderNVENC : public VideoEncoder {
public:
    VideoEncoderNVENC(std::shared_ptr<CD3DRender> pD3DRender, int width, int height);
    ~VideoEncoderNVENC();

    void Initialize();
    void Shutdown();

    void Transmit(
        ID3D11Texture2D* pTexture,
        uint64_t presentationTime,
        uint64_t targetTimestampNs,
        bool insertIDR
    );

    void SetFrameReadyTime(std::chrono::steady_clock::time_point time) override { m_frameReadyTime = time; }

private:
    void FillEncodeConfig(
        NV_ENC_INITIALIZE_PARAMS& initializeParams,
        int refreshRate,
        int renderWidth,
        int renderHeight,
        uint64_t bitrate_bps,
        NvEncoder* encoder = nullptr
    );
    // The QP map of the picture columns [x0, x0 + width) (the whole frame, or one eye's half).
    void BuildQpDeltaMap(int x0, int width, std::vector<int8_t>& map);
    void CreateEncoder(std::shared_ptr<NvEncoder>& encoder, int width, uint64_t bitrate_bps, uint32_t extraOutputDelay = 0);
    // MV-HEVC: encodes and sends one frame, its halves as the two views.
    void TransmitMultiview(ID3D11Texture2D* pTexture, uint64_t targetTimestampNs, bool insertIDR, bool motion);
    // MV-HEVC: queues one view, retrying once without the QP map if NVENC refuses it with the views.
    void SubmitView(int view, std::vector<int8_t>& map, bool insertIDR);
    void SendPackets(std::vector<std::vector<uint8_t>>& packets, uint64_t targetTimestampNs, bool insertIDR, int eye);

    std::ofstream fpOut;
    std::shared_ptr<NvEncoder> m_NvNecoder;
    // "Encode eyes separately": m_NvNecoder encodes the left half, this one the right half.
    std::shared_ptr<NvEncoder> m_NvNecoderRight;
    bool m_splitEyes = false;
    // "MV-HEVC": m_NvNecoder encodes the left half as view 0 and the right half as view 1 of one
    // multiview HEVC stream (m_qpDeltaMap and m_qpDeltaMapRight per view).
    bool m_mvHevc = false;
    bool m_mvQpMapReported = false;
    int m_encodeWidth = 0;

    std::shared_ptr<CD3DRender> m_pD3DRender;

    int m_codec;
    int m_refreshRate;
    int m_renderWidth;
    int m_renderHeight;
    int m_bitrateInMBits;

    // Foveated QP map: a QP offset per CTB (HEVC) or superblock (AV1), empty when off.
    std::vector<int8_t> m_qpDeltaMap;
    std::vector<int8_t> m_qpDeltaMapRight;

#ifdef ALVR_PYROWAVE
    // Motion vectors for the headset's frame interpolation (streamer setting "Send motion vectors").
    FrameMotion m_frameMotion;
#endif
    std::chrono::steady_clock::time_point m_frameReadyTime = std::chrono::steady_clock::now();
};
