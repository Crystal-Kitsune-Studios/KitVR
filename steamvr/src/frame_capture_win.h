#pragma once
// Windows frame capture for the IVRVirtualDisplay path.
//
// SteamVR's compositor hands the driver a shared D3D11 texture in IVRVirtualDisplay::Present. This
// copies it to a staging texture, reads it back in WaitForPresent, and publishes the pixels into a
// named shared memory ring with the same layout the Linux Vulkan layer writes (see
// linux/README.md), so `kitvr-server --source steamvr` reads both the same way.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace kitvr {

constexpr uint32_t kRingMagic = 0x4B565246;  // "KVRF"
constexpr uint32_t kRingVersion = 1;
constexpr size_t kRingHeaderBytes = 48;
// Address space reserved for the ring. Pages are only committed for the frame size in use.
constexpr uint64_t kRingReserveBytes = kRingHeaderBytes + 8192ull * 8192ull * 4ull;

enum RingFormat : uint32_t {
    RingFormatBgra8 = 0,
    RingFormatRgba8 = 1,
    RingFormatX2Rgb10 = 2,
    RingFormatX2Bgr10 = 3,
    RingFormatUnsupported = 0xFFFFFFFF,
};

inline uint32_t ringFormatForDxgi(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return RingFormatBgra8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return RingFormatRgba8;
    // DXGI keeps red in the low bits, which is ffmpeg's x2bgr10le.
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return RingFormatX2Bgr10;
    default:
        return RingFormatUnsupported;
    }
}

// Producer side of the frame ring, in a pagefile-backed section named Local\<name>.
class FrameRing {
public:
    ~FrameRing() { close(); }

    bool open() {
        if (base_) return true;
        const char* env = std::getenv("KITVR_STEAMVR_SHM");
        std::string name = env && *env ? env : "kitvr-steamvr";
        if (name.find('\\') == std::string::npos) name = "Local\\" + name;
        const std::wstring wide(name.begin(), name.end());
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                      static_cast<DWORD>(kRingReserveBytes >> 32),
                                      static_cast<DWORD>(kRingReserveBytes & 0xFFFFFFFF),
                                      wide.c_str());
        if (!mapping_) return false;
        base_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 0));
        if (!base_ || !commit(kRingHeaderBytes)) {
            close();
            return false;
        }
        writeU32(0, kRingMagic);
        writeU32(4, kRingVersion);
        return true;
    }

    void close() {
        if (base_) UnmapViewOfFile(base_);
        if (mapping_) CloseHandle(mapping_);
        base_ = nullptr;
        mapping_ = nullptr;
        committed_ = 0;
    }

    // Copies `height` rows of `width * 4` bytes from `pixels` (rows `pitch` bytes apart) and
    // publishes them under a new frame id.
    bool publish(uint32_t width, uint32_t height, uint32_t format, const uint8_t* pixels, uint32_t pitch) {
        if (!base_) return false;
        const uint64_t stride = static_cast<uint64_t>(width) * 4;
        const uint64_t payload = stride * height;
        if (payload > 0xFFFFFFFFull || kRingHeaderBytes + payload > kRingReserveBytes) return false;
        if (!commit(kRingHeaderBytes + static_cast<size_t>(payload))) return false;
        writeU32(8, width);
        writeU32(12, height);
        writeU32(16, format);
        writeU32(20, static_cast<uint32_t>(stride));
        writeU32(24, static_cast<uint32_t>(payload));
        uint8_t* dst = base_ + kRingHeaderBytes;
        if (pitch == stride) {
            std::memcpy(dst, pixels, static_cast<size_t>(payload));
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                std::memcpy(dst + row * stride, pixels + static_cast<size_t>(row) * pitch, static_cast<size_t>(stride));
            }
        }
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const uint64_t ns = static_cast<uint64_t>(
            static_cast<double>(counter.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart));
        std::memcpy(base_ + 40, &ns, sizeof(ns));
        // Full barrier: the reader's acquire load of the id then sees the pixels above.
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(base_ + 32), static_cast<LONG64>(++frameId_));
        return true;
    }

private:
    bool commit(size_t bytes) {
        if (bytes <= committed_) return true;
        if (!VirtualAlloc(base_, bytes, MEM_COMMIT, PAGE_READWRITE)) return false;
        committed_ = bytes;
        return true;
    }

    void writeU32(size_t offset, uint32_t value) { std::memcpy(base_ + offset, &value, sizeof(value)); }

    HANDLE mapping_ = nullptr;
    uint8_t* base_ = nullptr;
    size_t committed_ = 0;
    uint64_t frameId_ = 0;
};

// Reads back the compositor's shared backbuffer and feeds the frame ring.
class D3D11FrameCapture {
public:
    using LogFn = void (*)(const char*);

    explicit D3D11FrameCapture(LogFn log) : log_(log) {}
    ~D3D11FrameCapture() { reset(); }

    // Creates the device on the first hardware adapter. Returns its LUID so the HMD can ask SteamVR
    // to render on the same adapter; a shared texture cannot be opened from another one.
    bool init(uint64_t* adapterLuid) {
        if (device_) {
            if (adapterLuid) *adapterLuid = luid_;
            return true;
        }
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            log("KitVR: CreateDXGIFactory1 failed");
            return false;
        }
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        // No hardware adapter (for example a VM without GPU passthrough): use the default, which
        // is WARP or the basic display adapter and is fine for testing the frame path.
        const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        const HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (adapter) adapter->Release();
        if (FAILED(hr)) {
            log("KitVR: D3D11CreateDevice failed; SteamVR frames will not be captured");
            return false;
        }
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
            IDXGIAdapter* used = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&used))) {
                DXGI_ADAPTER_DESC desc{};
                used->GetDesc(&desc);
                luid_ = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                        desc.AdapterLuid.LowPart;
                used->Release();
            }
            dxgiDevice->Release();
        }
        if (!ring_.open()) log("KitVR: could not create the frame ring shared memory");
        if (adapterLuid) *adapterLuid = luid_;
        return true;
    }

    // Queues a copy of the shared backbuffer. Returns quickly; the readback happens in wait().
    void present(uint64_t sharedHandle) {
        if (!device_ || !sharedHandle) return;
        Source* source = sourceFor(sharedHandle);
        if (!source) return;
        D3D11_TEXTURE2D_DESC desc{};
        source->texture->GetDesc(&desc);
        if (!ensureStaging(desc)) return;
        bool locked = false;
        if (source->mutex) {
            const HRESULT hr = source->mutex->AcquireSync(0, 10);
            if (hr != S_OK) return;  // WAIT_TIMEOUT or abandoned: skip this frame.
            locked = true;
        }
        context_->CopyResource(staging_, source->texture);
        if (locked) source->mutex->ReleaseSync(0);
        context_->Flush();
        pending_ = true;
    }

    // Waits for the copy, then publishes the pixels.
    void wait() {
        if (!pending_) return;
        pending_ = false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) return;
        ring_.publish(stagingDesc_.Width, stagingDesc_.Height, format_,
                      static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch);
        context_->Unmap(staging_, 0);
    }

private:
    struct Source {
        ID3D11Texture2D* texture = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
    };

    Source* sourceFor(uint64_t handle) {
        auto found = sources_.find(handle);
        if (found != sources_.end()) return &found->second;
        // SteamVR rotates a few backbuffers; drop stale ones if the set keeps growing.
        if (sources_.size() >= 8) releaseSources();
        Source source;
        const HRESULT hr = device_->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
                                                       reinterpret_cast<void**>(&source.texture));
        if (FAILED(hr) || !source.texture) {
            log("KitVR: could not open the SteamVR backbuffer (is SteamVR on a different GPU?)");
            return nullptr;
        }
        source.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&source.mutex));
        return &sources_.emplace(handle, source).first->second;
    }

    bool ensureStaging(const D3D11_TEXTURE2D_DESC& desc) {
        if (staging_ && desc.Width == stagingDesc_.Width && desc.Height == stagingDesc_.Height &&
            desc.Format == stagingDesc_.Format) {
            return format_ != RingFormatUnsupported;
        }
        if (staging_) staging_->Release();
        staging_ = nullptr;
        stagingDesc_ = desc;
        format_ = ringFormatForDxgi(desc.Format);
        if (format_ == RingFormatUnsupported) {
            char message[96];
            std::snprintf(message, sizeof(message), "KitVR: unsupported backbuffer format %d",
                          static_cast<int>(desc.Format));
            log(message);
            return false;
        }
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = desc.Width;
        staging.Height = desc.Height;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = desc.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&staging, nullptr, &staging_))) {
            log("KitVR: could not create the staging texture");
            staging_ = nullptr;
            return false;
        }
        return true;
    }

    void releaseSources() {
        for (auto& entry : sources_) {
            if (entry.second.mutex) entry.second.mutex->Release();
            if (entry.second.texture) entry.second.texture->Release();
        }
        sources_.clear();
    }

    void reset() {
        releaseSources();
        if (staging_) staging_->Release();
        if (context_) context_->Release();
        if (device_) device_->Release();
        staging_ = nullptr;
        context_ = nullptr;
        device_ = nullptr;
    }

    void log(const char* message) const {
        if (log_) log_(message);
    }

    LogFn log_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc_{};
    uint32_t format_ = RingFormatUnsupported;
    std::unordered_map<uint64_t, Source> sources_;
    uint64_t luid_ = 0;
    bool pending_ = false;
    FrameRing ring_;
};

}  // namespace kitvr

#endif  // _WIN32
