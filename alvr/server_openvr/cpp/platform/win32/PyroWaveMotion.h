#pragma once

// Motion vectors for the headset's frame interpolation (streamer setting "Send motion vectors").
//
// NVIDIA Optical Flow, the GPU's fixed function optical flow unit (not the shader cores the game
// renders with), compares each frame with the previous one. The vectors go to the headset as
// extra packets of the frame (PyroWaveStripes::MotionHeader), so it can build the in-between of
// the two frames from known motion instead of estimating it on its own GPU.
//
// Runs on the encoder thread: Submit right after the frame was copied for PyroWave (the optical
// flow then runs while PyroWave encodes), Collect between the frame's packet pieces, without
// waiting, and once more, waiting, after the last piece.

#ifdef ALVR_PYROWAVE

#include <d3d11.h>
#include <stdint.h>
#include <memory>
#include <vector>
#include <wrl.h>

#include "nvof/nvOpticalFlowCommon.h"
#include "nvof/nvOpticalFlowD3D11.h"

class PoseHistory;

class PyroWaveMotion {
public:
    ~PyroWaveMotion();

    // False (and logged why) when the GPU or driver cannot do it; the stream then runs as usual.
    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        DXGI_FORMAT frameFormat,
        int width,
        int height,
        int cellPixels
    );

    // Queues the optical flow of this frame against the previous one on the GPU, and the readback
    // of its result. The first frame only becomes the reference.
    void Submit(ID3D11Texture2D* frame, uint64_t timestampNs);

    // Appends the motion packets of the last Submit to buffer and sizes (each at most
    // maxPacketBytes) once the GPU has them; false while they are not there yet (or there are
    // none). With wait, waits up to waitMs for them.
    bool Collect(
        bool wait,
        double waitMs,
        uint16_t stripeCount,
        size_t maxPacketBytes,
        std::vector<uint8_t>& buffer,
        std::vector<uint32_t>& sizes
    );

    // For the log every 5 s.
    void ReportIfDue();

    // The motion choice of the last frame Collect sent vectors for (PyroWaveStripes::
    // MotionChoiceMagic): the same choice as the headset's "Choose Motion per Pixel", made here on
    // the original frames, so the headset spends no GPU time on it. Like Collect: false while not
    // there yet; with wait, waits up to waitMs. Sent with the frame's own timestamp.
    bool CollectChoice(
        bool wait,
        double waitMs,
        uint16_t stripeCount,
        size_t maxPacketBytes,
        std::vector<uint8_t>& buffer,
        std::vector<uint32_t>& sizes
    );

    // Drops a choice that was not ready while its frame was sent.
    void DiscardChoice();

    // What the choice needs besides the frames, as the headset has it: the tracking poses by
    // timestamp (the head rotation between two frames) and the views' tangents (left, right, up,
    // down, all positive) as the headset sent them.
    static void SetPoseHistory(std::shared_ptr<PoseHistory> poseHistory);
    static void SetViewTangents(const float left[4], const float right[4]);

private:
    void Release();

    HMODULE m_library = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST m_api = {};
    NvOFHandle m_handle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    // The optical flow unit takes 8 bit gray (or BGRA) frames under D3D11, SteamVR's are usually
    // RGBA: the frame is copied to m_rgba and a compute shader writes its luma into the input.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_rgba;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_rgbaView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_inputViews[2];
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_toGray;
    // Contrast (standard deviation of the luma) of every cell: flat cells are sent as unreliable.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_inputSrvs[2];
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_cellContrast;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_contrast;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_contrastView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_contrastStaging;

    // The current and the previous frame, in turn.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inputs[2];
    NvOFGPUBufferHandle m_inputHandles[2] = {};
    uint64_t m_inputTimestamps[2] = {};
    int m_current = 0;
    bool m_haveReference = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
    NvOFGPUBufferHandle m_outputHandle = nullptr;
    // Backward flow (reference to input), for the forward-backward check.
    bool m_bidirectional = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backward;
    NvOFGPUBufferHandle m_backwardHandle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backwardStaging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    Microsoft::WRL::ComPtr<ID3D11Query> m_done;

    bool m_pending = false;
    uint64_t m_pendingTimestamp = 0;
    uint64_t m_pendingReference = 0;
    double m_pendingSubmitMs = 0.0;

    int m_width = 0;
    int m_height = 0;
    // Picture pixels per texel of the optical flow input: 2, or 4 with "quarter resolution".
    int m_scale = 2;
    // NVIDIA Optical Flow's own grid (4x4 pixels) and the grid sent (cellPixels).
    int m_flowWidth = 0;
    int m_flowHeight = 0;
    int m_cellPixels = 0;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    std::vector<int16_t> m_grid;

    double m_statStartMs = 0.0;
    uint64_t m_statFrames = 0;
    uint64_t m_statLate = 0;
    double m_statReadyMs = 0.0;
    double m_statReadyMaxMs = 0.0;
    uint64_t m_statBytes = 0;
    uint64_t m_statCells = 0;
    uint64_t m_statFlatCells = 0;
    uint64_t m_statVectors = 0;
    uint64_t m_statConsistentVectors = 0;
    uint64_t m_statInconsistentCells = 0;

    // Motion choice, all on the GPU: the field as the headset makes it from the vectors (head
    // rotation per cell, unreliable cells filled, 3x3 median with the rotation taken out, dead
    // zone), then the choice per block.
    void DispatchChoice();
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_rotationCs;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fillCs;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_medianCs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fieldParams;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_motionUpload;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_motionUploadView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_rotationTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_rotationView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_rotationUav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_filledTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_filledView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_filledUav;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_fieldUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_lutBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_lutView;
    int m_lutSize = 0; // 0 without foveated encoding
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_select;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_selectParams;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearSampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fieldTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fieldView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_choice;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_choiceView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_choiceStaging;
    Microsoft::WRL::ComPtr<ID3D11Query> m_choiceDone;
    bool m_choiceReady = false; // the choice resources exist
    bool m_choicePending = false;
    uint64_t m_choiceTimestamp = 0;
    uint64_t m_choiceReference = 0;
    int m_pendingInput = 0; // m_inputs index of the pending frame (its reference is the other)
    int m_blockWidth = 0;
    int m_blockHeight = 0;
    float m_eyeSizeRatio[2] = { 1.0f, 1.0f };
    std::vector<uint8_t> m_choiceBits;
};

#endif
