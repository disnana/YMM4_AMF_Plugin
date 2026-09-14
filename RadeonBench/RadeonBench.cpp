#include "../AmfNative/AmfNative.h"

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "public/include/core/Factory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    struct Options
    {
        std::wstring command;
        std::wstring output = L"artifacts\\runs\\smoke\\output.mp4";
        std::wstring result = L"artifacts\\runs\\smoke\\run.json";
        int width = 640;
        int height = 360;
        int fps = 60;
        int frames = 120;
        int bitrateKbps = 4000;
        int maxBitrateKbps = 4800;
        int codec = 0;
        int quality = 1;
        int rateControl = 1;
        int poolSize = 4;
        bool audio = false;
    };

    std::string Utf8(const std::wstring& value)
    {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string output(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
        return output;
    }

    std::string JsonEscape(const std::wstring& value)
    {
        std::string result;
        for (char ch : Utf8(value))
        {
            switch (ch)
            {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(ch); break;
            }
        }
        return result;
    }

    std::string VersionString(amf_uint64 version)
    {
        return std::to_string(AMF_GET_MAJOR_VERSION(version)) + "."
            + std::to_string(AMF_GET_MINOR_VERSION(version)) + "."
            + std::to_string(AMF_GET_SUBMINOR_VERSION(version)) + "."
            + std::to_string(AMF_GET_BUILD_VERSION(version));
    }

    bool ParseInt(const wchar_t* value, int& destination)
    {
        wchar_t* end = nullptr;
        const long parsed = wcstol(value, &end, 10);
        if (!end || *end != L'\0' || parsed < 1 || parsed > INT_MAX) return false;
        destination = static_cast<int>(parsed);
        return true;
    }

    bool ParseOptions(int argc, wchar_t** argv, Options& options)
    {
        if (argc < 2) return false;
        options.command = argv[1];
        for (int i = 2; i < argc; ++i)
        {
            const std::wstring key = argv[i];
            if (key == L"--audio")
            {
                options.audio = true;
                continue;
            }
            if (i + 1 >= argc) return false;
            const wchar_t* value = argv[++i];
            if (key == L"--output") options.output = value;
            else if (key == L"--result") options.result = value;
            else if (key == L"--width") { if (!ParseInt(value, options.width)) return false; }
            else if (key == L"--height") { if (!ParseInt(value, options.height)) return false; }
            else if (key == L"--fps") { if (!ParseInt(value, options.fps)) return false; }
            else if (key == L"--frames") { if (!ParseInt(value, options.frames)) return false; }
            else if (key == L"--bitrate-kbps") { if (!ParseInt(value, options.bitrateKbps)) return false; }
            else if (key == L"--max-bitrate-kbps") { if (!ParseInt(value, options.maxBitrateKbps)) return false; }
            else if (key == L"--pool-size") { if (!ParseInt(value, options.poolSize)) return false; }
            else if (key == L"--codec")
            {
                const std::wstring codec = value;
                if (codec == L"h264") options.codec = 0;
                else if (codec == L"hevc") options.codec = 1;
                else return false;
            }
            else if (key == L"--quality")
            {
                const std::wstring quality = value;
                if (quality == L"speed") options.quality = 0;
                else if (quality == L"balanced") options.quality = 1;
                else if (quality == L"quality") options.quality = 2;
                else return false;
            }
            else if (key == L"--rate-control")
            {
                const std::wstring rateControl = value;
                if (rateControl == L"cbr") options.rateControl = 0;
                else if (rateControl == L"vbr") options.rateControl = 1;
                else return false;
            }
            else return false;
        }
        return options.command == L"probe" || options.command == L"run";
    }

    bool CreateDevice(ID3D11Device** device, ID3D11DeviceContext** context, std::wstring& adapterName, std::wstring& error)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, ARRAYSIZE(requested), D3D11_SDK_VERSION,
            device, &featureLevel, context);
        if (FAILED(hr))
        {
            error = L"D3D11CreateDevice failed: " + std::to_wstring(static_cast<unsigned long>(hr));
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED((*device)->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))
            && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) adapterName = desc.Description;
        }
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();
        return true;
    }

    amf_uint64 QueryAmfVersion(std::wstring& error)
    {
        HMODULE module = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
        {
            error = L"amfrt64.dll was not found in System32";
            return 0;
        }
        auto queryVersion = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(module, AMF_QUERY_VERSION_FUNCTION_NAME));
        amf_uint64 version = 0;
        if (!queryVersion || queryVersion(&version) != AMF_OK) error = L"AMFQueryVersion failed";
        FreeLibrary(module);
        return version;
    }

    void FillFrame(std::vector<uint8_t>& pixels, int width, int height, uint32_t frameIndex)
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                pixels[offset + 0] = static_cast<uint8_t>((x + frameIndex * 3) & 0xff);
                pixels[offset + 1] = static_cast<uint8_t>((y * 2 + frameIndex * 5) & 0xff);
                pixels[offset + 2] = static_cast<uint8_t>(((x ^ y) + frameIndex * 7) & 0xff);
                pixels[offset + 3] = 255;
            }
        }

        const int markerSize = std::max(4, std::min(width, height) / 32);
        for (int bit = 0; bit < 16; ++bit)
        {
            const uint8_t value = (frameIndex & (1u << bit)) ? 255 : 0;
            const int startX = (bit % 8) * markerSize;
            const int startY = (bit / 8) * markerSize;
            for (int y = startY; y < std::min(height, startY + markerSize); ++y)
            {
                for (int x = startX; x < std::min(width, startX + markerSize); ++x)
                {
                    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                    pixels[offset + 0] = value;
                    pixels[offset + 1] = value;
                    pixels[offset + 2] = value;
                }
            }
        }
    }

    void EnsureParent(const std::wstring& path)
    {
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
    }

    void WriteResult(const Options& options, const std::wstring& status, const std::wstring& reason,
        const std::wstring& adapterName, amf_uint64 runtimeVersion, double wallMs, int acceptedFrames)
    {
        EnsureParent(options.result);
        std::ofstream file(std::filesystem::path(options.result), std::ios::binary | std::ios::trunc);
        const double completedFps = wallMs > 0.0 ? acceptedFrames * 1000.0 / wallMs : 0.0;
        file << "{\n"
            << "  \"schema_version\": 1,\n"
            << "  \"status\": \"" << JsonEscape(status) << "\",\n"
            << "  \"reason\": \"" << JsonEscape(reason) << "\",\n"
            << "  \"environment\": {\"gpu_name\": \"" << JsonEscape(adapterName)
            << "\", \"amf_runtime_version\": \"" << VersionString(runtimeVersion)
            << "\", \"amf_sdk_version\": \"" << VersionString(AMF_FULL_VERSION) << "\"},\n"
            << "  \"requested_config\": {\"width\": " << options.width << ", \"height\": " << options.height
            << ", \"fps\": " << options.fps << ", \"frames\": " << options.frames
            << ", \"codec\": \"" << (options.codec == 1 ? "hevc" : "h264")
            << "\", \"pool_size\": " << options.poolSize << ", \"audio\": " << (options.audio ? "true" : "false")
            << ", \"rate_control\": \"" << (options.rateControl == 0 ? "cbr" : "vbr")
            << "\", \"quality_intent\": \"" << (options.quality == 0 ? "speed" : options.quality == 2 ? "quality" : "balanced")
            << "\", \"target_bitrate_kbps\": " << options.bitrateKbps
            << ", \"max_bitrate_kbps\": " << options.maxBitrateKbps << "},\n"
            << "  \"metrics\": {\"export_wall_ms\": " << wallMs << ", \"accepted_frames\": "
            << acceptedFrames << ", \"completed_fps\": " << completedFps << "},\n"
            << "  \"validation\": {\"decode\": \"not_run_missing_dependency\", \"frame_order\": \"not_run\", \"color\": \"not_run\", \"audio_sync\": \"not_run\"}\n"
            << "}\n";
    }

    int Probe(const Options& options)
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        std::wstring adapterName;
        std::wstring error;
        const bool deviceOk = CreateDevice(&device, &context, adapterName, error);
        const amf_uint64 version = QueryAmfVersion(error);
        if (context) context->Release();
        if (device) device->Release();
        WriteResult(options, deviceOk && version != 0 ? L"passed" : L"failed", error, adapterName, version, 0, 0);
        return deviceOk && version != 0 ? 0 : 2;
    }

    int Run(const Options& options)
    {
        if ((options.width & 1) || (options.height & 1) || options.poolSize < 4 || options.poolSize > 8
            || (options.rateControl != 0 && options.maxBitrateKbps < options.bitrateKbps))
        {
            WriteResult(options, L"failed", L"invalid_dimensions_or_pool_size", L"", 0, 0, 0);
            return 2;
        }

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        ID3D11Texture2D* texture = nullptr;
        std::wstring adapterName;
        std::wstring error;
        const amf_uint64 runtimeVersion = QueryAmfVersion(error);
        if (!CreateDevice(&device, &context, adapterName, error))
        {
            WriteResult(options, L"failed", error, adapterName, runtimeVersion, 0, 0);
            return 2;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(options.width);
        desc.Height = static_cast<UINT>(options.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) error = L"CreateTexture2D failed";

        EnsureParent(options.output);
        void* encoder = error.empty() ? AmfCreate(device, options.width, options.height, options.fps,
            options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
            3, options.poolSize, 1, options.output.c_str()) : nullptr;
        if (encoder && AmfGetLastError(encoder)[0] != L'\0') error = AmfGetLastError(encoder);
        if (!encoder && error.empty()) error = L"AmfCreate returned null";

        std::vector<uint8_t> pixels(static_cast<size_t>(options.width) * options.height * 4);
        std::vector<float> audioSamples;
        constexpr int audioRate = 48000;
        constexpr int audioChannels = 2;
        int64_t emittedSamplesPerChannel = 0;
        int acceptedFrames = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; encoder && error.empty() && frame < options.frames; ++frame)
        {
            FillFrame(pixels, options.width, options.height, static_cast<uint32_t>(frame));
            context->UpdateSubresource(texture, 0, nullptr, pixels.data(), static_cast<UINT>(options.width * 4), 0);
            if (!AmfEncode(encoder, texture)) error = AmfGetLastError(encoder);
            else ++acceptedFrames;

            if (options.audio && error.empty())
            {
                const int64_t targetSamplesPerChannel = static_cast<int64_t>(frame + 1) * audioRate / options.fps;
                const int sampleFrames = static_cast<int>(targetSamplesPerChannel - emittedSamplesPerChannel);
                audioSamples.resize(static_cast<size_t>(sampleFrames) * audioChannels);
                for (int sample = 0; sample < sampleFrames; ++sample)
                {
                    const double time = static_cast<double>(emittedSamplesPerChannel + sample) / audioRate;
                    const float value = static_cast<float>(0.15 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * time));
                    audioSamples[static_cast<size_t>(sample) * 2] = value;
                    audioSamples[static_cast<size_t>(sample) * 2 + 1] = value;
                }
                if (!audioSamples.empty() && !AmfWriteAudio(encoder, audioSamples.data(), static_cast<int>(audioSamples.size()), audioRate, audioChannels))
                {
                    error = AmfGetLastError(encoder);
                }
                emittedSamplesPerChannel = targetSamplesPerChannel;
            }
        }
        if (encoder && error.empty() && !AmfFinalize(encoder)) error = AmfGetLastError(encoder);
        const auto end = std::chrono::steady_clock::now();
        const double wallMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (encoder) AmfDestroy(encoder);
        if (texture) texture->Release();
        if (context) context->Release();
        if (device) device->Release();

        const bool passed = error.empty() && acceptedFrames == options.frames;
        WriteResult(options, passed ? L"passed" : L"failed", error, adapterName, runtimeVersion, wallMs, acceptedFrames);
        return passed ? 0 : 3;
    }
}

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        std::wcerr << L"Usage:\n"
            << L"  RadeonBench probe [--result probe.json]\n"
            << L"  RadeonBench run [--output output.mp4] [--result run.json] [--codec h264|hevc]\n"
            << L"                  [--width N] [--height N] [--fps N] [--frames N] [--bitrate-kbps N]\n"
            << L"                  [--max-bitrate-kbps N] [--rate-control cbr|vbr] [--quality speed|balanced|quality]\n"
            << L"                  [--pool-size 4|6|8] [--audio]\n";
        return 1;
    }
    return options.command == L"probe" ? Probe(options) : Run(options);
}
