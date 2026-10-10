#ifdef ALVR_PYROWAVE

#include "PyroWaveMotion.h"

#include "FFR.h"
#include "PyroWaveStripes.h"
#include "alvr_server/Logger.h"
#include "alvr_server/PoseHistory.h"
#include "alvr_server/Settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// NVIDIA Optical Flow's own vector grid; the vectors sent are averages over cellPixels.
const int kFlowGrid = 4;
// Cells whose luma varies less than this (standard deviation, 0..1) have no texture to match.
const float kFlatContrast = 2.5f / 255.0f;

// Motion choice: what it needs from outside (SetPoseHistory, SetViewTangents).
std::mutex g_choiceMutex;
std::shared_ptr<PoseHistory> g_poseHistory;
float g_tangents[2][4] = {};
bool g_haveTangents = false;

// Entries per foveation table, as FrameExtrapolationGeometry.swift.
const int kLutSize = 2048;
// Dead zone of the scene motion in picture pixels, and the fixed point iterations inverting the
// field: the headset's FrameExtrapolation.deadzonePixels and warpIterations.
const float kDeadzone = 1.5f;
const int kWarpIterations = 3;
// Picture pixels per block of the choice (FX_SELECT_BLOCK in the headset's ShaderTypes.h).
const int kChoiceBlock = 8;

// FrameExtrapolationGeometry.compressedCoordinate: one axis of the headset renderer's
// decompressAxisAlignedCoord, an undistorted eye coordinate (0..1) to where the foveated frame
// holds it, before the eye size ratio.
float CompressedCoordinate(float e, float centerSize, float centerShift, float edgeRatio) {
    const float c0 = (1 - centerSize) * 0.5f;
    const float c1 = (edgeRatio - 1) * c0 * (centerShift + 1) / edgeRatio;
    const float c2 = (edgeRatio - 1) * centerSize + 1;
    const float loBound = c0 * (centerShift + 1);
    const float hiBound = c0 * (centerShift - 1) + 1;
    float value = 0;
    if (e < loBound) {
        const float loBoundC = c0 * (centerShift + 1) / c2;
        const float a = (c1 + c2 * loBoundC) / loBoundC;
        value = (-a + std::sqrt(a * a + 4 * c2 * (1 - edgeRatio) / (edgeRatio * loBoundC) * e))
            / (2 * c2 * (1 - edgeRatio)) * (edgeRatio * loBoundC);
    } else if (e > hiBound) {
        const float hiBoundC = c0 * (centerShift - 1) / c2 + 1;
        const float b = (c2 - edgeRatio * c1 - 2 * edgeRatio * c2 + c2 * edgeRatio * (1 - hiBoundC) + edgeRatio)
            / (edgeRatio * (1 - hiBoundC));
        const float k = (c2 * edgeRatio - c2) * (c1 - hiBoundC + hiBoundC * c2) / (edgeRatio * (1 - hiBoundC) * (1 - hiBoundC))
            - e * (c2 * edgeRatio - c2) / (edgeRatio * (1 - hiBoundC));
        value = (-b + std::sqrt(b * b - 4 * k)) / (2 * c2 * (edgeRatio - 1)) * (edgeRatio * (1 - hiBoundC));
    } else if (e > loBound && e < hiBound) {
        value = (e - c1) * edgeRatio / c2;
    }
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

// FrameExtrapolationGeometry.foveationTables: x and y undistorted -> compressed, then x and y
// compressed -> undistorted, kLutSize floats each.
std::vector<float> FoveationTables(const float centerSize[2], const float centerShift[2], const float edgeRatio[2]) {
    const int n = kLutSize;
    std::vector<float> tables(4 * size_t(n));
    for (int axis = 0; axis < 2; axis++) {
        const int fine = 8 * n;
        std::vector<float> forward(fine + 1);
        for (int i = 0; i <= fine; i++) {
            forward[i] = CompressedCoordinate(float(i) / fine, centerSize[axis], centerShift[axis], edgeRatio[axis]);
        }
        for (int i = 0; i < n; i++) {
            tables[axis * n + i] = CompressedCoordinate(float(i) / (n - 1), centerSize[axis], centerShift[axis], edgeRatio[axis]);
        }
        int j = 0;
        for (int i = 0; i < n; i++) {
            const float c = float(i) / (n - 1);
            while (j < fine - 1 && forward[j + 1] < c) {
                j++;
            }
            const float span = forward[j + 1] - forward[j];
            const float f = span > 0 ? std::clamp((c - forward[j]) / span, 0.0f, 1.0f) : 0.0f;
            tables[(2 + axis) * n + i] = (float(j) + f) / fine;
        }
    }
    return tables;
}

// fx_lut (FrameExtrapolation.metal): linear lookup in one of the four tables.
float Lut(const std::vector<float>& lut, int table, float x) {
    const float f = std::clamp(x, 0.0f, 1.0f) * float(kLutSize - 1);
    const int i = std::min(int(f), kLutSize - 2);
    const float* t = lut.data() + size_t(table) * kLutSize;
    return t[i] + (t[i + 1] - t[i]) * (f - float(i));
}

// The rotation matrix of a unit quaternion (x, y, z, w), row major.
void QuatToMatrix(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

} // namespace


void PyroWaveMotion::SetPoseHistory(std::shared_ptr<PoseHistory> poseHistory) {
    std::lock_guard<std::mutex> lock(g_choiceMutex);
    g_poseHistory = std::move(poseHistory);
}

void PyroWaveMotion::SetViewTangents(const float left[4], const float right[4]) {
    std::lock_guard<std::mutex> lock(g_choiceMutex);
    std::memcpy(g_tangents[0], left, sizeof(g_tangents[0]));
    std::memcpy(g_tangents[1], right, sizeof(g_tangents[1]));
    g_haveTangents = true;
}

PyroWaveMotion::~PyroWaveMotion() { Release(); }

bool PyroWaveMotion::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    DXGI_FORMAT frameFormat,
    int width,
    int height,
    int cellPixels
) {
    // The optical flow runs on the luma at half resolution (setting "quarter resolution": a quarter):
    // at full resolution it took ~17 ms per frame at 7040x2496 on an RTX 5090 (2026-10-07), longer
    // than the frame takes to send, and the vectors sent are 16 pixel averages anyway.
    m_scale = Settings::Instance().m_pyroWaveMotionQuarterResolution ? 4 : 2;
    // The contrast shader works on cells of 8 texels of the half resolution input.
    if (cellPixels % (kFlowGrid * m_scale) != 0 || cellPixels / m_scale > 8 || width % cellPixels != 0 || (width / 2) % cellPixels != 0) {
        Error(
            "PyroWave motion vectors: %dx%d does not split into %d pixel cells per eye, not sending "
            "them\n",
            width,
            height,
            cellPixels
        );
        return false;
    }
    m_library = LoadLibraryW(L"nvofapi64.dll");
    if (!m_library) {
        Error("PyroWave motion vectors: nvofapi64.dll not found (NVIDIA GPU and driver needed), not "
              "sending them\n");
        return false;
    }
    auto create = reinterpret_cast<NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*)>(
        GetProcAddress(m_library, "NvOFAPICreateInstanceD3D11")
    );
    if (!create || create(NV_OF_API_VERSION, &m_api) != NV_OF_SUCCESS
        || m_api.nvCreateOpticalFlowD3D11(device, context, &m_handle) != NV_OF_SUCCESS) {
        Error("PyroWave motion vectors: NVIDIA Optical Flow is not available, not sending them\n");
        Release();
        return false;
    }

    // The input is the frame's luma, 8 bit gray: under D3D11 the unit takes gray, NV12 or BGRA
    // (measured on an RTX 5090: 61, 103, 87), and SteamVR's frames are usually RGBA.
    uint32_t count = 0;
    m_api.nvOFGetSurfaceFormatCountD3D11(m_handle, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW, &count);
    std::vector<DXGI_FORMAT> formats(count);
    if (count) {
        m_api.nvOFGetSurfaceFormatD3D11(
            m_handle, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW, formats.data()
        );
    }
    if (std::find(formats.begin(), formats.end(), DXGI_FORMAT_R8_UNORM) == formats.end()) {
        std::string list;
        for (DXGI_FORMAT f : formats) {
            list += " " + std::to_string((int)f);
        }
        Error(
            "PyroWave motion vectors: the optical flow unit takes no gray input (takes%s), not "
            "sending them\n",
            list.c_str()
        );
        Release();
        return false;
    }

    // The luma conversion, compiled here so the build needs no shader compiler.
    static const char kToGray[] = R"(
Texture2D<float4> frame : register(t0);
RWTexture2D<unorm float> gray : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint2 p = id.xy * SCALE;
    float3 rgb = float3(0, 0, 0);
    [unroll] for (uint y = 0; y < SCALE; y++) {
        [unroll] for (uint x = 0; x < SCALE; x++) {
            rgb += frame[p + uint2(x, y)].rgb;
        }
    }
    rgb /= SCALE * SCALE;
    gray[id.xy] = dot(rgb, float3(0.2126, 0.7152, 0.0722));
}
)";
    // The standard deviation of the gray input over each cell (kCell texels of the half resolution
    // input). Reads past the edge return 0 in D3D11, which only touches the last row and column.
    static const char kCellContrast[] = R"(
Texture2D<float> gray : register(t0);
RWTexture2D<float> contrast : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint2 base = id.xy * CELL;
    float s = 0, s2 = 0;
    [unroll] for (uint y = 0; y < CELL; y++) {
        [unroll] for (uint x = 0; x < CELL; x++) {
            const float v = gray[base + uint2(x, y)];
            s += v;
            s2 += v * v;
        }
    }
    const float n = CELL * CELL;
    const float mean = s / n;
    contrast[id.xy] = sqrt(max(s2 / n - mean * mean, 0.0));
}
)";
    // The field the choice compares against, made on the GPU as the headset makes it from the
    // vectors (its fxRotationField, fxFillUnreliable and fxMedian): per cell the head rotation
    // motion, unreliable cells filled from their reliable neighbours, then a 3x3 median with the
    // rotation taken out and a dead zone. On the CPU this took 10-15 ms per frame on the encoder
    // thread (2026-10-10), enough to hold up the next frame.
#define ALVR_FIELD_PARAMS                                                                        \
    "cbuffer FieldParams : register(b0) {\n"                                                     \
    "    float4 rot0; float4 rot1; float4 rot2;\n"                                               \
    "    float4 tanL; float4 tanR;\n"                                                            \
    "    float2 frameSize; float2 eyeSizeRatio;\n"                                               \
    "    float cellPixels; int cellsPerEye; int lutSize; float deadzone;\n"                      \
    "};\n"
    static const char kRotation[] = ALVR_FIELD_PARAMS R"(
Buffer<float> lut : register(t0);
RWTexture2D<float2> rotationOut : register(u0);
float Lut(int table, float x) {
    const float f = saturate(x) * (lutSize - 1);
    const int i = min(int(f), lutSize - 2);
    const float a = lut[table * lutSize + i], b = lut[table * lutSize + i + 1];
    return lerp(a, b, f - i);
}
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    rotationOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int eye = int(id.x) / cellsPerEye;
    const int local = int(id.x) - eye * cellsPerEye;
    const float eyeWidth = frameSize.x * 0.5;
    const float2 p = float2(eye * eyeWidth + (local + 0.5) * cellPixels, (id.y + 0.5) * cellPixels);
    const float2 t = p / frameSize;
    // The headset renderer's TextureToEyeUV: the right eye's foveation is mirrored.
    const float2 c = float2(eye == 0 ? t.x * 2.0 : (1.0 - t.x) * 2.0, t.y);
    float2 e = c;
    if (lutSize > 0) {
        const float2 cn = c / eyeSizeRatio;
        e = float2(Lut(2, cn.x), Lut(3, cn.y));
    }
    const float4 tn = eye == 0 ? tanL : tanR;
    const float u = eye == 0 ? e.x : 1.0 - e.x;
    const float3 d = float3(lerp(-tn.x, tn.y, u), lerp(tn.z, -tn.w, e.y), -1.0);
    const float3 o = float3(dot(rot0.xyz, d), dot(rot1.xyz, d), dot(rot2.xyz, d));
    if (o.z > -1e-3) {
        rotationOut[id.xy] = float2(0, 0);
        return;
    }
    const float2 to = o.xy / -o.z;
    const float uOld = (to.x + tn.x) / (tn.x + tn.y);
    const float vOld = (tn.z - to.y) / (tn.z + tn.w);
    const float2 eo = float2(eye == 0 ? uOld : 1.0 - uOld, vOld);
    float2 co = eo;
    if (lutSize > 0) {
        // Outside the frame the tables clamp: the motion saturates at the frame edge.
        co = float2(Lut(0, eo.x), Lut(1, eo.y)) * eyeSizeRatio;
    }
    const float2 tOld = float2(eye == 0 ? co.x * 0.5 : 1.0 - co.x * 0.5, co.y);
    rotationOut[id.xy] = p - tOld * frameSize;
}
)";
    static const char kFill[] = ALVR_FIELD_PARAMS R"(
Texture2D<float4> motion : register(t0);
Texture2D<float2> rotation : register(t1);
RWTexture2D<float2> filled : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    filled.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const float4 own = motion[id.xy];
    if (own.b > 0.5) {
        filled[id.xy] = own.rg;
        return;
    }
    const int eye = int(id.x) / cellsPerEye;
    const int x0 = eye * cellsPerEye, x1 = x0 + cellsPerEye - 1;
    for (int r = 1; r <= 6; r++) {
        float2 sum = float2(0, 0);
        float n = 0;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (max(abs(dx), abs(dy)) != r) continue;
                const int2 q = int2(id.xy) + int2(dx, dy);
                if (q.x < x0 || q.x > x1 || q.y < 0 || q.y >= int(h)) continue;
                const float4 m = motion[q];
                if (m.b > 0.5) {
                    sum += m.rg - rotation[q];
                    n += 1;
                }
            }
        }
        if (n > 0) {
            filled[id.xy] = sum / n + rotation[id.xy];
            return;
        }
    }
    filled[id.xy] = rotation[id.xy];
}
)";
    static const char kMedian[] = ALVR_FIELD_PARAMS R"(
Texture2D<float2> filled : register(t0);
Texture2D<float2> rotation : register(t1);
RWTexture2D<float4> field : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    field.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int eye = int(id.x) / cellsPerEye;
    const int x0 = eye * cellsPerEye, x1 = x0 + cellsPerEye - 1;
    float xs[9], ys[9];
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int2 q = int2(clamp(int(id.x) + dx, x0, x1), clamp(int(id.y) + dy, 0, int(h) - 1));
            const float2 m = filled[q];
            xs[n] = m.x;
            ys[n] = m.y;
            n++;
        }
    }
    // Partial selection sort up to the median.
    for (int i = 0; i <= 4; i++) {
        for (int j = i + 1; j < 9; j++) {
            if (xs[j] < xs[i]) { const float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { const float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    const float2 rot = rotation[id.xy];
    float2 scene = float2(xs[4], ys[4]) - rot;
    if (length(scene) < deadzone) scene = float2(0, 0);
    field[id.xy] = float4(scene, rot);
}
)";
    // The motion choice, as fxSelectMotion (FrameExtrapolation.metal) makes it on the headset, on
    // the half resolution gray inputs of the two frames instead of the decoded frames' lowpass
    // level 0 (the same resolution), with the three candidates the headset uses: the field, head
    // rotation only, none. One thread per kChoiceBlock x kChoiceBlock picture pixels.
    static const char kSelect[] = R"(
cbuffer Params : register(b0) {
    float2 frameSize;
    float cellPixels;
    int cellsPerEye;
    int gridRows;
    int iterations;
    int samplesPerSide;
    int block;
    float2 graySize;
    float grayPerPixel;
    float pad;
};
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
Texture2D<float4> field : register(t2);
SamplerState linearClamp : register(s0);
RWTexture2D<uint> choice : register(u0);
float4 Field(float2 p, int eye) {
    const float eyeWidth = frameSize.x * 0.5;
    const float2 grid = float2(2 * cellsPerEye, gridRows);
    const float gx = clamp((p.x - eye * eyeWidth) / cellPixels, 0.5, cellsPerEye - 0.5) + eye * cellsPerEye;
    const float gy = clamp(p.y / cellPixels, 0.5, grid.y - 0.5);
    return field.SampleLevel(linearClamp, float2(gx, gy) / grid, 0);
}
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    choice.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const float2 y = float2(id.xy) * block + block * 0.5;
    const float eyeWidth = frameSize.x * 0.5;
    const int eye = y.x >= eyeWidth ? 1 : 0;
    const float2 lo = float2(eye * eyeWidth + 0.5, 0.5);
    const float2 hi = float2((eye + 1) * eyeWidth - 0.5, frameSize.y - 0.5);
    const float a = 0.5;
    const float2 rot = Field(y, eye).ba;
    float2 x = y;
    for (int i = 0; i < iterations; i++) x = y + a * Field(x, eye).rg;
    float2 cand[3];
    cand[0] = Field(x, eye).rg + rot;
    cand[1] = rot;
    cand[2] = float2(0, 0);
    float best = 0;
    uint chosen = 0;
    const int step = samplesPerSide >= 3 ? 4 : 8;
    [unroll] for (int k = 0; k < 3; k++) {
        const float2 t = cand[k];
        const float2 xc = clamp(y + a * (t - rot), lo, hi);
        const float2 pc = clamp(xc - t, lo, hi);
        float cost = 0, lmin = 1, lmax = 0;
        for (int oy = -4; oy <= 4; oy += step) {
            for (int ox = -4; ox <= 4; ox += step) {
                const float2 o = float2(ox, oy);
                const float lc = cur.SampleLevel(linearClamp, (xc + o) * grayPerPixel / graySize, 0);
                cost += abs(lc - prev.SampleLevel(linearClamp, (pc + o) * grayPerPixel / graySize, 0));
                lmin = min(lmin, lc);
                lmax = max(lmax, lc);
            }
        }
        // As on the headset: another candidate only where it fits clearly better than the field,
        // and only where the samples show structure.
        if (k == 0 || (lmax - lmin >= 0.03 && cost < best * 0.35 - 0.04)) {
            best = cost;
            chosen = k;
        }
    }
    choice[id.xy] = chosen;
}
)";
    using CompileFn = HRESULT(WINAPI*)(
        LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**
    );
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    // SCALE: picture pixels per gray texel; CELL: gray texels per cell.
    const std::string scaleText = std::to_string(m_scale), cellText = std::to_string(cellPixels / m_scale);
    struct Macro {
        const char* name;
        const char* definition;
    };
    const Macro macros[] = { { "SCALE", scaleText.c_str() }, { "CELL", cellText.c_str() }, { nullptr, nullptr } };
    auto build = [&](const char* source, size_t size, const char* name, ComPtr<ID3D11ComputeShader>& shader) {
        ComPtr<ID3DBlob> code, errors;
        const bool ok = compile
            && SUCCEEDED(compile(source, size, name, macros, nullptr, "main", "cs_5_0", 0, 0, &code, &errors))
            && SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        // The blobs belong to the compiler DLL: they are released here, before it is unloaded.
        return ok;
    };
    const bool shadersOk = build(kToGray, sizeof(kToGray) - 1, "ToGray", m_toGray)
        && build(kCellContrast, sizeof(kCellContrast) - 1, "CellContrast", m_cellContrast);
    // Without it the vectors still go out, only the choice does not.
    const bool selectOk = build(kSelect, sizeof(kSelect) - 1, "SelectMotion", m_select)
        && build(kRotation, sizeof(kRotation) - 1, "RotationField", m_rotationCs)
        && build(kFill, sizeof(kFill) - 1, "FillUnreliable", m_fillCs)
        && build(kMedian, sizeof(kMedian) - 1, "MedianField", m_medianCs);
    if (compiler) {
        FreeLibrary(compiler);
    }
    if (!shadersOk) {
        Error("PyroWave motion vectors: cannot build the luma shaders, not sending them\n");
        Release();
        return false;
    }
    m_api.nvOFGetSurfaceFormatCountD3D11(m_handle, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_MODE_OPTICALFLOW, &count);
    std::vector<DXGI_FORMAT> outputFormats(count);
    if (count) {
        m_api.nvOFGetSurfaceFormatD3D11(
            m_handle, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_MODE_OPTICALFLOW, outputFormats.data()
        );
    }
    if (outputFormats.empty()) {
        Error("PyroWave motion vectors: no optical flow output format, not sending them\n");
        Release();
        return false;
    }

    NV_OF_INIT_PARAMS init = {};
    init.width = (uint32_t)(width / m_scale);
    init.height = (uint32_t)(height / m_scale);
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    // Setting "optical flow level"; slow is the best quality.
    const uint32_t level = Settings::Instance().m_pyroWaveMotionLevel;
    init.perfLevel = level == NV_OF_PERF_LEVEL_FAST     ? NV_OF_PERF_LEVEL_FAST
        : level == NV_OF_PERF_LEVEL_MEDIUM              ? NV_OF_PERF_LEVEL_MEDIUM
                                                        : NV_OF_PERF_LEVEL_SLOW;
    // Both directions, for the forward-backward check in Collect: a vector counts only if the flow
    // back from where it points returns to its start. Repetitive texture such as a line of text
    // matches a few pixels off almost as well, and those vectors fail the check.
    // Setting "check both directions" off: forward only, about half the time.
    const bool bothWanted = Settings::Instance().m_pyroWaveMotionBothDirections;
    init.predDirection = bothWanted ? NV_OF_PRED_DIRECTION_BOTH : NV_OF_PRED_DIRECTION_FORWARD;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    NV_OF_STATUS status = m_api.nvOFInit(m_handle, &init);
    m_bidirectional = bothWanted && status == NV_OF_SUCCESS;
    if (bothWanted && !m_bidirectional) {
        Info("PyroWave motion vectors: no backward flow (%d), sending forward vectors unchecked\n", (int)status);
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        status = m_api.nvOFInit(m_handle, &init);
    }
    if (status != NV_OF_SUCCESS) {
        Error("PyroWave motion vectors: optical flow init failed (%d), not sending them\n", (int)status);
        Release();
        return false;
    }

    m_width = width;
    m_height = height;
    m_flowWidth = (width / m_scale + kFlowGrid - 1) / kFlowGrid;
    m_flowHeight = (height / m_scale + kFlowGrid - 1) / kFlowGrid;
    m_cellPixels = cellPixels;
    m_gridWidth = width / cellPixels;
    m_gridHeight = (height + cellPixels - 1) / cellPixels;
    m_grid.resize(size_t(m_gridWidth) * m_gridHeight * 2);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = frameFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_rgba))
        || FAILED(device->CreateShaderResourceView(m_rgba.Get(), nullptr, &m_rgbaView))) {
        Error("PyroWave motion vectors: cannot create the frame copy, not sending them\n");
        Release();
        return false;
    }
    desc.Width = (UINT)(width / m_scale);
    desc.Height = (UINT)(height / m_scale);
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < 2; i++) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_inputs[i]))
            || FAILED(device->CreateUnorderedAccessView(m_inputs[i].Get(), nullptr, &m_inputViews[i]))
            || FAILED(device->CreateShaderResourceView(m_inputs[i].Get(), nullptr, &m_inputSrvs[i]))
            || m_api.nvOFRegisterResourceD3D11(m_handle, m_inputs[i].Get(), &m_inputHandles[i])
                != NV_OF_SUCCESS) {
            Error("PyroWave motion vectors: cannot create the input textures, not sending them\n");
            Release();
            return false;
        }
    }
    desc.Width = (UINT)m_gridWidth;
    desc.Height = (UINT)m_gridHeight;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_contrast))
        || FAILED(device->CreateUnorderedAccessView(m_contrast.Get(), nullptr, &m_contrastView))) {
        Error("PyroWave motion vectors: cannot create the contrast texture, not sending them\n");
        Release();
        return false;
    }
    {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &m_contrastStaging))) {
            Error("PyroWave motion vectors: cannot create the contrast readback, not sending them\n");
            Release();
            return false;
        }
    }
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.Width = (UINT)m_flowWidth;
    desc.Height = (UINT)m_flowHeight;
    desc.Format = outputFormats[0];
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_output))
        || m_api.nvOFRegisterResourceD3D11(m_handle, m_output.Get(), &m_outputHandle) != NV_OF_SUCCESS
        || (m_bidirectional
            && (FAILED(device->CreateTexture2D(&desc, nullptr, &m_backward))
                || m_api.nvOFRegisterResourceD3D11(m_handle, m_backward.Get(), &m_backwardHandle)
                    != NV_OF_SUCCESS))) {
        Error("PyroWave motion vectors: cannot create the output texture, not sending them\n");
        Release();
        return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query = { D3D11_QUERY_EVENT, 0 };
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_staging))
        || (m_bidirectional && FAILED(device->CreateTexture2D(&desc, nullptr, &m_backwardStaging)))
        || FAILED(device->CreateQuery(&query, &m_done))) {
        Error("PyroWave motion vectors: cannot create the readback, not sending them\n");
        Release();
        return false;
    }
    // Motion choice resources (setting "Send motion choice"): the vectors as uploaded, the field
    // the GPU makes from them, the choice per block and its readback.
    m_blockWidth = width / kChoiceBlock;
    m_blockHeight = height / kChoiceBlock;
    if (Settings::Instance().m_pyroWaveMotionChoice) {
        D3D11_TEXTURE2D_DESC fd = {};
        fd.Width = (UINT)m_gridWidth;
        fd.Height = (UINT)m_gridHeight;
        fd.MipLevels = 1;
        fd.ArraySize = 1;
        fd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        fd.SampleDesc.Count = 1;
        fd.Usage = D3D11_USAGE_DYNAMIC;
        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        fd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        // The vectors as uploaded (motion, reliable), then the GPU's rotation, filled and final field.
        D3D11_TEXTURE2D_DESC gd = fd;
        gd.Usage = D3D11_USAGE_DEFAULT;
        gd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        gd.CPUAccessFlags = 0;
        D3D11_TEXTURE2D_DESC g2 = gd;
        g2.Format = DXGI_FORMAT_R32G32_FLOAT;
        auto makeRw = [&](const D3D11_TEXTURE2D_DESC& desc, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv, ComPtr<ID3D11UnorderedAccessView>& uav) {
            return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex))
                && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))
                && SUCCEEDED(device->CreateUnorderedAccessView(tex.Get(), nullptr, &uav));
        };
        float centerSize[2], centerShift[2], edgeRatio[2];
        std::vector<float> tables(1, 0.0f);
        m_lutSize = 0;
        if (FFR::GetFoveation(centerSize, centerShift, edgeRatio, m_eyeSizeRatio)) {
            tables = FoveationTables(centerSize, centerShift, edgeRatio);
            m_lutSize = kLutSize;
        }
        D3D11_BUFFER_DESC lutDesc = {};
        lutDesc.ByteWidth = UINT(tables.size() * sizeof(float));
        lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
        lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA lutData = { tables.data(), 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC lutView = {};
        lutView.Format = DXGI_FORMAT_R32_FLOAT;
        lutView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        lutView.Buffer.NumElements = UINT(tables.size());
        D3D11_BUFFER_DESC paramsDesc = {};
        paramsDesc.ByteWidth = 112;
        paramsDesc.Usage = D3D11_USAGE_DYNAMIC;
        paramsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        paramsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const bool fieldOk = SUCCEEDED(device->CreateTexture2D(&fd, nullptr, &m_motionUpload))
            && SUCCEEDED(device->CreateShaderResourceView(m_motionUpload.Get(), nullptr, &m_motionUploadView))
            && makeRw(g2, m_rotationTexture, m_rotationView, m_rotationUav)
            && makeRw(g2, m_filledTexture, m_filledView, m_filledUav)
            && makeRw(gd, m_fieldTexture, m_fieldView, m_fieldUav)
            && SUCCEEDED(device->CreateBuffer(&lutDesc, &lutData, &m_lutBuffer))
            && SUCCEEDED(device->CreateShaderResourceView(m_lutBuffer.Get(), &lutView, &m_lutView))
            && SUCCEEDED(device->CreateBuffer(&paramsDesc, nullptr, &m_fieldParams));
        D3D11_TEXTURE2D_DESC cd = fd;
        cd.Width = (UINT)m_blockWidth;
        cd.Height = (UINT)m_blockHeight;
        cd.Format = DXGI_FORMAT_R8_UINT;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        cd.CPUAccessFlags = 0;
        D3D11_TEXTURE2D_DESC sd = cd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 48;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_QUERY_DESC eventQuery = { D3D11_QUERY_EVENT, 0 };
        m_choiceReady = selectOk && fieldOk && width % kChoiceBlock == 0 && height % kChoiceBlock == 0
            && SUCCEEDED(device->CreateTexture2D(&cd, nullptr, &m_choice))
            && SUCCEEDED(device->CreateUnorderedAccessView(m_choice.Get(), nullptr, &m_choiceView))
            && SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &m_choiceStaging))
            && SUCCEEDED(device->CreateSamplerState(&sampler, &m_linearSampler))
            && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m_selectParams))
            && SUCCEEDED(device->CreateQuery(&eventQuery, &m_choiceDone));
        m_choiceBits.assign(size_t((m_blockWidth + 3) / 4) * m_blockHeight, 0);
        if (!m_choiceReady) {
            Warn("PyroWave motion vectors: no motion choice (cannot build its shader or resources)\n");
        }
    } else {
        Info("PyroWave motion vectors: motion choice off (setting \"Send motion choice\"), sending the vectors only\n");
    }
    m_context = context;
    m_statStartMs = NowMs();
    Info(
        "PyroWave motion vectors: optical flow level %s, %s, on the luma at 1/%d resolution\n",
        init.perfLevel == NV_OF_PERF_LEVEL_FAST ? "fast" : init.perfLevel == NV_OF_PERF_LEVEL_MEDIUM ? "medium" : "slow",
        m_bidirectional ? "both directions (checked)" : "forward only",
        m_scale
    );
    Info(
        "PyroWave motion vectors: NVIDIA Optical Flow ready, %dx%d, vectors every %d pixels (%dx%d, "
        "%zu bytes per frame)\n",
        width,
        height,
        cellPixels,
        m_gridWidth,
        m_gridHeight,
        m_grid.size() * sizeof(int16_t)
    );
    return true;
}

void PyroWaveMotion::Submit(ID3D11Texture2D* frame, uint64_t timestampNs) {
    if (!m_handle) {
        return;
    }
    // A frame whose vectors were never collected is dropped: only the newest counts.
    m_pending = false;
    m_context->CopyResource(m_rgba.Get(), frame);
    // Only compute state is touched, and unbound again, so SteamVR's and ALVR's rendering does not
    // see it.
    ID3D11ShaderResourceView* srv = m_rgbaView.Get();
    ID3D11UnorderedAccessView* uav = m_inputViews[m_current].Get();
    m_context->CSSetShader(m_toGray.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &srv);
    m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    m_context->Dispatch((UINT)((m_width / m_scale + 15) / 16), (UINT)((m_height / m_scale + 15) / 16), 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    m_context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    // Contrast of every cell of this frame.
    ID3D11ShaderResourceView* graySrv = m_inputSrvs[m_current].Get();
    ID3D11UnorderedAccessView* contrastUav = m_contrastView.Get();
    m_context->CSSetShader(m_cellContrast.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &graySrv);
    m_context->CSSetUnorderedAccessViews(0, 1, &contrastUav, nullptr);
    m_context->Dispatch((UINT)((m_gridWidth + 7) / 8), (UINT)((m_gridHeight + 7) / 8), 1);
    m_context->CSSetShaderResources(0, 1, &noSrv);
    m_context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    m_context->CSSetShader(nullptr, nullptr, 0);
    m_inputTimestamps[m_current] = timestampNs;
    const int reference = 1 - m_current;
    if (m_haveReference && m_inputTimestamps[reference] < timestampNs) {
        NV_OF_EXECUTE_INPUT_PARAMS in = {};
        in.inputFrame = m_inputHandles[m_current];
        in.referenceFrame = m_inputHandles[reference];
        in.disableTemporalHints = NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
        out.outputBuffer = m_outputHandle;
        out.bwdOutputBuffer = m_bidirectional ? m_backwardHandle : nullptr;
        if (m_api.nvOFExecute(m_handle, &in, &out) == NV_OF_SUCCESS) {
            m_context->CopyResource(m_staging.Get(), m_output.Get());
            if (m_bidirectional) {
                m_context->CopyResource(m_backwardStaging.Get(), m_backward.Get());
            }
            m_context->CopyResource(m_contrastStaging.Get(), m_contrast.Get());
            m_context->End(m_done.Get());
            m_pending = true;
            m_pendingTimestamp = timestampNs;
            m_pendingInput = m_current;
            m_pendingReference = m_inputTimestamps[reference];
            m_pendingSubmitMs = NowMs();
        }
    }
    m_haveReference = true;
    m_current = reference;
}

bool PyroWaveMotion::Collect(
    bool wait,
    double waitMs,
    uint16_t stripeCount,
    size_t maxPacketBytes,
    std::vector<uint8_t>& buffer,
    std::vector<uint32_t>& sizes
) {
    if (!m_pending) {
        return false;
    }
    const double startMs = NowMs();
    // The device is multithread protected, so this thread may poll while the present thread
    // records the next frame.
    while (m_context->GetData(m_done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        if (!wait || NowMs() - startMs > waitMs) {
            if (wait) {
                m_statLate++;
                m_pending = false;
            }
            return false;
        }
        Sleep(0);
    }
    m_pending = false;
    const double readyMs = NowMs() - m_pendingSubmitMs;
    m_statReadyMs += readyMs;
    m_statReadyMaxMs = std::max(m_statReadyMaxMs, readyMs);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE backward = {};
    const bool check = m_bidirectional
        && SUCCEEDED(m_context->Map(m_backwardStaging.Get(), 0, D3D11_MAP_READ, 0, &backward));
    auto vectorAt = [](const D3D11_MAPPED_SUBRESOURCE& m, int x, int y) {
        return reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch)
            + 2 * x;
    };
    // Average the consistent 4x4 pixel vectors of each cell (S10.5 stays S10.5). A vector is
    // consistent when the backward flow where it points comes back to within a pixel (or 15 % of
    // its length) of its start.
    const int k = m_cellPixels / (kFlowGrid * m_scale);
    for (int gy = 0; gy < m_gridHeight; gy++) {
        for (int gx = 0; gx < m_gridWidth; gx++) {
            int32_t sx = 0, sy = 0, n = 0;
            for (int y = gy * k; y < std::min((gy + 1) * k, m_flowHeight); y++) {
                for (int x = gx * k; x < (gx + 1) * k; x++) {
                    const int16_t* f = vectorAt(mapped, x, y);
                    if (check) {
                        const float fx = f[0] / 32.0f, fy = f[1] / 32.0f;
                        const int bx = std::clamp(x + (int)std::lround(fx / kFlowGrid), 0, m_flowWidth - 1);
                        const int by = std::clamp(y + (int)std::lround(fy / kFlowGrid), 0, m_flowHeight - 1);
                        const int16_t* b = vectorAt(backward, bx, by);
                        const float ex = fx + b[0] / 32.0f, ey = fy + b[1] / 32.0f;
                        m_statVectors++;
                        if (std::sqrt(ex * ex + ey * ey) > std::max(1.0f, 0.15f * std::sqrt(fx * fx + fy * fy))) {
                            continue;
                        }
                    }
                    sx += f[0];
                    sy += f[1];
                    n++;
                }
            }
            const size_t i = 2 * (size_t(gy) * m_gridWidth + gx);
            if (n == 0) {
                m_grid[i] = PyroWaveStripes::MotionUnreliable;
                m_grid[i + 1] = 0;
                m_statInconsistentCells++;
                continue;
            }
            // Back to full resolution pixels, still S10.5 (x never reaches MotionUnreliable).
            m_grid[i] = int16_t(std::clamp(sx * m_scale / n, -32767, 32767));
            m_grid[i + 1] = int16_t(std::clamp(sy * m_scale / n, -32767, 32767));
            m_statConsistentVectors += n;
        }
    }
    m_context->Unmap(m_staging.Get(), 0);
    if (check) {
        m_context->Unmap(m_backwardStaging.Get(), 0);
    }

    // Flat cells: optical flow finds no unique match there, and its vectors would drag the
    // picture around them (seen on SteamVR menu backgrounds, 2026-10-07).
    if (SUCCEEDED(m_context->Map(m_contrastStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        for (int gy = 0; gy < m_gridHeight; gy++) {
            const float* row = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(mapped.pData) + size_t(gy) * mapped.RowPitch
            );
            for (int gx = 0; gx < m_gridWidth; gx++) {
                int16_t* cell = &m_grid[2 * (size_t(gy) * m_gridWidth + gx)];
                if (row[gx] < kFlatContrast && cell[0] != PyroWaveStripes::MotionUnreliable) {
                    cell[0] = PyroWaveStripes::MotionUnreliable;
                    cell[1] = 0;
                    m_statFlatCells++;
                }
            }
        }
        m_context->Unmap(m_contrastStaging.Get(), 0);
    }
    m_statCells += uint64_t(m_gridWidth) * m_gridHeight;

    // Whole rows per packet.
    const size_t rowBytes = size_t(m_gridWidth) * 2 * sizeof(int16_t);
    const size_t overhead = sizeof(PyroWaveStripes::PacketPrefix) + sizeof(PyroWaveStripes::MotionHeader);
    const int rowsPerPacket = (int)std::max<size_t>(1, (maxPacketBytes - overhead) / rowBytes);
    const uint32_t packetCount = (uint32_t)((m_gridHeight + rowsPerPacket - 1) / rowsPerPacket);
    for (uint32_t p = 0; p < packetCount; p++) {
        const int firstRow = (int)p * rowsPerPacket;
        const int rows = std::min(rowsPerPacket, m_gridHeight - firstRow);
        PyroWaveStripes::PacketPrefix prefix = {};
        prefix.stripe = PyroWaveStripes::MotionStripe;
        prefix.stripe_count = stripeCount;
        prefix.packet_index = p;
        prefix.packet_count = packetCount;
        prefix.data_bytes = (uint32_t)(sizeof(PyroWaveStripes::MotionHeader) + rows * rowBytes);
        PyroWaveStripes::MotionHeader header = {};
        header.magic = PyroWaveStripes::MotionMagic;
        header.version = PyroWaveStripes::MotionVersion;
        header.reference_timestamp_ns = m_pendingReference;
        header.grid_width = (uint16_t)m_gridWidth;
        header.grid_height = (uint16_t)m_gridHeight;
        header.cell_pixels = (uint16_t)m_cellPixels;
        header.first_row = (uint16_t)firstRow;
        header.row_count = (uint16_t)rows;
        const size_t start = buffer.size();
        buffer.resize(start + overhead + rows * rowBytes);
        memcpy(buffer.data() + start, &prefix, sizeof(prefix));
        memcpy(buffer.data() + start + sizeof(prefix), &header, sizeof(header));
        memcpy(buffer.data() + start + overhead, m_grid.data() + size_t(firstRow) * m_gridWidth * 2, rows * rowBytes);
        sizes.push_back((uint32_t)(overhead + rows * rowBytes));
        m_statBytes += overhead + rows * rowBytes;
    }
    if (m_choiceReady) {
        DispatchChoice();
    }
    m_statFrames++;
    return true;
}

void PyroWaveMotion::DispatchChoice() {
    // Head rotation between the two frames, from the poses the headset sent for them (what its
    // FramePoseHistory holds): older^-1 * newer takes directions in the newer frame's eye space into
    // the older frame's. Identity without poses.
    std::shared_ptr<PoseHistory> poses;
    float tangents[2][4];
    bool haveTangents;
    {
        std::lock_guard<std::mutex> lock(g_choiceMutex);
        poses = g_poseHistory;
        std::memcpy(tangents, g_tangents, sizeof(tangents));
        haveTangents = g_haveTangents;
    }
    if (!haveTangents) {
        return;
    }
    float r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const auto older = poses ? poses->GetPoseAt(m_pendingReference) : std::nullopt;
    const auto newer = poses ? poses->GetPoseAt(m_pendingTimestamp) : std::nullopt;
    if (older && newer) {
        const FfiQuat& a = older->motion.orientation;
        const FfiQuat& b = newer->motion.orientation;
        const float ax = -a.x, ay = -a.y, az = -a.z, aw = a.w;
        const float q[4] = {
            aw * b.x + ax * b.w + ay * b.z - az * b.y,
            aw * b.y - ax * b.z + ay * b.w + az * b.x,
            aw * b.z + ax * b.y - ay * b.x + az * b.w,
            aw * b.w - ax * b.x - ay * b.y - az * b.z,
        };
        QuatToMatrix(q, r);
    }

    // The vectors as the headset reads them: motion is their negative, b = 1 where reliable.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_motionUpload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    for (int gy = 0; gy < m_gridHeight; gy++) {
        float* row = reinterpret_cast<float*>(static_cast<uint8_t*>(mapped.pData) + size_t(gy) * mapped.RowPitch);
        const int16_t* cells = m_grid.data() + 2 * size_t(gy) * m_gridWidth;
        for (int gx = 0; gx < m_gridWidth; gx++) {
            const bool reliable = cells[2 * gx] != PyroWaveStripes::MotionUnreliable;
            row[4 * gx] = reliable ? -cells[2 * gx] / 32.0f : 0.0f;
            row[4 * gx + 1] = reliable ? -cells[2 * gx + 1] / 32.0f : 0.0f;
            row[4 * gx + 2] = reliable ? 1.0f : 0.0f;
            row[4 * gx + 3] = 0.0f;
        }
    }
    m_context->Unmap(m_motionUpload.Get(), 0);

    struct FieldParams {
        float rot0[4], rot1[4], rot2[4];
        float tanL[4], tanR[4];
        float frameSize[2], eyeSizeRatio[2];
        float cellPixels;
        int cellsPerEye;
        int lutSize;
        float deadzone;
    };
    static_assert(sizeof(FieldParams) == 112, "FieldParams layout");
    if (FAILED(m_context->Map(m_fieldParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    FieldParams fp = {};
    for (int i = 0; i < 3; i++) {
        float* row = i == 0 ? fp.rot0 : i == 1 ? fp.rot1 : fp.rot2;
        row[0] = r[3 * i];
        row[1] = r[3 * i + 1];
        row[2] = r[3 * i + 2];
    }
    std::memcpy(fp.tanL, tangents[0], sizeof(fp.tanL));
    std::memcpy(fp.tanR, tangents[1], sizeof(fp.tanR));
    fp.frameSize[0] = float(m_width);
    fp.frameSize[1] = float(m_height);
    fp.eyeSizeRatio[0] = m_eyeSizeRatio[0];
    fp.eyeSizeRatio[1] = m_eyeSizeRatio[1];
    fp.cellPixels = float(m_cellPixels);
    fp.cellsPerEye = m_gridWidth / 2;
    fp.lutSize = m_lutSize;
    fp.deadzone = kDeadzone;
    std::memcpy(mapped.pData, &fp, sizeof(fp));
    m_context->Unmap(m_fieldParams.Get(), 0);

    struct SelectParams {
        float frameSize[2];
        float cellPixels;
        int cellsPerEye;
        int gridRows;
        int iterations;
        int samplesPerSide;
        int block;
        float graySize[2];
        float pad[2];
    };
    static_assert(sizeof(SelectParams) == 48, "SelectParams layout");
    if (FAILED(m_context->Map(m_selectParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    // 3x3 points per candidate, as the choice was tuned offline (the headset takes 2x2 to save GPU).
    const SelectParams sp = { { float(m_width), float(m_height) },
                              float(m_cellPixels),
                              m_gridWidth / 2,
                              m_gridHeight,
                              kWarpIterations,
                              3,
                              kChoiceBlock,
                              { float(m_width / m_scale), float(m_height / m_scale) },
                              { 1.0f / m_scale, 0 } };
    std::memcpy(mapped.pData, &sp, sizeof(sp));
    m_context->Unmap(m_selectParams.Get(), 0);

    // Only compute state is touched, and unbound after each pass (as in Submit), so SteamVR's and
    // ALVR's rendering does not see it and no resource is bound for reading and writing at once.
    ID3D11ShaderResourceView* noSrvs[3] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    auto pass = [&](ID3D11ComputeShader* shader, ID3D11Buffer* cb, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                    ID3D11UnorderedAccessView* uav, UINT groupsX, UINT groupsY) {
        std::vector<ID3D11ShaderResourceView*> views(srvs);
        m_context->CSSetShader(shader, nullptr, 0);
        m_context->CSSetConstantBuffers(0, 1, &cb);
        m_context->CSSetShaderResources(0, (UINT)views.size(), views.data());
        m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        m_context->Dispatch(groupsX, groupsY, 1);
        m_context->CSSetShaderResources(0, 3, noSrvs);
        m_context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    };
    const UINT cellGroupsX = (UINT)((m_gridWidth + 15) / 16), cellGroupsY = (UINT)((m_gridHeight + 7) / 8);
    pass(m_rotationCs.Get(), m_fieldParams.Get(), { m_lutView.Get() }, m_rotationUav.Get(), cellGroupsX, cellGroupsY);
    pass(m_fillCs.Get(), m_fieldParams.Get(), { m_motionUploadView.Get(), m_rotationView.Get() }, m_filledUav.Get(),
         cellGroupsX, cellGroupsY);
    pass(m_medianCs.Get(), m_fieldParams.Get(), { m_filledView.Get(), m_rotationView.Get() }, m_fieldUav.Get(),
         cellGroupsX, cellGroupsY);
    ID3D11SamplerState* sampler = m_linearSampler.Get();
    m_context->CSSetSamplers(0, 1, &sampler);
    pass(m_select.Get(), m_selectParams.Get(),
         { m_inputSrvs[m_pendingInput].Get(), m_inputSrvs[1 - m_pendingInput].Get(), m_fieldView.Get() },
         m_choiceView.Get(), (UINT)((m_blockWidth + 15) / 16), (UINT)((m_blockHeight + 7) / 8));
    ID3D11SamplerState* noSampler = nullptr;
    ID3D11Buffer* noCb = nullptr;
    m_context->CSSetSamplers(0, 1, &noSampler);
    m_context->CSSetConstantBuffers(0, 1, &noCb);
    m_context->CSSetShader(nullptr, nullptr, 0);
    m_context->CopyResource(m_choiceStaging.Get(), m_choice.Get());
    m_context->End(m_choiceDone.Get());
    // CollectChoice polls without flushing.
    m_context->Flush();
    m_choicePending = true;
    m_choiceTimestamp = m_pendingTimestamp;
    m_choiceReference = m_pendingReference;
}

void PyroWaveMotion::DiscardChoice() {
    if (m_choicePending) {
        m_choicePending = false;
    }
}

bool PyroWaveMotion::CollectChoice(
    bool wait,
    double waitMs,
    uint16_t stripeCount,
    size_t maxPacketBytes,
    std::vector<uint8_t>& buffer,
    std::vector<uint32_t>& sizes
) {
    if (!m_choicePending) {
        return false;
    }
    const double startMs = NowMs();
    while (m_context->GetData(m_choiceDone.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        if (!wait || NowMs() - startMs > waitMs) {
            if (wait) {
                m_choicePending = false;
            }
            return false;
        }
        Sleep(0);
    }
    m_choicePending = false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_choiceStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const size_t rowBytes = size_t(m_blockWidth + 3) / 4;
    std::fill(m_choiceBits.begin(), m_choiceBits.end(), uint8_t(0));
    for (int by = 0; by < m_blockHeight; by++) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(by) * mapped.RowPitch;
        uint8_t* out = m_choiceBits.data() + size_t(by) * rowBytes;
        for (int bx = 0; bx < m_blockWidth; bx++) {
            const uint8_t c = std::min<uint8_t>(row[bx], 2);
            out[bx / 4] |= uint8_t(c << (2 * (bx % 4)));
        }
    }
    m_context->Unmap(m_choiceStaging.Get(), 0);

    const size_t overhead = sizeof(PyroWaveStripes::PacketPrefix) + sizeof(PyroWaveStripes::MotionHeader);
    const int rowsPerPacket = (int)std::max<size_t>(1, (maxPacketBytes - overhead) / rowBytes);
    const uint32_t packetCount = (uint32_t)((m_blockHeight + rowsPerPacket - 1) / rowsPerPacket);
    for (uint32_t p = 0; p < packetCount; p++) {
        const int firstRow = (int)p * rowsPerPacket;
        const int rows = std::min(rowsPerPacket, m_blockHeight - firstRow);
        PyroWaveStripes::PacketPrefix prefix = {};
        prefix.stripe = PyroWaveStripes::MotionStripe;
        prefix.stripe_count = stripeCount;
        prefix.packet_index = p;
        prefix.packet_count = packetCount;
        prefix.data_bytes = (uint32_t)(sizeof(PyroWaveStripes::MotionHeader) + rows * rowBytes);
        PyroWaveStripes::MotionHeader header = {};
        header.magic = PyroWaveStripes::MotionChoiceMagic;
        header.version = PyroWaveStripes::MotionChoiceVersion;
        header.reference_timestamp_ns = m_choiceReference;
        header.grid_width = (uint16_t)m_blockWidth;
        header.grid_height = (uint16_t)m_blockHeight;
        header.cell_pixels = (uint16_t)kChoiceBlock;
        header.first_row = (uint16_t)firstRow;
        header.row_count = (uint16_t)rows;
        const size_t start = buffer.size();
        buffer.resize(start + overhead + rows * rowBytes);
        memcpy(buffer.data() + start, &prefix, sizeof(prefix));
        memcpy(buffer.data() + start + sizeof(prefix), &header, sizeof(header));
        memcpy(buffer.data() + start + overhead, m_choiceBits.data() + size_t(firstRow) * rowBytes, rows * rowBytes);
        sizes.push_back((uint32_t)(overhead + rows * rowBytes));
        m_statBytes += overhead + rows * rowBytes;
    }
    return true;
}

void PyroWaveMotion::ReportIfDue() {
    if (!m_handle) {
        return;
    }
    const double now = NowMs();
    if (now - m_statStartMs < 5000.0) {
        return;
    }
    const double frames = (double)std::max<uint64_t>(m_statFrames, 1);
    Info(
        "PyroWave motion vectors: %llu frames in %.1f s, ready %.2f ms avg %.2f max after the copy, "
        "%llu too late, %.0f KB per frame; cells sent as unreliable: %.0f %% flat, %.0f %% without a "
        "vector passing the forward-backward check (%.0f %% of all vectors passed)\n",
        (unsigned long long)m_statFrames,
        (now - m_statStartMs) / 1000.0,
        m_statReadyMs / frames,
        m_statReadyMaxMs,
        (unsigned long long)m_statLate,
        m_statBytes / frames / 1024.0,
        m_statCells ? 100.0 * double(m_statFlatCells) / double(m_statCells) : 0.0,
        m_statCells ? 100.0 * double(m_statInconsistentCells) / double(m_statCells) : 0.0,
        m_statVectors ? 100.0 * double(m_statConsistentVectors) / double(m_statVectors) : 100.0
    );
    m_statStartMs = now;
    m_statFrames = 0;
    m_statLate = 0;
    m_statReadyMs = 0.0;
    m_statReadyMaxMs = 0.0;
    m_statBytes = 0;
    m_statCells = 0;
    m_statFlatCells = 0;
    m_statVectors = 0;
    m_statConsistentVectors = 0;
    m_statInconsistentCells = 0;
}

void PyroWaveMotion::Release() {
    if (m_handle) {
        for (auto& h : m_inputHandles) {
            if (h) {
                m_api.nvOFUnregisterResourceD3D11(h);
                h = nullptr;
            }
        }
        if (m_outputHandle) {
            m_api.nvOFUnregisterResourceD3D11(m_outputHandle);
            m_outputHandle = nullptr;
        }
        if (m_backwardHandle) {
            m_api.nvOFUnregisterResourceD3D11(m_backwardHandle);
            m_backwardHandle = nullptr;
        }
        m_api.nvOFDestroy(m_handle);
        m_handle = nullptr;
    }
    for (auto& t : m_inputs) {
        t.Reset();
    }
    for (auto& v : m_inputViews) {
        v.Reset();
    }
    m_rgbaView.Reset();
    m_rgba.Reset();
    m_toGray.Reset();
    m_select.Reset();
    m_selectParams.Reset();
    m_linearSampler.Reset();
    m_fieldView.Reset();
    m_fieldTexture.Reset();
    m_choiceView.Reset();
    m_choice.Reset();
    m_choiceStaging.Reset();
    m_choiceDone.Reset();
    m_rotationCs.Reset();
    m_fillCs.Reset();
    m_medianCs.Reset();
    m_motionUploadView.Reset();
    m_motionUpload.Reset();
    m_rotationUav.Reset();
    m_rotationView.Reset();
    m_rotationTexture.Reset();
    m_filledUav.Reset();
    m_filledView.Reset();
    m_filledTexture.Reset();
    m_fieldUav.Reset();
    m_lutView.Reset();
    m_lutBuffer.Reset();
    m_fieldParams.Reset();
    m_choiceReady = false;
    m_choicePending = false;
    m_cellContrast.Reset();
    m_contrastView.Reset();
    m_contrast.Reset();
    m_contrastStaging.Reset();
    for (auto& v : m_inputSrvs) {
        v.Reset();
    }
    m_output.Reset();
    m_backward.Reset();
    m_backwardStaging.Reset();
    m_staging.Reset();
    m_done.Reset();
    m_context.Reset();
    if (m_library) {
        FreeLibrary(m_library);
        m_library = nullptr;
    }
}

#endif
