#pragma once

#include "NvEncoderD3D11.h"
#include "shared/d3drender.h"
#include <chrono>
#include <functional>
#include <memory>

class VideoEncoder {
public:
    virtual void Initialize() = 0;
    virtual void Shutdown() = 0;

    virtual void Transmit(
        ID3D11Texture2D* pTexture,
        uint64_t presentationTime,
        uint64_t targetTimestampNs,
        bool insertIDR
    ) = 0;

    // When the frame handed to the next Transmit was composed (CEncoder::NewFrameReady), for
    // encoders that report their latency split. Called on the encoder thread before Transmit.
    virtual void SetFrameReadyTime(std::chrono::steady_clock::time_point) { }

    // Called on the present thread right before FrameRender records the composition of the next
    // frame, for encoders that time it on the GPU.
    virtual void BeforeFrameRender() { }

    // Set by CEncoder before the first Transmit.
    void SetInputReleasedCallback(std::function<void()> callback) {
        m_inputReleased = std::move(callback);
    }

protected:
    // An encoder may call this from Transmit once it no longer uses the composed texture or the
    // D3D11 context, so the present thread can compose the next frame while this one is still
    // encoded and sent. CEncoder releases the input after Transmit otherwise.
    void ReleaseInput() {
        if (m_inputReleased) {
            m_inputReleased();
        }
    }

private:
    std::function<void()> m_inputReleased;
};
