#pragma once
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

// A single bounded handoff texture. Both copies finish being *issued* on the
// producer thread before enqueue/return. The encoder owns the destination pool.
// No readback, host ClearState/Flush, per-frame handles, or adapter guessing.
class AmfDeviceBridge
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11Device> sourceDevice, encoderDevice;
    ComPtr<ID3D11DeviceContext> sourceContext, encoderContext;
    ComPtr<ID3D11Texture2D> sharedSource, sharedEncoder;
    ComPtr<IDXGIKeyedMutex> sourceMutex, encoderMutex;
public:
    uint64_t adapterLuid = 0;
    uint64_t nominalBytes = 0;
    ID3D11Device* Device() const { return encoderDevice.Get(); }
    bool Initialize(ID3D11Device* source, const D3D11_TEXTURE2D_DESC& ordinary, std::wstring& error)
    {
        sourceDevice = source;
        source->GetImmediateContext(&sourceContext);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = sourceDevice.As(&dxgi);
        if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapterDesc);
        if (FAILED(hr)) { error = L"Could not identify the source D3D adapter."; return false; }
        adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(adapterDesc.AdapterLuid.HighPart)) << 32)
            | adapterDesc.AdapterLuid.LowPart;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &encoderDevice, nullptr, &encoderContext);
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(hr)) hr = encoderContext.As(&multithread);
        if (FAILED(hr)) { error = L"Could not create a dedicated D3D11 video device on the source adapter."; return false; }
        multithread->SetMultithreadProtected(TRUE); // Only our own device, never change the host's setting.
        auto desc = ordinary;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = sourceDevice->CreateTexture2D(&desc, nullptr, &sharedSource);
        ComPtr<IDXGIResource1> resource;
        ComPtr<ID3D11Device1> encoder1;
        if (SUCCEEDED(hr)) hr = sharedSource.As(&resource);
        if (SUCCEEDED(hr)) hr = encoderDevice.As(&encoder1);
        HANDLE handle = nullptr;
        if (SUCCEEDED(hr)) hr = resource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        if (SUCCEEDED(hr)) hr = encoder1->OpenSharedResource1(handle, IID_PPV_ARGS(&sharedEncoder));
        if (handle) CloseHandle(handle); // NT handle, not a legacy DXGI pseudo-handle.
        if (SUCCEEDED(hr)) hr = sharedSource.As(&sourceMutex);
        if (SUCCEEDED(hr)) hr = sharedEncoder.As(&encoderMutex);
        if (FAILED(hr)) { error = L"Dedicated-device shared texture initialization failed: " + std::to_wstring(hr); return false; }
        nominalBytes = static_cast<uint64_t>(desc.Width) * desc.Height * 4;
        return true;
    }
    bool Copy(ID3D11Texture2D* destination, ID3D11Texture2D* source, std::wstring& error)
    {
        // WAIT_TIMEOUT / WAIT_ABANDONED are positive HRESULTs: require S_OK.
        HRESULT hr = sourceMutex->AcquireSync(0, 5000);
        if (hr != S_OK) { error = L"Source keyed mutex acquisition failed: " + std::to_wstring(hr); return false; }
        sourceContext->CopyResource(sharedSource.Get(), source);
        hr = sourceMutex->ReleaseSync(1);
        if (hr != S_OK) { error = L"Source keyed mutex release failed: " + std::to_wstring(hr); return false; }
        hr = encoderMutex->AcquireSync(1, 5000);
        if (hr != S_OK) { error = L"Encoder keyed mutex acquisition failed: " + std::to_wstring(hr); return false; }
        encoderContext->CopyResource(destination, sharedEncoder.Get());
        hr = encoderMutex->ReleaseSync(0);
        if (hr != S_OK) { error = L"Encoder keyed mutex release failed: " + std::to_wstring(hr); return false; }
        return true;
    }
    // Invoke only after AMF/owned pool destruction. Never clear the borrowed host context.
    void ReleaseResources()
    {
        sourceMutex.Reset(); encoderMutex.Reset(); sharedSource.Reset(); sharedEncoder.Reset();
        if (encoderContext) { encoderContext->ClearState(); encoderContext->Flush(); }
    }
};
