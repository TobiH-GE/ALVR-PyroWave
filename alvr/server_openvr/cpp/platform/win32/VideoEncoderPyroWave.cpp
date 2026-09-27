#ifdef ALVR_PYROWAVE

#include "VideoEncoderPyroWave.h"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <type_traits>

using Microsoft::WRL::ComPtr;

namespace {

// The DLL name depends on the toolchain it was built with (DLL_NAME_WITH_SOVERSION, lib
// prefix with MinGW). Tried in order, next to the driver DLL.
const wchar_t* const kDllNames[] = {
    L"pyrowave-shared-0.dll",
    L"pyrowave-shared.dll",
    L"libpyrowave-shared-0.dll",
    L"libpyrowave-shared.dll",
};

std::wstring DriverDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&DriverDirectory),
            &self
        )) {
        return L"";
    }
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return L"";
    }
    std::wstring dir(path, len);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
}

// The shared texture's format for a given input format, and its Vulkan equivalent. Only
// formats PyroWave's RGB input path takes (UNORM RGB(A)); sRGB inputs are read as UNORM, i.e.
// as the gamma-encoded values, which is what the client expects.
bool MapInputFormat(DXGI_FORMAT input, DXGI_FORMAT* shared, VkFormat* vk) {
    switch (input) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *shared = DXGI_FORMAT_R8G8B8A8_UNORM;
        *vk = VK_FORMAT_R8G8B8A8_UNORM;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *shared = DXGI_FORMAT_B8G8R8A8_UNORM;
        *vk = VK_FORMAT_B8G8R8A8_UNORM;
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        *shared = DXGI_FORMAT_R10G10B10A2_UNORM;
        *vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        return true;
    default:
        return false;
    }
}

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

double MsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// The phases of one frame, from SteamVR's present to the packets queued for the network. They
// add up to "total", which is what the dashboard shows as Encoding (frame composed -> frame
// encoded, reported at the end of VideoSendPackets).
const char* const kPhaseNames[8] = {
    "wake",          // present thread -> encoder thread
    "copy submit",   // CPU: CopyResource, fence signal, flush
    "encode submit", // CPU: record and submit the Vulkan encode
    "D3D11 GPU",     // until D3D11 finished the composition (FrameRender) and the copy
    "Vulkan GPU",    // then until PyroWave's encode finished on its compute queue
    "packetize",     // CPU: build_packets
    "send",          // CPU: VideoSendPackets (FFI copy, packet vectors, queue to the send thread)
    "total",
};

} // namespace

VideoEncoderPyroWave::VideoEncoderPyroWave(
    std::shared_ptr<CD3DRender> d3dRender, int width, int height
)
    : m_d3dRender(d3dRender)
    , m_width(width)
    , m_height(height)
    , m_chroma(
          Settings::Instance().m_pyroWaveChroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444
                                                   : PYROWAVE_CHROMA_SUBSAMPLING_420
      )
    , m_maxFrameBytes((size_t)((double)width * height * Settings::Instance().m_pyroWaveBitsPerPixel
                               / 8.0)) { }

VideoEncoderPyroWave::~VideoEncoderPyroWave() { Shutdown(); }

void VideoEncoderPyroWave::LoadApi() {
    std::wstring dir = DriverDirectory();
    for (const wchar_t* name : kDllNames) {
        std::wstring path = dir + name;
        // LOAD_WITH_ALTERED_SEARCH_PATH: resolve the DLL's own dependencies from its directory.
        m_api.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m_api.module) {
            Info("PyroWave: loaded %ls\n", path.c_str());
            break;
        }
    }
    if (!m_api.module) {
        throw MakeException(
            "PyroWave: pyrowave-shared DLL not found next to the driver (%ls). See "
            "deps/windows/pyrowave/README.md.",
            dir.c_str()
        );
    }

    bool missing = false;
    auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
            reinterpret_cast<void*>(GetProcAddress(m_api.module, name))
        );
        if (!fn) {
            Error("PyroWave: DLL has no entry point %s\n", name);
            missing = true;
        }
    };
    resolve(m_api.get_api_version, "pyrowave_get_api_version");
    resolve(m_api.create_device_by_compat2, "pyrowave_create_device_by_compat2");
    resolve(m_api.device_get_global_priority, "pyrowave_device_get_global_priority");
    resolve(m_api.device_destroy, "pyrowave_device_destroy");
    resolve(m_api.device_report_performance_stats, "pyrowave_device_report_performance_stats");
    resolve(m_api.sync_object_create, "pyrowave_sync_object_create");
    resolve(m_api.sync_object_get_semaphore, "pyrowave_sync_object_get_semaphore");
    resolve(m_api.sync_object_destroy, "pyrowave_sync_object_destroy");
    resolve(m_api.image_create, "pyrowave_image_create");
    resolve(m_api.image_get_image_view, "pyrowave_image_get_image_view");
    resolve(m_api.image_destroy, "pyrowave_image_destroy");
    resolve(m_api.encoder_create, "pyrowave_encoder_create");
    resolve(
        m_api.encoder_encode_gpu_scaled_synchronous,
        "pyrowave_encoder_encode_gpu_scaled_synchronous"
    );
    resolve(m_api.encoder_get_mapped_raw_bitstream, "pyrowave_encoder_get_mapped_raw_bitstream");
    resolve(m_api.encoder_destroy, "pyrowave_encoder_destroy");
    if (missing) {
        throw MakeException("PyroWave: the DLL does not match the header this was built with");
    }

    // The ABI is not stable before 1.0, so a DLL from another minor version is refused rather
    // than trusted.
    uint32_t major = 0, minor = 0, patch = 0;
    m_api.get_api_version(&major, &minor, &patch);
    if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
        throw MakeException(
            "PyroWave: DLL has API version %u.%u.%u, the driver was built against %u.%u.%u",
            major,
            minor,
            patch,
            PYROWAVE_API_VERSION_MAJOR,
            PYROWAVE_API_VERSION_MINOR,
            PYROWAVE_API_VERSION_PATCH
        );
    }
}

void VideoEncoderPyroWave::Initialize() {
    if (Settings::Instance().m_enableHdr) {
        throw MakeException("PyroWave: HDR is not supported yet, turn it off to use PyroWave");
    }
    if (m_chroma == PYROWAVE_CHROMA_SUBSAMPLING_420 && (m_width % 2 || m_height % 2)) {
        throw MakeException(
            "PyroWave: 4:2:0 needs an even resolution, the stream is %dx%d", m_width, m_height
        );
    }
    if (m_maxFrameBytes < 64 * 1024) {
        throw MakeException(
            "PyroWave: %.2f bits per pixel leaves only %zu bytes per frame",
            Settings::Instance().m_pyroWaveBitsPerPixel,
            m_maxFrameBytes
        );
    }

    LoadApi();

    ID3D11Device* device = m_d3dRender->GetDevice();
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_device5)))
        || FAILED(m_d3dRender->GetContext()->QueryInterface(IID_PPV_ARGS(&m_context4)))) {
        throw MakeException("PyroWave: D3D11.4 is required for shared fences");
    }

    // Vulkan has to run on the GPU that renders the frames; the adapter LUID identifies it.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc = {};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))
        || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&adapterDesc))) {
        throw MakeException("PyroWave: could not identify the D3D11 adapter");
    }
    // The composition (FrameRender) and the copy run on this device's graphics queue, which the
    // game fills with its next frame: measured ~4 ms of waiting there while a GPU-heavy game ran.
    // FrameRender already asks for the realtime scheduling class for the process, which Windows
    // only grants to administrators; the device's own GPU thread priority is the part a normal
    // user may raise.
    {
        HRESULT hr = dxgiDevice->SetGPUThreadPriority(7);
        INT gpuPriority = 0;
        dxgiDevice->GetGPUThreadPriority(&gpuPriority);
        Info(
            "PyroWave: D3D11 GPU thread priority %d%s\n",
            gpuPriority,
            SUCCEEDED(hr) ? "" : " (raising it to 7 failed)"
        );
    }
    static_assert(sizeof(LUID) == sizeof(pyrowave_luid), "LUID size mismatch");
    pyrowave_luid luid;
    memcpy(luid.luid, &adapterDesc.AdapterLuid, sizeof(luid.luid));

    VkQueueGlobalPriority priority = Settings::Instance().m_pyroWaveHighPriorityQueue
        ? VK_QUEUE_GLOBAL_PRIORITY_HIGH
        : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;
    pyrowave_result res
        = m_api.create_device_by_compat2(0, 0, nullptr, nullptr, &luid, priority, &m_device);
    if (res != PYROWAVE_SUCCESS) {
        throw MakeException(
            "PyroWave: no Vulkan device for adapter %ls (error %d)", adapterDesc.Description, res
        );
    }
    VkQueueGlobalPriority granted = m_api.device_get_global_priority(m_device);
    Info(
        "PyroWave: Vulkan device on %ls, queue priority %s\n",
        adapterDesc.Description,
        granted == VK_QUEUE_GLOBAL_PRIORITY_HIGH           ? "high"
            : granted == VK_QUEUE_GLOBAL_PRIORITY_REALTIME ? "realtime"
            : granted == VK_QUEUE_GLOBAL_PRIORITY_LOW      ? "low"
                                                           : "medium"
    );
    if (priority == VK_QUEUE_GLOBAL_PRIORITY_HIGH && granted != priority) {
        Warn("PyroWave: high queue priority was requested but not granted\n");
    }

    pyrowave_encoder_create_info encoderInfo = {};
    encoderInfo.device = m_device;
    encoderInfo.width = m_width;
    encoderInfo.height = m_height;
    encoderInfo.chroma = m_chroma;
    res = m_api.encoder_create(&encoderInfo, &m_encoder);
    if (res != PYROWAVE_SUCCESS) {
        ReleaseResources();
        throw MakeException("PyroWave: encoder_create failed (error %d)", res);
    }

    // Sharing a fence from D3D11 to Vulkan is the well supported direction.
    HANDLE fenceHandle = nullptr;
    if (FAILED(m_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence)))
        || FAILED(m_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        ReleaseResources();
        throw MakeException("PyroWave: could not create a shared D3D11 fence");
    }
    pyrowave_sync_object_create_info syncInfo = {};
    syncInfo.device = m_device;
    // Takes ownership of the handle, also on failure.
    syncInfo.external_handle = (pyrowave_os_handle)fenceHandle;
    // D3D11 fences are D3D12 fences on Windows 10 and later.
    syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    res = m_api.sync_object_create(&syncInfo, &m_sync);
    if (res != PYROWAVE_SUCCESS) {
        ReleaseResources();
        throw MakeException("PyroWave: could not import the D3D11 fence (error %d)", res);
    }
    m_copyDoneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_copyDoneEvent) {
        ReleaseResources();
        throw MakeException("PyroWave: could not create an event");
    }
    CreateGpuTimers();

    m_statStart = std::chrono::steady_clock::now();

    const bool chroma444 = m_chroma == PYROWAVE_CHROMA_SUBSAMPLING_444;
    if (!m_geometry.init(
            m_width, m_height, chroma444, (int)Settings::Instance().m_pyroWaveStripeHeight
        )) {
        ReleaseResources();
        throw MakeException(
            "PyroWave: stripe height %u is not a multiple of 32",
            Settings::Instance().m_pyroWaveStripeHeight
        );
    }
    m_stripeBlocks = m_geometry.stripe_blocks();

    m_packetBytes = Settings::Instance().m_pyroWavePacketBytes;
    const size_t overhead = sizeof(PyroWaveStripes::PacketPrefix) + 2 * sizeof(uint32_t);
    if (m_packetBytes < overhead + 256) {
        ReleaseResources();
        throw MakeException(
            "PyroWave: a packet size of %zu bytes leaves no room for data, raise the packet size",
            m_packetBytes
        );
    }
    m_sendBuffer.reserve(m_maxFrameBytes * 2 + 64 * 1024);

    Info(
        "PyroWave: encoder ready, %dx%d %s, at most %zu bytes per frame (%.2f bpp), %d stripes of "
        "%d rows, packets of up to %zu bytes\n",
        m_width,
        m_height,
        chroma444 ? "4:4:4" : "4:2:0",
        m_maxFrameBytes,
        Settings::Instance().m_pyroWaveBitsPerPixel,
        m_geometry.stripe_count(),
        m_geometry.stripe_height,
        m_packetBytes
    );
}

void VideoEncoderPyroWave::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& inputDesc) {
    DXGI_FORMAT sharedFormat;
    VkFormat vkFormat;
    if (!MapInputFormat(inputDesc.Format, &sharedFormat, &vkFormat)) {
        throw MakeException(
            "PyroWave: frame texture format %d is not supported", (int)inputDesc.Format
        );
    }
    if ((int)inputDesc.Width != m_width || (int)inputDesc.Height != m_height) {
        // PyroWave's input stage scales, so this still works, but it is not what anyone wants.
        Warn(
            "PyroWave: frame is %ux%u, the encoder %dx%d; frames will be rescaled\n",
            inputDesc.Width,
            inputDesc.Height,
            m_width,
            m_height
        );
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = inputDesc.Width;
    desc.Height = inputDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = sharedFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(m_device5->CreateTexture2D(&desc, nullptr, &m_sharedTexture))) {
        throw MakeException("PyroWave: could not create the shared texture");
    }

    ComPtr<IDXGIResource1> dxgiResource;
    HANDLE textureHandle = nullptr;
    if (FAILED(m_sharedTexture.As(&dxgiResource))
        || FAILED(dxgiResource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &textureHandle
        ))) {
        m_sharedTexture.Reset();
        throw MakeException("PyroWave: could not share the texture");
    }

    VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = vkFormat;
    imageCreateInfo.extent = { desc.Width, desc.Height, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    pyrowave_image_create_info info = {};
    info.device = m_device;
    // Takes ownership of the handle, also on failure.
    info.external_handle = (pyrowave_os_handle)textureHandle;
    info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    info.image_create_info = &imageCreateInfo;
    pyrowave_result res = m_api.image_create(&info, &m_image);
    if (res != PYROWAVE_SUCCESS) {
        m_sharedTexture.Reset();
        throw MakeException("PyroWave: could not import the shared texture (error %d)", res);
    }

    res = m_api.image_get_image_view(
        m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &m_imageView
    );
    if (res != PYROWAVE_SUCCESS) {
        m_api.image_destroy(m_image);
        m_image = nullptr;
        m_sharedTexture.Reset();
        throw MakeException("PyroWave: no image view for the shared texture (error %d)", res);
    }
}

void VideoEncoderPyroWave::CreateGpuTimers() {
    D3D11_QUERY_DESC disjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC timestampDesc = { D3D11_QUERY_TIMESTAMP, 0 };
    for (GpuTimer& timer : m_gpuTimers) {
        if (FAILED(m_device5->CreateQuery(&disjointDesc, &timer.disjoint))
            || FAILED(m_device5->CreateQuery(&timestampDesc, &timer.composeStart))
            || FAILED(m_device5->CreateQuery(&timestampDesc, &timer.copyStart))
            || FAILED(m_device5->CreateQuery(&timestampDesc, &timer.copyEnd))) {
            // Only for the log; the encoder works without them.
            Warn("PyroWave: no D3D11 timestamp queries, the composition is not timed on the GPU\n");
            for (GpuTimer& t : m_gpuTimers) {
                t = GpuTimer();
            }
            return;
        }
    }
}

void VideoEncoderPyroWave::BeforeFrameRender() {
    if (m_failed || !m_gpuTimers[0].disjoint) {
        return;
    }
    ID3D11DeviceContext* context = m_d3dRender->GetContext();
    if (m_gpuTimerActive >= 0) {
        // The last frame never reached Transmit: close its measurement without using it.
        GpuTimer& stale = m_gpuTimers[m_gpuTimerActive];
        context->End(stale.copyStart.Get());
        context->End(stale.copyEnd.Get());
        context->End(stale.disjoint.Get());
        stale.pending = true;
        stale.valid = false;
        m_gpuTimerActive = -1;
    }
    GpuTimer& timer = m_gpuTimers[m_gpuTimerNext];
    if (timer.pending) {
        // Its results are not in yet; this frame goes untimed.
        return;
    }
    context->Begin(timer.disjoint.Get());
    context->End(timer.composeStart.Get());
    m_gpuTimerActive = m_gpuTimerNext;
    m_gpuTimerNext = (m_gpuTimerNext + 1) % kGpuTimerCount;
}

void VideoEncoderPyroWave::CollectGpuTimers() {
    ID3D11DeviceContext* context = m_d3dRender->GetContext();
    for (GpuTimer& timer : m_gpuTimers) {
        if (!timer.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        UINT64 composeStart = 0, copyStart = 0, copyEnd = 0;
        const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        if (context->GetData(timer.disjoint.Get(), &disjoint, sizeof(disjoint), flags) != S_OK
            || context->GetData(timer.composeStart.Get(), &composeStart, sizeof(UINT64), flags)
                != S_OK
            || context->GetData(timer.copyStart.Get(), &copyStart, sizeof(UINT64), flags) != S_OK
            || context->GetData(timer.copyEnd.Get(), &copyEnd, sizeof(UINT64), flags) != S_OK) {
            continue;
        }
        timer.pending = false;
        if (!timer.valid || disjoint.Disjoint || disjoint.Frequency == 0 || copyStart < composeStart
            || copyEnd < copyStart) {
            continue;
        }
        const double toMs = 1000.0 / (double)disjoint.Frequency;
        const double composeMs = (double)(copyStart - composeStart) * toMs;
        m_statGpuTimed++;
        m_statComposeGpuMs += composeMs;
        m_statComposeGpuMaxMs = std::max(m_statComposeGpuMaxMs, composeMs);
        m_statCopyGpuMs += (double)(copyEnd - copyStart) * toMs;
    }
}

void VideoEncoderPyroWave::SendStreamConfig() {
    PyroWaveStripes::StreamConfig config = {};
    config.magic = PyroWaveStripes::StreamConfigMagic;
    config.version = PyroWaveStripes::StreamConfigVersion;
    config.width = (uint32_t)m_width;
    config.height = (uint32_t)m_height;
    config.chroma = (uint32_t)m_chroma;
    config.color = 0;
    config.stripe_height = (uint32_t)m_geometry.stripe_height;
    config.max_frame_bytes = (uint32_t)m_maxFrameBytes;
    SetVideoConfigNals(
        reinterpret_cast<const unsigned char*>(&config), sizeof(config), ALVR_CODEC_PYROWAVE
    );
}

bool VideoEncoderPyroWave::BuildPackets(
    const void* mappedBitstream, size_t bitstreamBytes, const void* mappedMeta, size_t metaBytes
) {
    const char* problem = PyroWaveStripes::build_packets(
        m_geometry,
        m_stripeBlocks,
        static_cast<const uint32_t*>(mappedBitstream),
        bitstreamBytes / sizeof(uint32_t),
        static_cast<const PyroWaveStripes::BlockMeta*>(mappedMeta),
        metaBytes / sizeof(PyroWaveStripes::BlockMeta),
        m_packetBytes,
        Settings::Instance().m_pyroWavePadPackets,
        m_sendBuffer,
        m_packetSizes
    );
    if (problem) {
        Error("PyroWave: cannot packetize the frame: %s\n", problem);
        return false;
    }
    return true;
}

void VideoEncoderPyroWave::Transmit(
    ID3D11Texture2D* pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR
) {
    (void)presentationTime;
    if (m_failed) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const int gpuTimer = m_gpuTimerActive;
    m_gpuTimerActive = -1;
    // A frame whose composition time is unknown (or stale) counts from here.
    const Clock::time_point ready
        = m_frameReadyTime <= start && start - m_frameReadyTime < std::chrono::milliseconds(100)
        ? m_frameReadyTime
        : start;

    if (!m_image) {
        D3D11_TEXTURE2D_DESC inputDesc;
        pTexture->GetDesc(&inputDesc);
        try {
            CreateSharedTexture(inputDesc);
        } catch (Exception& e) {
            Error("%s\n", e.what());
            m_failed = true;
            return;
        }
    }

    ID3D11DeviceContext* context = m_d3dRender->GetContext();
    if (gpuTimer >= 0) {
        context->End(m_gpuTimers[gpuTimer].copyStart.Get());
    }
    context->CopyResource(m_sharedTexture.Get(), pTexture);
    if (gpuTimer >= 0) {
        GpuTimer& timer = m_gpuTimers[gpuTimer];
        context->End(timer.copyEnd.Get());
        context->End(timer.disjoint.Get());
        timer.pending = true;
        timer.valid = true;
    }
    uint64_t copiedValue = ++m_fenceValue;
    m_context4->Signal(m_fence.Get(), copiedValue);
    // Vulkan waits for the signal above, so it must reach the GPU now, not at some later flush.
    context->Flush();
    uint64_t readValue = ++m_fenceValue;
    const Clock::time_point copySubmitted = Clock::now();

    pyrowave_gpu_external_reference ref = { m_image, VK_QUEUE_FAMILY_EXTERNAL };
    VkSemaphore semaphore = m_api.sync_object_get_semaphore(m_sync);

    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = &ref;
    acquire.num_images = 1;
    acquire.sync.semaphore = semaphore;
    acquire.sync.value = copiedValue;

    pyrowave_gpu_sync_operation release = {};
    release.images = &ref;
    release.num_images = 1;
    release.sync.semaphore = semaphore;
    release.sync.value = readValue;

    pyrowave_scaled_encode_info scaled = {};
    scaled.view = m_imageView;
    scaled.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.intermediate_plane_format = Settings::Instance().m_pyroWaveIntermediate16Bit
        ? VK_FORMAT_R16_UNORM
        : VK_FORMAT_R8_UNORM;
    scaled.ycbcr_chroma_midpoint = 0.5f;

    pyrowave_rate_control rateControl = {};
    rateControl.maximum_bitstream_size = m_maxFrameBytes;

    pyrowave_result res = m_api.encoder_encode_gpu_scaled_synchronous(
        m_encoder, &acquire, &release, &scaled, &rateControl
    );
    if (res != PYROWAVE_SUCCESS) {
        // Keep the timeline consistent: nothing will signal readValue now.
        m_context4->Signal(m_fence.Get(), readValue);
        Error("PyroWave: encode failed (error %d)\n", res);
        return;
    }
    // The next frame's copy must not overwrite the texture while Vulkan still reads it. This is
    // a GPU-side wait; the CPU does not block here.
    m_context4->Wait(m_fence.Get(), readValue);
    const Clock::time_point encodeSubmitted = Clock::now();

    // Only for the log: wait for the D3D11 part first, so the GPU time of each API is known. The
    // Vulkan encode waits for the same fence value on the GPU anyway, so this costs nothing but
    // one wake-up of this thread.
    if (SUCCEEDED(m_fence->SetEventOnCompletion(copiedValue, m_copyDoneEvent))) {
        WaitForSingleObject(m_copyDoneEvent, 100);
    }
    const Clock::time_point copyDone = Clock::now();

    // Waits for the Vulkan encode.
    const void* mappedBitstream = nullptr;
    const void* mappedMeta = nullptr;
    size_t bitstreamBytes = 0, metaBytes = 0;
    res = m_api.encoder_get_mapped_raw_bitstream(
        m_encoder, &mappedBitstream, &bitstreamBytes, &mappedMeta, &metaBytes
    );
    if (res != PYROWAVE_SUCCESS) {
        Error("PyroWave: get_mapped_raw_bitstream failed (error %d)\n", res);
        return;
    }
    const Clock::time_point encodeDone = Clock::now();

    if (!BuildPackets(mappedBitstream, bitstreamBytes, mappedMeta, metaBytes)) {
        return;
    }
    const size_t frameBytes = m_sendBuffer.size();
    const size_t packetCount = m_packetSizes.size();
    // Without the padding, for the log.
    size_t codedBytes = 0;
    for (size_t i = 0, offset = 0; i < packetCount; offset += m_packetSizes[i], i++) {
        PyroWaveStripes::PacketPrefix prefix;
        memcpy(&prefix, &m_sendBuffer[offset], sizeof(prefix));
        codedBytes += sizeof(prefix) + prefix.data_bytes;
    }
    m_statCodedBytes += codedBytes;
    const Clock::time_point packetized = Clock::now();

    // Like SPS/PPS for H.264, the config goes out ahead of each IDR the scheduler asks for.
    // Every PyroWave frame is intra coded, so each one is reported as an IDR.
    if (insertIDR || !m_configSent) {
        SendStreamConfig();
        m_configSent = true;
    }
    // Each packet becomes its own video packet with the frame's timestamp, so the headset can
    // start decoding a stripe as soon as its packets are in.
    VideoSendPackets(
        targetTimestampNs,
        m_sendBuffer.data(),
        m_packetSizes.data(),
        (unsigned int)packetCount,
        true
    );

    const Clock::time_point sent = Clock::now();
    CollectGpuTimers();

    const double phaseMs[8] = {
        MsBetween(ready, start),
        MsBetween(start, copySubmitted),
        MsBetween(copySubmitted, encodeSubmitted),
        MsBetween(encodeSubmitted, copyDone),
        MsBetween(copyDone, encodeDone),
        MsBetween(encodeDone, packetized),
        MsBetween(packetized, sent),
        MsBetween(ready, sent),
    };
    ReportTiming(phaseMs, frameBytes, packetCount);
}

void VideoEncoderPyroWave::ReportTiming(
    const double (&phaseMs)[8], size_t frameBytes, size_t packetCount
) {
    m_statFrames++;
    m_statBytes += frameBytes;
    m_statPackets += packetCount;
    for (int i = 0; i < 8; i++) {
        m_statPhaseSumMs[i] += phaseMs[i];
        m_statPhaseMaxMs[i] = std::max(m_statPhaseMaxMs[i], phaseMs[i]);
    }
    double sinceReportMs = MsSince(m_statStart);
    if (sinceReportMs < 5000.0) {
        return;
    }

    Info(
        "PyroWave: %.1f fps, %.0f Mbps, %.0f KB per frame (coded %.0f KB, the rest is padding; "
        "limit %zu KB), %.0f packets per frame\n",
        m_statFrames * 1000.0 / sinceReportMs,
        m_statBytes * 8.0 / sinceReportMs / 1000.0,
        m_statBytes / 1024.0 / m_statFrames,
        m_statCodedBytes / 1024.0 / m_statFrames,
        m_maxFrameBytes / 1024,
        (double)m_statPackets / m_statFrames
    );
    std::string split = "PyroWave timing, ms avg/max:";
    for (int i = 0; i < 8; i++) {
        char part[64];
        snprintf(
            part,
            sizeof(part),
            " %s %.2f/%.2f%s",
            kPhaseNames[i],
            m_statPhaseSumMs[i] / m_statFrames,
            m_statPhaseMaxMs[i],
            i < 7 ? " |" : ""
        );
        split += part;
    }
    Info("%s\n", split.c_str());
    if (m_statGpuTimed > 0) {
        // The rest of "D3D11 GPU" is waiting: for the frame's own rendering (game and SteamVR's
        // compositor) to finish, and for other work on the graphics queue.
        Info(
            "PyroWave D3D11 GPU work: composition (FrameRender) %.2f ms avg %.2f max, copy %.2f ms "
            "(%llu frames timed)\n",
            m_statComposeGpuMs / m_statGpuTimed,
            m_statComposeGpuMaxMs,
            m_statCopyGpuMs / m_statGpuTimed,
            (unsigned long long)m_statGpuTimed
        );
    }

    // GPU timestamps of PyroWave's own passes (the RGB -> YCbCr input stage is not among them).
    std::string gpu;
    m_api.device_report_performance_stats(
        m_device,
        [](void* userdata, const char* msg) {
            std::string& out = *static_cast<std::string*>(userdata);
            // Memory heap lines are not what this log is for.
            if (strncmp(msg, "Memory Heap", 11) == 0) {
                return;
            }
            out += out.empty() ? " " : " | ";
            out += msg;
        },
        &gpu,
        true
    );
    if (!gpu.empty()) {
        Info("PyroWave GPU passes:%s\n", gpu.c_str());
    }

    m_statStart = std::chrono::steady_clock::now();
    m_statFrames = 0;
    m_statBytes = 0;
    m_statCodedBytes = 0;
    m_statPackets = 0;
    m_statGpuTimed = 0;
    m_statComposeGpuMs = 0.0;
    m_statComposeGpuMaxMs = 0.0;
    m_statCopyGpuMs = 0.0;
    for (int i = 0; i < 8; i++) {
        m_statPhaseSumMs[i] = 0.0;
        m_statPhaseMaxMs[i] = 0.0;
    }
}

void VideoEncoderPyroWave::ReleaseResources() {
    // PyroWave waits for the GPU to be idle before destroying anything.
    if (m_encoder) {
        m_api.encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    if (m_image) {
        m_api.image_destroy(m_image);
        m_image = nullptr;
    }
    if (m_sync) {
        m_api.sync_object_destroy(m_sync);
        m_sync = nullptr;
    }
    if (m_device) {
        m_api.device_destroy(m_device);
        m_device = nullptr;
    }
    m_sharedTexture.Reset();
    m_fence.Reset();
    for (GpuTimer& timer : m_gpuTimers) {
        timer = GpuTimer();
    }
    m_gpuTimerActive = -1;
    if (m_copyDoneEvent) {
        CloseHandle(m_copyDoneEvent);
        m_copyDoneEvent = nullptr;
    }
}

void VideoEncoderPyroWave::Shutdown() {
    ReleaseResources();
    if (m_api.module) {
        FreeLibrary(m_api.module);
        m_api = Api();
    }
}

#endif
