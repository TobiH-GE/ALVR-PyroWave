#pragma once

#include "d3d-render-utils/RenderPipeline.h"

class FFR {
public:
    FFR(ID3D11Device* device);
    void Initialize(ID3D11Texture2D* compositionTexture);
    void Render();
    void GetOptimizedResolution(uint32_t* width, uint32_t* height);
    // The foveation as the headset's renderer undoes it (FFR.swift computes the same): the aligned
    // center size and shift, the edge ratio and the eye size ratio, x then y. False when foveated
    // encoding is off (the arrays are then left as they are).
    static bool GetFoveation(float centerSize[2], float centerShift[2], float edgeRatio[2], float eyeSizeRatio[2]);
    ID3D11Texture2D* GetOutputTexture();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mOptimizedTexture;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mQuadVertexShader;

    std::vector<d3d_render_utils::RenderPipeline> mPipelines;
};
