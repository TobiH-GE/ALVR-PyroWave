#pragma once

#include "ALVR-common/packet_types.h"
#include <string>

class Settings {
    static Settings m_Instance;
    bool m_loaded;

    Settings();
    virtual ~Settings();

public:
    void Load();
    static Settings& Instance() { return m_Instance; }

    bool IsLoaded() { return m_loaded; }

    int m_refreshRate;
    uint32_t m_renderWidth;
    uint32_t m_renderHeight;
    int32_t m_recommendedTargetWidth;
    int32_t m_recommendedTargetHeight;
    int32_t m_nAdapterIndex;
    std::string m_captureFrameDir;

    bool m_enableFoveatedEncoding;
    float m_foveationCenterSizeX;
    float m_foveationCenterSizeY;
    float m_foveationCenterShiftX;
    float m_foveationCenterShiftY;
    float m_foveationEdgeRatioX;
    float m_foveationEdgeRatioY;

    bool m_enableColorCorrection;
    float m_brightness;
    float m_contrast;
    float m_saturation;
    float m_gamma;
    float m_sharpening;

    int m_codec;
    int m_h264Profile;
    bool m_use10bitEncoder;
    bool m_useFullRangeEncoding;
    double m_encodingGamma;
    bool m_enableHdr;
    bool m_forceHdrSrgbCorrection;
    bool m_clampHdrExtendedRange;
    bool m_enableAmfPreAnalysis;
    bool m_enableVbaq;
    bool m_enableAmfHmqb;
    bool m_useAmfPreproc;
    uint32_t m_amfPreProcSigma;
    uint32_t m_amfPreProcTor;
    uint32_t m_encoderQualityPreset;
    bool m_amdBitrateCorruptionFix;
    uint32_t m_nvencQualityPreset;
    uint32_t m_rateControlMode;
    bool m_fillerData;
    uint32_t m_entropyCoding;
    bool m_force_sw_encoding;
    uint32_t m_swThreadCount;

    // PyroWave encoder settings (see platform/win32/VideoEncoderPyroWave.h). Only used when
    // m_codec is ALVR_CODEC_PYROWAVE.
    float m_pyroWaveBitsPerPixel;
    uint32_t m_pyroWaveStripeHeight;
    // Largest packet that fits one UDP datagram of the stream socket.
    uint32_t m_pyroWavePacketBytes;
    // Pad packets to m_pyroWavePacketBytes, so the stream socket can batch them (UDP segmentation
    // offload).
    bool m_pyroWavePadPackets;
    bool m_pyroWaveChroma444;
    bool m_pyroWaveIntermediate16Bit;
    bool m_pyroWaveHighPriorityQueue;

    uint32_t m_nvencTuningPreset;
    uint32_t m_nvencMultiPass;
    uint32_t m_nvencAdaptiveQuantizationMode;
    int64_t m_nvencLowDelayKeyFrameScale;
    int64_t m_nvencRefreshRate;
    bool m_nvencEnableIntraRefresh;
    int64_t m_nvencIntraRefreshPeriod;
    int64_t m_nvencIntraRefreshCount;
    int64_t m_nvencMaxNumRefFrames;
    int64_t m_nvencGopLength;
    int64_t m_nvencPFrameStrategy;
    int64_t m_nvencRateControlMode;
    int64_t m_nvencRcBufferSize;
    int64_t m_nvencRcInitialDelay;
    int64_t m_nvencRcMaxBitrate;
    int64_t m_nvencRcAverageBitrate;
    bool m_nvencEnableWeightedPrediction;

    uint64_t m_minimumIdrIntervalMs;

    bool m_enableViveTrackerProxy = false;
    bool m_TrackingRefOnly = false;
    bool m_enableLinuxVulkanAsyncCompute;
    bool m_enableLinuxAsyncReprojection;

    bool m_enableControllers;
    int m_controllerIsTracker = false;
    int m_enableBodyTrackingFakeVive = false;
    int m_bodyTrackingHasLegs = false;
    bool m_useSeparateHandTrackers = false;
};
