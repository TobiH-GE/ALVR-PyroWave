#include "VideoEncoderNVENC.h"
#include "FFR.h"
#include "NvCodecUtils.h"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"

#include <algorithm>
#include <cmath>

VideoEncoderNVENC::VideoEncoderNVENC(std::shared_ptr<CD3DRender> pD3DRender, int width, int height)
    : m_pD3DRender(pD3DRender)
    , m_codec(Settings::Instance().m_codec)
    , m_refreshRate(Settings::Instance().m_refreshRate)
    , m_renderWidth(width)
    , m_renderHeight(height)
    , m_bitrateInMBits(30) { }

VideoEncoderNVENC::~VideoEncoderNVENC() { }

void VideoEncoderNVENC::Initialize() {
    // "Encode eyes separately": an encoder per eye, each on half the width and half the bitrate.
    // H.264 and HEVC only (the client builds a decoder per eye from the one configuration both
    // halves share; AV1 sets its decoder up differently there).
    m_splitEyes = Settings::Instance().m_nvencSplitEyes
        && (m_codec == ALVR_CODEC_HEVC || m_codec == ALVR_CODEC_H264) && m_renderWidth % 4 == 0;
    if (Settings::Instance().m_nvencSplitEyes && !m_splitEyes) {
        Warn("Encode eyes separately: only for H.264 and HEVC, encoding the whole frame\n");
    }
    m_encodeWidth = m_splitEyes ? m_renderWidth / 2 : m_renderWidth;

    BuildQpDeltaMap(0, m_encodeWidth, m_qpDeltaMap);
    if (m_splitEyes) {
        BuildQpDeltaMap(m_encodeWidth, m_encodeWidth, m_qpDeltaMapRight);
    }

    const uint64_t bitrate = m_bitrateInMBits * 1'000'000L / (m_splitEyes ? 2 : 1);
    CreateEncoder(m_NvNecoder, m_encodeWidth, bitrate);
    if (m_splitEyes) {
        CreateEncoder(m_NvNecoderRight, m_encodeWidth, bitrate);
        Info("Encode eyes separately: two encoders of %dx%d\n", m_encodeWidth, m_renderHeight);
    }

    Debug("CNvEncoder is successfully initialized.\n");
}

void VideoEncoderNVENC::CreateEncoder(std::shared_ptr<NvEncoder>& encoder, int width, uint64_t bitrate_bps) {
    NV_ENC_BUFFER_FORMAT format
        = Settings::Instance().m_enableHdr ? NV_ENC_BUFFER_FORMAT_NV12 : NV_ENC_BUFFER_FORMAT_ABGR;

    if (Settings::Instance().m_use10bitEncoder) {
        format = Settings::Instance().m_enableHdr ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT
                                                  : NV_ENC_BUFFER_FORMAT_ABGR10;
    }

    Debug("Initializing CNvEncoder. Width=%d Height=%d Format=%d\n", width, m_renderHeight, format);

    try {
        encoder = std::make_shared<NvEncoderD3D11>(m_pD3DRender->GetDevice(), width, m_renderHeight, format, 0);
    } catch (NVENCException e) {
        throw MakeException(
            "NvEnc NvEncoderD3D11 failed. Code=%d %hs\n", e.getErrorCode(), e.what()
        );
    }

    NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
    initializeParams.encodeConfig = &encodeConfig;

    FillEncodeConfig(initializeParams, m_refreshRate, width, m_renderHeight, bitrate_bps, encoder.get());
    try {
        encoder->CreateEncoder(&initializeParams);
    } catch (NVENCException e) {
        if (e.getErrorCode() == NV_ENC_ERR_INVALID_PARAM) {
            throw MakeException(
                "This GPU does not support H.265 encoding. (NvEncoderCuda NV_ENC_ERR_INVALID_PARAM)"
            );
        }
        throw MakeException("NvEnc CreateEncoder failed. Code=%d %hs", e.getErrorCode(), e.what());
    }
}

void VideoEncoderNVENC::Shutdown() {
    for (auto* encoder : { &m_NvNecoder, &m_NvNecoderRight }) {
        if (!*encoder) {
            continue;
        }
        std::vector<std::vector<uint8_t>> vPacket;
        (*encoder)->EndEncode(vPacket);
        for (std::vector<uint8_t>& packet : vPacket) {
            if (fpOut) {
                fpOut.write(reinterpret_cast<char*>(packet.data()), packet.size());
            }
        }
        (*encoder)->DestroyEncoder();
        encoder->reset();
    }

    Debug("CNvEncoder::Shutdown\n");

    if (fpOut) {
        fpOut.close();
    }
}

void VideoEncoderNVENC::Transmit(
    ID3D11Texture2D* pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR
) {
    auto params = GetDynamicEncoderParams();
    if (params.updated) {
        m_bitrateInMBits = params.bitrate_bps / 1'000'000;
        for (auto* encoder : { &m_NvNecoder, &m_NvNecoderRight }) {
            if (!*encoder) {
                continue;
            }
            NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
            initializeParams.encodeConfig = &encodeConfig;
            FillEncodeConfig(
                initializeParams,
                params.framerate,
                m_encodeWidth,
                m_renderHeight,
                m_bitrateInMBits * 1'000'000L / (m_splitEyes ? 2 : 1),
                encoder->get()
            );
            NV_ENC_RECONFIGURE_PARAMS reconfigureParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfigureParams.reInitEncodeParams = initializeParams;
            (*encoder)->Reconfigure(&reconfigureParams);
        }
    }

    auto picParamsFor = [&](std::vector<int8_t>& map) {
        NV_ENC_PIC_PARAMS picParams = {};
        if (insertIDR) {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
        }
        if (!map.empty()) {
            picParams.qpDeltaMap = map.data();
            picParams.qpDeltaMapSize = (uint32_t)map.size();
        }
        return picParams;
    };
    if (insertIDR) {
        Debug("Inserting IDR frame.\n");
    }
    std::vector<std::vector<uint8_t>> vPacket;
#ifdef ALVR_PYROWAVE
    const bool motion = FrameMotion::Enabled();
#else
    const bool motion = false;
#endif

    if (m_splitEyes) {
        // Both halves are queued before either is waited for, so the two encoders run at the same
        // time; the left half goes out as soon as it is done.
        auto* context = m_pD3DRender->GetContext();
        for (int eye = 0; eye < 2; eye++) {
            auto& encoder = eye == 0 ? m_NvNecoder : m_NvNecoderRight;
            const NvEncInputFrame* input = encoder->GetNextInputFrame();
            D3D11_BOX box = { UINT(eye * m_encodeWidth), 0, 0, UINT((eye + 1) * m_encodeWidth), UINT(m_renderHeight), 1 };
            context->CopySubresourceRegion(
                reinterpret_cast<ID3D11Texture2D*>(input->inputPtr), 0, 0, 0, 0, pTexture, 0, &box
            );
        }
        NV_ENC_PIC_PARAMS left = picParamsFor(m_qpDeltaMap), right = picParamsFor(m_qpDeltaMapRight);
        m_NvNecoder->SubmitFrame(&left);
        m_NvNecoderRight->SubmitFrame(&right);
#ifdef ALVR_PYROWAVE
        if (motion) {
            m_frameMotion.Submit(m_pD3DRender->GetDevice(), context, pTexture, targetTimestampNs);
        }
#endif
        // Every read of the composed texture is queued (the halves' copies, the optical flow's
        // copy) ahead of anything the present thread records next: SteamVR may compose the next
        // frame now. It waited for this thread up to 12 ms while the frame waited for its motion
        // vectors at 50 Hz (2026-10-13), which made frames late.
        ReleaseInput();
#ifdef ALVR_PYROWAVE
        if (motion) {
            m_frameMotion.SendBeforeFrame(targetTimestampNs, m_frameReadyTime);
        }
#endif
        m_NvNecoder->CollectFrame(vPacket);
        SendPackets(vPacket, targetTimestampNs, insertIDR, 0);
        m_NvNecoderRight->CollectFrame(vPacket);
        SendPackets(vPacket, targetTimestampNs, insertIDR, 1);
    } else {
        const NvEncInputFrame* encoderInputFrame = m_NvNecoder->GetNextInputFrame();
        ID3D11Texture2D* pInputTexture = reinterpret_cast<ID3D11Texture2D*>(encoderInputFrame->inputPtr);
        m_pD3DRender->GetContext()->CopyResource(pInputTexture, pTexture);

        NV_ENC_PIC_PARAMS picParams = picParamsFor(m_qpDeltaMap);
#ifdef ALVR_PYROWAVE
        if (motion) {
            // The optical flow of this frame, queued once the encode is (so the encode never waits
            // for it on the GPU); its packets go out with the frame (FrameMotion.h).
            m_NvNecoder->EncodeFrame(vPacket, &picParams, [&] {
                m_frameMotion.Submit(m_pD3DRender->GetDevice(), m_pD3DRender->GetContext(), pTexture, targetTimestampNs);
                // The composed texture is read by queued copies only from here on (as above).
                ReleaseInput();
            });
            m_frameMotion.SendBeforeFrame(targetTimestampNs, m_frameReadyTime);
        } else {
            ReleaseInput();
            m_NvNecoder->EncodeFrame(vPacket, &picParams);
        }
#else
        ReleaseInput();
        m_NvNecoder->EncodeFrame(vPacket, &picParams);
#endif
        SendPackets(vPacket, targetTimestampNs, insertIDR, -1);
    }
#ifdef ALVR_PYROWAVE
    if (motion) {
        m_frameMotion.SendAfterFrame(targetTimestampNs, m_frameReadyTime);
    }
#endif
}

// eye -1: the whole frame; 0 or 1: that eye's half, the left one first.
void VideoEncoderNVENC::SendPackets(
    std::vector<std::vector<uint8_t>>& packets, uint64_t targetTimestampNs, bool insertIDR, int eye
) {
    for (size_t i = 0; i < packets.size(); i++) {
        uint8_t* buf = packets[i].data();
        int len = (int)packets[i].size();

        // NVENC's AV1 encoding includes a bunch of IVF wrapping,
        // so we need to strip it down to just the OBUs
        if (m_codec == ALVR_CODEC_AV1) {
            const uint8_t ivf_magic[4] = { 0x44, 0x4B, 0x49, 0x46 };
            if (len >= 4 && !memcmp(buf, ivf_magic, 4)) {
                buf += 32;
                len -= 32;
            }
            if (len <= 12) {
                continue;
            }
            buf += 12; // skip past the IVF packet size header thing
            len -= 12;
        }

        if (len <= 0) {
            continue;
        }

        if (eye < 0) {
            if (fpOut) {
                fpOut.write(reinterpret_cast<char*>(buf), len);
            }
            ParseFrameNals(m_codec, buf, len, targetTimestampNs, insertIDR);
        } else {
            ParseFrameNalsOfEye(
                m_codec, buf, len, targetTimestampNs, insertIDR, eye, eye == 0 && i == 0,
                eye == 1 && i + 1 == packets.size()
            );
        }
    }
}

void VideoEncoderNVENC::FillEncodeConfig(
    NV_ENC_INITIALIZE_PARAMS& initializeParams,
    int refreshRate,
    int renderWidth,
    int renderHeight,
    uint64_t bitrate_bps,
    NvEncoder* encoder
) {
    auto& encodeConfig = *initializeParams.encodeConfig;

    GUID encoderGUID;
    switch (m_codec) {
    case ALVR_CODEC_H264:
        encoderGUID = NV_ENC_CODEC_H264_GUID;
        break;
    case ALVR_CODEC_HEVC:
        encoderGUID = NV_ENC_CODEC_HEVC_GUID;
        break;
    case ALVR_CODEC_AV1:
        encoderGUID = NV_ENC_CODEC_AV1_GUID;
        break;
    }

    GUID qualityPreset;
    // See recommended NVENC settings for low-latency encoding.
    // https://docs.nvidia.com/video-technologies/video-codec-sdk/nvenc-video-encoder-api-prog-guide/#recommended-nvenc-settings
    switch (Settings::Instance().m_nvencQualityPreset) {
    case 7:
        qualityPreset = NV_ENC_PRESET_P7_GUID;
        break;
    case 6:
        qualityPreset = NV_ENC_PRESET_P6_GUID;
        break;
    case 5:
        qualityPreset = NV_ENC_PRESET_P5_GUID;
        break;
    case 4:
        qualityPreset = NV_ENC_PRESET_P4_GUID;
        break;
    case 3:
        qualityPreset = NV_ENC_PRESET_P3_GUID;
        break;
    case 2:
        qualityPreset = NV_ENC_PRESET_P2_GUID;
        break;
    case 1:
    default:
        qualityPreset = NV_ENC_PRESET_P1_GUID;
        break;
    }

    NV_ENC_TUNING_INFO tuningPreset
        = static_cast<NV_ENC_TUNING_INFO>(Settings::Instance().m_nvencTuningPreset);

    (encoder ? encoder : m_NvNecoder.get())->CreateDefaultEncoderParams(
        &initializeParams, encoderGUID, qualityPreset, tuningPreset
    );

    initializeParams.encodeWidth = initializeParams.darWidth = renderWidth;
    initializeParams.encodeHeight = initializeParams.darHeight = renderHeight;
    initializeParams.frameRateNum = refreshRate;
    initializeParams.frameRateDen = 1;

    if (Settings::Instance().m_nvencRefreshRate != -1) {
        initializeParams.frameRateNum = Settings::Instance().m_nvencRefreshRate;
    }

    initializeParams.enableWeightedPrediction
        = Settings::Instance().m_nvencEnableWeightedPrediction;

    // 16 is recommended when using reference frame invalidation. But it has caused bad visual
    // quality. Now, use 0 (use default).
    uint32_t maxNumRefFrames = 0;
    uint32_t gopLength = NVENC_INFINITE_GOPLENGTH;

    if (Settings::Instance().m_nvencMaxNumRefFrames != -1) {
        maxNumRefFrames = Settings::Instance().m_nvencMaxNumRefFrames;
    }
    if (Settings::Instance().m_nvencGopLength != -1) {
        gopLength = Settings::Instance().m_nvencGopLength;
    }

    switch (m_codec) {
    case ALVR_CODEC_H264: {
        auto& config = encodeConfig.encodeCodecConfig.h264Config;
        config.repeatSPSPPS = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        switch (Settings::Instance().m_entropyCoding) {
        case ALVR_CABAC:
            config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
            break;
        case ALVR_CAVLC:
            config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
            break;
        }

        config.maxNumRefFrames = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_fillerData) {
            config.enableFillerDataInsertion = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.h264VUIParameters.videoSignalTypePresentFlag = 1;
        config.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        config.h264VUIParameters.videoFullRangeFlag
            = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        config.h264VUIParameters.colourDescriptionPresentFlag = 1;
        if (Settings::Instance().m_enableHdr) {
            config.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.h264VUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.h264VUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    } break;
    case ALVR_CODEC_HEVC: {
        auto& config = encodeConfig.encodeCodecConfig.hevcConfig;
        config.repeatSPSPPS = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        config.maxNumRefFramesInDPB = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_use10bitEncoder) {
            encodeConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 2;
        }

        if (Settings::Instance().m_fillerData) {
            config.enableFillerDataInsertion = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.hevcVUIParameters.videoSignalTypePresentFlag = 1;
        config.hevcVUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        config.hevcVUIParameters.videoFullRangeFlag
            = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        config.hevcVUIParameters.colourDescriptionPresentFlag = 1;
        if (Settings::Instance().m_enableHdr) {
            config.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.hevcVUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.hevcVUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    } break;
    case ALVR_CODEC_AV1: {
        auto& config = encodeConfig.encodeCodecConfig.av1Config;
        config.repeatSeqHdr = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        config.maxNumRefFramesInDPB = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_use10bitEncoder) {
            config.pixelBitDepthMinus8 = 2;
        }

        if (Settings::Instance().m_fillerData) {
            config.enableBitstreamPadding = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.chromaFormatIDC = 1; // 4:2:0, 4:4:4 currently not supported
        config.colorRange = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        if (Settings::Instance().m_enableHdr) {
            config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    } break;
    }

    // Disable automatic IDR insertion by NVENC. We need to manually insert IDR when packet is
    // dropped if don't use reference frame invalidation.
    encodeConfig.gopLength = gopLength;
    encodeConfig.frameIntervalP = 1;

    if (Settings::Instance().m_nvencPFrameStrategy != -1) {
        encodeConfig.frameIntervalP = Settings::Instance().m_nvencPFrameStrategy;
    }

    switch (Settings::Instance().m_rateControlMode) {
    case ALVR_CBR:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        break;
    case ALVR_VBR:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        break;
    }
    encodeConfig.rcParams.multiPass
        = static_cast<NV_ENC_MULTI_PASS>(Settings::Instance().m_nvencMultiPass);
    encodeConfig.rcParams.lowDelayKeyFrameScale = 1;

    if (Settings::Instance().m_nvencLowDelayKeyFrameScale != -1) {
        encodeConfig.rcParams.lowDelayKeyFrameScale
            = Settings::Instance().m_nvencLowDelayKeyFrameScale;
    }

    uint32_t maxFrameSize = static_cast<uint32_t>(bitrate_bps / refreshRate);
    Debug("VideoEncoderNVENC: maxFrameSize=%d bits\n", maxFrameSize);
    encodeConfig.rcParams.vbvBufferSize = maxFrameSize * 1.1;
    encodeConfig.rcParams.vbvInitialDelay = maxFrameSize * 1.1;
    encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrate_bps);
    encodeConfig.rcParams.averageBitRate = static_cast<uint32_t>(bitrate_bps);
    if (Settings::Instance().m_nvencAdaptiveQuantizationMode == SpatialAQ) {
        encodeConfig.rcParams.enableAQ = 1;
    } else if (Settings::Instance().m_nvencAdaptiveQuantizationMode == TemporalAQ) {
        encodeConfig.rcParams.enableTemporalAQ = 1;
    }
    if (!m_qpDeltaMap.empty()) {
        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;
    }

    if (Settings::Instance().m_nvencRateControlMode != -1) {
        encodeConfig.rcParams.rateControlMode
            = (NV_ENC_PARAMS_RC_MODE)Settings::Instance().m_nvencRateControlMode;
    }
    if (Settings::Instance().m_nvencRcBufferSize != -1) {
        encodeConfig.rcParams.vbvBufferSize = Settings::Instance().m_nvencRcBufferSize;
    }
    if (Settings::Instance().m_nvencRcInitialDelay != -1) {
        encodeConfig.rcParams.vbvInitialDelay = Settings::Instance().m_nvencRcInitialDelay;
    }
    if (Settings::Instance().m_nvencRcMaxBitrate != -1) {
        encodeConfig.rcParams.maxBitRate = Settings::Instance().m_nvencRcMaxBitrate;
    }
    if (Settings::Instance().m_nvencRcAverageBitrate != -1) {
        encodeConfig.rcParams.averageBitRate = Settings::Instance().m_nvencRcAverageBitrate;
    }
}

void VideoEncoderNVENC::BuildQpDeltaMap(int x0, int width, std::vector<int8_t>& map) {
    map.clear();
    const auto& settings = Settings::Instance();
    if (!settings.m_nvencFoveatedQpMap) {
        return;
    }
    // NVENC takes one value per CTB for HEVC (always 32x32) and per superblock for AV1 (64x64).
    // The settings are in HEVC QP steps (0-51); AV1's quantizer index runs 0-255, so its offsets
    // are scaled by 5 (measured on a capture: -4/+3 gives HEVC +1.6 dB in the center, AV1 with
    // -20/+15 +1.1 dB, with -4/+3 only +0.3 dB).
    int block, scale;
    switch (m_codec) {
    case ALVR_CODEC_HEVC:
        block = 32;
        scale = 1;
        break;
    case ALVR_CODEC_AV1:
        block = 64;
        scale = 5;
        break;
    default:
        Warn("Foveated QP map: only for HEVC and AV1, not used\n");
        return;
    }
    float lo[2], hi[2];
    if (!FFR::GetCenterSpan(lo, hi)) {
        Warn("Foveated QP map: needs foveated encoding, not used\n");
        return;
    }
    const int cols = (width + block - 1) / block;
    const int rows = (m_renderHeight + block - 1) / block;
    const float eyeWidth = m_renderWidth / 2.f;
    // The two centers in pixels, the right one mirrored (TextureToEyeUV).
    const float y0 = lo[1] * m_renderHeight, y1 = hi[1] * m_renderHeight;
    const float centers[2][2] = { { lo[0] * eyeWidth, hi[0] * eyeWidth },
                                  { m_renderWidth - hi[0] * eyeWidth, m_renderWidth - lo[0] * eyeWidth } };
    // The offsets blend over two blocks across the center's border, so no hard step shows.
    const float ramp = 2.f * block;
    const int center = settings.m_nvencCenterQpOffset * scale, edge = settings.m_nvencEdgeQpOffset * scale;
    map.resize(size_t(cols) * rows);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const float px = x0 + (c + 0.5f) * block, py = (r + 0.5f) * block;
            const float(&span)[2] = centers[px < eyeWidth ? 0 : 1];
            // Signed distance to the center rectangle, negative inside.
            const float dx = std::max(span[0] - px, px - span[1]);
            const float dy = std::max(y0 - py, py - y1);
            const float outside = std::hypot(std::max(dx, 0.f), std::max(dy, 0.f));
            const float distance = outside > 0.f ? outside : std::max(dx, dy);
            const float t = std::clamp(0.5f - distance / ramp, 0.f, 1.f);
            map[size_t(r) * cols + c] = (int8_t)std::lround(edge + (center - edge) * t);
        }
    }
    Info(
        "Foveated QP map: %dx%d blocks of %d from x %d, center %+d, edge %+d, center x %.0f-%.0f y %.0f-%.0f\n",
        cols,
        rows,
        block,
        x0,
        center,
        edge,
        centers[0][0],
        centers[0][1],
        y0,
        y1
    );
}
