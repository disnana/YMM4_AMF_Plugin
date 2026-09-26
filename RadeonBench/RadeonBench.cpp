#include "../AmfNative/AmfNative.h"
#include "../AmfNative/AmfProfiling.h"
#include "../AmfNative/AmfOutputWait.h"
#include "../AmfNative/AmfInputRecyclePool.h"
#include "../AmfNative/AmfSubmissionQueue.h"
#include "../AmfNative/AmfInputWait.h"
#include "../AmfNative/AmfTexturePoolPolicy.h"

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include "public/include/core/Factory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    struct Options
    {
        std::wstring command;
        std::wstring output = L"artifacts\\runs\\smoke\\output.mp4";
        std::wstring result = L"artifacts\\runs\\smoke\\run.json";
        std::wstring rawReference;
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
        bool profile = false;
        bool debugLog = true;
        bool optimizeOutputWait = false;
        bool recycleInput = false;
        bool asyncSubmission = false;
        bool dedicatedDevice = false;
        bool adaptiveInputWait = false;
        bool mfStyleNv12 = false;
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
            if (key == L"--profile") { options.profile = true; continue; }
            if (key == L"--optimize-output-wait") { options.optimizeOutputWait = true; continue; }
            if (key == L"--async-submission") { options.asyncSubmission = true; continue; }
            if (key == L"--dedicated-device") { options.dedicatedDevice = true; continue; }
            if (key == L"--adaptive-input-wait") { options.adaptiveInputWait = true; continue; }
            if (key == L"--recycle-input") { options.recycleInput = true; continue; }
            if (key == L"--mf-style-nv12") { options.mfStyleNv12 = true; continue; }
            if (key == L"--no-debug-log") { options.debugLog = false; continue; }
            if (key == L"--audio")
            {
                options.audio = true;
                continue;
            }
            if (i + 1 >= argc) return false;
            const wchar_t* value = argv[++i];
            if (key == L"--output") options.output = value;
            else if (key == L"--result") options.result = value;
            else if (key == L"--raw-reference") options.rawReference = value;
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
        return options.command == L"probe" || options.command == L"run" || options.command == L"profile-self-test"
            || options.command == L"output-wait-self-test" || options.command == L"output-wait-stop-test"
            || options.command == L"input-recycling-self-test" || options.command == L"pipeline-self-test"
            || options.command == L"pipeline-contract-test";
    }

    bool CreateDevice(ID3D11Device** device, ID3D11DeviceContext** context, std::wstring& adapterName, std::wstring& error)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            requested, ARRAYSIZE(requested), D3D11_SDK_VERSION,
            device, &featureLevel, context);
        if (FAILED(hr))
        {
            error = L"D3D11CreateDevice failed: " + std::to_wstring(static_cast<unsigned long>(hr));
            return false;
        }

        ID3D11Multithread* protection = nullptr;
        if (SUCCEEDED((*context)->QueryInterface(IID_PPV_ARGS(&protection))))
        { protection->SetMultithreadProtected(TRUE); protection->Release(); }
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

    void WriteProfile(std::ofstream& file, const AmfProfileSnapshot* profile)
    {
        if (!profile) { file << "null"; return; }
        file << "{\"version\":" << profile->version << ",\"accepted_frames\":" << profile->acceptedFrames
            << ",\"completed_frames\":" << profile->completedFrames << ",\"input_retries\":" << profile->inputRetries
            << ",\"stages\":{";
        for (uint32_t i = 0; i < AmfProfileStageCount; ++i)
        {
            const auto& metric = profile->metrics[i];
            const double totalMs = metric.totalNanoseconds / 1000000.0;
            if (i) file << ',';
            file << '\"' << AmfProfileStageNames[i] << "\":{\"count\":" << metric.count
                << ",\"total_ms\":" << totalMs << ",\"mean_ms\":" << (metric.count ? totalMs / metric.count : 0)
                << ",\"max_ms\":" << metric.maxNanoseconds / 1000000.0 << '}';
        }
        file << "}}";
    }

    void WriteResult(const Options& options, const std::wstring& status, const std::wstring& reason,
        const std::wstring& adapterName, amf_uint64 runtimeVersion, double wallMs, int acceptedFrames,
        const AmfProfileSnapshot* profile = nullptr, const AmfOutputWaitStatus* outputWait = nullptr,
        const AmfInputRecycleStatus* recycle = nullptr, const AmfTexturePoolStatus* pool = nullptr,
        const AmfPipelineStatus* pipeline = nullptr, const AmfInputWaitStatus* inputWait = nullptr,
        const AmfMfStyleNv12Status* mfStyleNv12 = nullptr)
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
            << ", \"max_bitrate_kbps\": " << options.maxBitrateKbps
            << ", \"profiling\": " << (options.profile ? "true" : "false")
            << ", \"optimize_output_wait\": " << (options.optimizeOutputWait ? "true" : "false")
            << ", \"recycle_input\": " << (options.recycleInput ? "true" : "false")
            << ", \"async_submission\": " << (options.asyncSubmission ? "true" : "false")
            << ", \"dedicated_device\": " << (options.dedicatedDevice ? "true" : "false")
            << ", \"adaptive_input_wait\": " << (options.adaptiveInputWait ? "true" : "false")
            << ", \"mf_style_nv12\": " << (options.mfStyleNv12 ? "true" : "false")
            << ", \"debug_log\": " << (options.debugLog ? "true" : "false") << "},\n"
            << "  \"metrics\": {\"export_wall_ms\": " << wallMs << ", \"accepted_frames\": "
            << acceptedFrames << ", \"completed_fps\": " << completedFps << "},\n"
            << "  \"profiling\": ";
        WriteProfile(file, profile);
        file << ",\n  \"pipeline\": ";
        if (!pipeline) file << "null";
        else file << "{\"initialized\":" << pipeline->initialized
            << ",\"async_submission\":" << pipeline->asyncSubmission
            << ",\"dedicated_device\":" << pipeline->dedicatedDevice
            << ",\"gpu_copies_per_frame\":" << pipeline->gpuCopiesPerFrame
            << ",\"queued_frames\":" << pipeline->queuedFrames
            << ",\"submitted_frames\":" << pipeline->submittedFrames
            << ",\"queue_depth\":" << pipeline->queueDepth
            << ",\"peak_queue_depth\":" << pipeline->peakQueueDepth
            << ",\"queue_capacity\":" << pipeline->queueCapacity
            << ",\"extra_shared_texture_bytes\":" << pipeline->extraSharedTextureBytes << "}";
        file << ",\n  \"input_full_wait\": ";
        if (!inputWait) file << "null";
        else file << "{\"requested\":" << (inputWait->requested ? "true" : "false")
            << ",\"effective_mode\":\"" << (inputWait->mode == AmfInputWaitProgressNotification
                ? "encoded_output_notification_with_10ms_fallback" : "sleep_1ms_legacy")
            << "\",\"fallback_timeout_ms\":" << inputWait->fallbackTimeoutMs
            << ",\"waits\":" << inputWait->waits
            << ",\"signaled_waits\":" << inputWait->signaledWaits
            << ",\"timed_out_waits\":" << inputWait->timedOutWaits
            << ",\"input_release_notifications\":" << inputWait->inputReleaseNotifications
            << ",\"output_notifications\":" << inputWait->outputNotifications
            << ",\"wait_ms\":{" << "\"count\":" << inputWait->wait.count
            << ",\"total\":" << inputWait->wait.totalNanoseconds / 1000000.0
            << ",\"max\":" << inputWait->wait.maxNanoseconds / 1000000.0 << "}}";
        file << ",\n  \"mf_style_nv12\": ";
        if (!mfStyleNv12) file << "null";
        else file << "{\"requested\":" << (mfStyleNv12->requested ? "true" : "false")
            << ",\"active\":" << (mfStyleNv12->active ? "true" : "false")
            << ",\"source_dxgi_format\":" << mfStyleNv12->sourceDxgiFormat
            << ",\"encoder_surface_format\":" << mfStyleNv12->encoderSurfaceFormat
            << ",\"gpu_operations_per_frame\":" << mfStyleNv12->gpuOperationsPerFrame
            << ",\"conversions\":" << mfStyleNv12->conversions
            << ",\"failures\":" << mfStyleNv12->failures
            << ",\"conversion_cpu_ms\":{\"count\":" << mfStyleNv12->conversionCpu.count
            << ",\"total\":" << mfStyleNv12->conversionCpu.totalNanoseconds / 1000000.0
            << ",\"max\":" << mfStyleNv12->conversionCpu.maxNanoseconds / 1000000.0 << "}}";
        file << ",\n  \"output_wait\": ";
        if (!outputWait) file << "null";
        else file << "{\"requested\":" << (outputWait->requested ? "true" : "false")
            << ",\"effective_mode\":\"" << (outputWait->reason >= AmfOutputWaitResetFailed ? "initialization_failed"
                : outputWait->timeoutMs ? "amf_query_timeout" : "poll_sleep_1ms")
            << "\",\"query_timeout_ms\":" << outputWait->timeoutMs
            << ",\"reason\":\"" << AmfOutputWaitReasonName(outputWait->reason)
            << "\",\"capability_result\":" << outputWait->capabilityResult
            << ",\"property_result\":" << outputWait->propertyResult
            << ",\"early_poll_waits\":" << outputWait->earlyPollWaits
            << ",\"high_resolution_poll_waits\":" << outputWait->highResolutionPollWaits << '}';
        file << ",\n  \"input_recycling\": ";
        if (!recycle) file << "null";
        else file << "{\"requested\":" << (recycle->requested ? "true" : "false")
            << ",\"pool_size\":" << recycle->poolSize << ",\"pending_output_limit\":" << recycle->maxPendingOutputs
            << ",\"inputs_in_use\":" << recycle->inputsInUse << ",\"peak_inputs_in_use\":" << recycle->peakInputsInUse
            << ",\"pending_outputs\":" << recycle->pendingOutputs << ",\"peak_pending_outputs\":" << recycle->peakPendingOutputs
            << ",\"input_releases\":" << recycle->inputReleases << ",\"output_completions\":" << recycle->outputCompletions
            << ",\"reuses_before_output\":" << recycle->reusesBeforeOutput
            << ",\"input_releases_before_output\":" << recycle->inputReleasesBeforeOutput
            << ",\"invalid_events\":" << recycle->invalidEvents
            << ",\"input_residence\":{\"count\":" << recycle->inputResidence.count
            << ",\"total_ms\":" << recycle->inputResidence.totalNanoseconds / 1000000.0
            << ",\"max_ms\":" << recycle->inputResidence.maxNanoseconds / 1000000.0 << "}}";
        file << ",\n  \"texture_pool\": ";
        if (!pool) file << "null";
        else file << "{\"requested_size\":" << pool->requestedSize << ",\"effective_size\":" << pool->effectiveSize
            << ",\"inputs_in_use\":" << pool->inputsInUse << ",\"peak_inputs_in_use\":" << pool->peakInputsInUse
            << ",\"pending_output_limit\":" << pool->pendingOutputLimit
            << ",\"nominal_texture_bytes\":" << pool->nominalBytes
            << ",\"expanded_payload_limit_bytes\":" << pool->expandedPayloadLimitBytes
            << ",\"memory_measurement\":\"estimated_pixel_payload_not_total_vram\""
            << ",\"input_exhaustion_count\":" << pool->inputExhaustionCount
            << ",\"output_budget_exhaustion_count\":" << pool->outputBudgetExhaustionCount << '}';
        file << ",\n  \"validation\": {\"decode\": \"not_run_missing_dependency\", \"frame_order\": \"not_run\", \"color\": \"not_run\", \"audio_sync\": \"not_run\"}\n"
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
        if ((options.width & 1) || (options.height & 1) || options.poolSize < 4 || options.poolSize > 128
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
        std::ofstream rawReference;
        if (!options.rawReference.empty())
        {
            EnsureParent(options.rawReference);
            rawReference.open(std::filesystem::path(options.rawReference), std::ios::binary | std::ios::trunc);
            if (!rawReference) error = L"Could not create the requested raw BGRA reference.";
        }
        // Exercise the original ABI for OFF, including in same-binary AB/BA runs.
        void* encoder = nullptr;
        if (error.empty() && options.mfStyleNv12)
            encoder = AmfCreateWithMfStyleNv12(device, options.width, options.height, options.fps,
                options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
                3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str(), options.optimizeOutputWait ? 1 : 0,
                options.recycleInput ? 1 : 0, options.asyncSubmission ? 1 : 0, options.dedicatedDevice ? 1 : 0,
                options.adaptiveInputWait ? 1 : 0, 1);
        else if (error.empty() && options.adaptiveInputWait)
            encoder = AmfCreateWithAdaptiveInputWait(device, options.width, options.height, options.fps,
                options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
                3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str(), options.optimizeOutputWait ? 1 : 0,
                options.recycleInput ? 1 : 0, options.asyncSubmission ? 1 : 0, options.dedicatedDevice ? 1 : 0, 1);
        else if (error.empty() && (options.asyncSubmission || options.dedicatedDevice))
            encoder = AmfCreateWithPipeline(device, options.width, options.height, options.fps,
                options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
                3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str(), options.optimizeOutputWait ? 1 : 0,
                options.recycleInput ? 1 : 0, options.asyncSubmission ? 1 : 0, options.dedicatedDevice ? 1 : 0);
        else if (error.empty() && options.recycleInput)
            encoder = AmfCreateWithInputRecycling(device, options.width, options.height, options.fps,
                options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
                3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str(), options.optimizeOutputWait ? 1 : 0);
        else if (error.empty() && options.optimizeOutputWait)
            encoder = AmfCreateWithOutputWait(device, options.width, options.height, options.fps,
                options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
                3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str(), 1);
        else if (error.empty()) encoder = AmfCreate(device, options.width, options.height, options.fps,
            options.bitrateKbps, options.codec, options.quality, options.rateControl, options.maxBitrateKbps,
            3, options.poolSize, options.debugLog ? 1 : 0, options.output.c_str());
        if (encoder && AmfGetLastError(encoder)[0] != L'\0') error = AmfGetLastError(encoder);
        if (!encoder && error.empty()) error = L"AmfCreate returned null";
        if (encoder && error.empty() && options.profile && !AmfEnableProfiling(encoder))
            error = L"Failed to enable profiling";

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
            if (rawReference)
                rawReference.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
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

        AmfProfileSnapshot profile{};
        bool hasProfile = encoder && options.profile && AmfGetProfile(encoder, &profile, sizeof(profile)) != 0;
        if (options.profile && !hasProfile && error.empty()) error = L"Profiling snapshot unavailable";
        AmfOutputWaitStatus outputWait{};
        const bool hasOutputWait = encoder && AmfGetOutputWaitStatus(encoder, &outputWait, sizeof(outputWait)) != 0;
        if (encoder && !hasOutputWait && error.empty()) error = L"Output-wait status unavailable";
        AmfInputRecycleStatus recycle{};
        const bool hasRecycle = encoder && AmfGetInputRecycleStatus(encoder, &recycle, sizeof(recycle));
        if (encoder && !hasRecycle && error.empty()) error = L"Input-recycling status unavailable";
        AmfTexturePoolStatus pool{};
        const bool hasPool = encoder && AmfGetTexturePoolStatus(encoder, &pool, sizeof(pool));
        if (encoder && !hasPool && error.empty()) error = L"Texture pool status unavailable";
        AmfPipelineStatus pipeline{};
        const bool hasPipeline = encoder && AmfGetPipelineStatus(encoder, &pipeline, sizeof(pipeline));
        if (encoder && (!hasPipeline || (pipeline.asyncSubmission != 0) != options.asyncSubmission
            || (pipeline.dedicatedDevice != 0) != options.dedicatedDevice || !pipeline.initialized
            || pipeline.submittedFrames != acceptedFrames || pipeline.queueDepth != 0
            || (options.asyncSubmission && pipeline.queuedFrames != acceptedFrames)) && error.empty())
            error = L"Pipeline execution status/counter mismatch";
        AmfInputWaitStatus inputWait{};
        const bool hasInputWait = encoder && AmfGetInputWaitStatus(encoder, &inputWait, sizeof(inputWait));
        if (encoder && (!hasInputWait || (inputWait.requested != 0) != options.adaptiveInputWait
            || inputWait.version != 1 || inputWait.mode != (options.adaptiveInputWait
                ? AmfInputWaitProgressNotification : AmfInputWaitLegacySleep)) && error.empty())
            error = L"Input-full wait execution status mismatch";
        AmfMfStyleNv12Status mfStyleNv12{};
        const bool hasMfStyleNv12 = encoder && AmfGetMfStyleNv12Status(encoder, &mfStyleNv12, sizeof(mfStyleNv12));
        if (encoder && (!hasMfStyleNv12 || (mfStyleNv12.requested != 0) != options.mfStyleNv12
            || (mfStyleNv12.active != 0) != options.mfStyleNv12 || mfStyleNv12.version != 1
            || (options.mfStyleNv12 && (mfStyleNv12.conversions != static_cast<uint64_t>(acceptedFrames)
                || mfStyleNv12.failures != 0))) && error.empty())
            error = L"MF-style NV12 execution status mismatch";

        if (encoder) AmfDestroy(encoder);
        if (texture) texture->Release();
        if (context) context->Release();
        if (device) device->Release();

        const bool passed = error.empty() && acceptedFrames == options.frames;
        WriteResult(options, passed ? L"passed" : L"failed", error, adapterName, runtimeVersion, wallMs, acceptedFrames,
            hasProfile ? &profile : nullptr, hasOutputWait ? &outputWait : nullptr, hasRecycle ? &recycle : nullptr,
            hasPool ? &pool : nullptr, hasPipeline ? &pipeline : nullptr, hasInputWait ? &inputWait : nullptr,
            hasMfStyleNv12 ? &mfStyleNv12 : nullptr);
        return passed ? 0 : 3;
    }

    int PipelineContractTest(const Options& options)
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        std::wstring adapter, error;
        if (!CreateDevice(&device, &context, adapter, error)) return 4;
        ID3D11Multithread* protection = nullptr;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&protection)))) return 4;
        protection->SetMultithreadProtected(FALSE); // Isolated test-owned device, never YMM4.
        EnsureParent(options.output);
        auto create = [&](bool dedicated, const wchar_t* suffix)
        {
            const std::wstring path = options.output + suffix;
            return AmfCreateWithPipeline(device, 640, 360, 60, 4000, 0, 1, 1, 4800, 3, 16, 0,
                path.c_str(), 1, 1, 1, dedicated ? 1 : 0);
        };
        int result = 0;
        void* rejected = create(false, L"-unprotected.mp4");
        if (!rejected || std::wstring(AmfGetLastError(rejected)).find(L"multithread-protected") == std::wstring::npos) result = 4;
        AmfDestroy(rejected);
        if (protection->GetMultithreadProtected()) result = 4;

        void* mismatch = create(true, L"-mismatch.mp4");
        if (!mismatch || AmfGetLastError(mismatch)[0]) result = 4;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320; desc.Height = 360; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* badTexture = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &badTexture))) result = 4;
        if (result == 0 && AmfEncode(mismatch, badTexture)) result = 4;
        const std::wstring original = AmfGetLastError(mismatch);
        if (original.find(L"dimensions") == std::wstring::npos || AmfFinalize(mismatch)
            || original != AmfGetLastError(mismatch)) result = 4;
        if (badTexture) badTexture->Release();
        AmfDestroy(mismatch);

        void* encoder = create(true, L"-host-state.mp4");
        if (!encoder || AmfGetLastError(encoder)[0]) result = 4;
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* view = nullptr;
        desc.Width = 640;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))
            || FAILED(device->CreateRenderTargetView(texture, nullptr, &view))) result = 4;
        if (result == 0)
        {
            context->OMSetRenderTargets(1, &view, nullptr);
            const float white[] = { 1, 1, 1, 1 };
            for (int i = 0; i < 16; ++i)
            {
                context->ClearRenderTargetView(view, white);
                if (!AmfEncode(encoder, texture)) { result = 4; break; }
            }
        }
        if (result == 0 && !AmfFinalize(encoder)) result = 4;
        AmfDestroy(encoder);
        ID3D11RenderTargetView* after = nullptr;
        context->OMGetRenderTargets(1, &after, nullptr);
        if (after != view || protection->GetMultithreadProtected()) result = 4;
        if (after) after->Release();
        if (view) view->Release();
        if (texture) texture->Release();
        protection->Release(); context->ClearState(); context->Flush(); context->Release(); device->Release();
        std::cout << "Pipeline source protection, invalid input, originating error, dedicated-device host state: " << (result ? "FAILED" : "passed") << '\n';
        return result;
    }

    int PipelineSelfTest()
    {
        AmfSubmissionReturnGate outputFirst, submitFirst;
        if (outputFirst.MarkOutputReceived() || !outputFirst.MarkSubmitReturned()
            || submitFirst.MarkSubmitReturned() || !submitFirst.MarkOutputReceived()) return 4;
        AmfSubmissionQueue<std::unique_ptr<int>> bounded(1);
        auto a = std::make_unique<int>(1), b = std::make_unique<int>(2);
        if (!bounded.Push(a, std::chrono::milliseconds(0)) || a) return 4;
        if (bounded.Push(b, std::chrono::milliseconds(1)) || !b || *b != 2) return 4;
        bounded.Close();
        if (bounded.Push(b, std::chrono::milliseconds(0))) return 4;
        std::unique_ptr<int> out;
        if (!bounded.Pop(out) || *out != 1 || bounded.Pop(out)) return 4;
        uint32_t depth = 0, peak = 0;
        bounded.Snapshot(depth, peak);
        if (depth != 0 || peak != 1) return 4;

        AmfSubmissionQueue<int> fifo(4);
        std::atomic_bool valid{ true };
        std::thread producer([&]()
        {
            for (int i = 0; i < 4096; ++i)
                if (!fifo.Push(i, std::chrono::seconds(2))) { valid = false; break; }
            fifo.Close();
        });
        int value = -1, expected = 0;
        while (fifo.Pop(value)) if (value != expected++) valid = false;
        producer.join(); fifo.Snapshot(depth, peak);
        if (!valid || expected != 4096 || peak > 4 || depth) return 4;

        AmfSubmissionQueue<int> empty(1);
        std::thread reader([&]() { int item = 0; if (empty.Pop(item)) valid = false; });
        empty.Abort(); reader.join();
        AmfSubmissionQueue<int> full(1);
        int first = 1, second = 2;
        full.Push(first, std::chrono::milliseconds(0));
        std::thread writer([&]() { if (full.Push(second, std::chrono::seconds(30))) valid = false; });
        full.Abort(); writer.join();
        auto cancelled = full.TakeAborted();
        if (!valid || cancelled.size() != 1 || cancelled.front() != 1 || !full.TakeAborted().empty()) return 4;
        AmfPipelineStatus status{};
        if (AmfGetPipelineStatus(nullptr, &status, sizeof(status))) return 4;
        static_assert(sizeof(AmfPipelineStatus) == 88, "Pipeline ABI size");

        AmfInputWaitSignal disabled(false);
        const auto disabledStatus = disabled.Snapshot();
        if (disabledStatus.requested || disabledStatus.mode != AmfInputWaitLegacySleep
            || disabledStatus.fallbackTimeoutMs || disabledStatus.waits) return 4;
        AmfInputWaitSignal progress(true);
        auto generation = progress.Observe();
        progress.NotifyInputRelease(); // Surface release is diagnostic only.
        if (progress.WaitForProgress(generation, std::chrono::steady_clock::now() + std::chrono::seconds(1))
            != AmfInputWaitSignal::Result::Timeout) return 4;
        generation = progress.Observe();
        progress.NotifyOutput(); // Output notification before Wait must not be lost.
        if (progress.WaitForProgress(generation, std::chrono::steady_clock::now() + std::chrono::seconds(1))
            != AmfInputWaitSignal::Result::Progress) return 4;
        AmfInputWaitSignal outputProgress(true);
        generation = outputProgress.Observe();
        // Do not assume sleep_for(1 ms) wakes inside the 10 ms fallback on
        // Windows. The generation predicate must handle notification both
        // before and after wait_until starts.
        std::thread notifier([&]() { outputProgress.NotifyOutput(); });
        const auto outputResult = outputProgress.WaitForProgress(generation,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        notifier.join();
        if (outputResult != AmfInputWaitSignal::Result::Progress
            || outputProgress.Snapshot().outputNotifications != 1) return 4;
        AmfInputWaitSignal stopping(true);
        generation = stopping.Observe();
        AmfInputWaitSignal::Result stopResult = AmfInputWaitSignal::Result::Timeout;
        std::thread waiter([&]() { stopResult = stopping.WaitForProgress(generation,
            std::chrono::steady_clock::now() + std::chrono::seconds(5)); });
        stopping.Stop(); waiter.join();
        const auto progressStatus = progress.Snapshot();
        if (stopResult != AmfInputWaitSignal::Result::Stopped || progressStatus.waits != 2
            || progressStatus.signaledWaits != 1 || progressStatus.timedOutWaits != 1
            || progressStatus.inputReleaseNotifications != 1 || progressStatus.outputNotifications != 1
            || progressStatus.wait.count != 2) return 4;
        AmfInputWaitStatus waitStatus{};
        if (AmfGetInputWaitStatus(nullptr, &waitStatus, sizeof(waitStatus))) return 4;
        static_assert(sizeof(AmfInputWaitStatus) == 80, "Input-wait ABI size");
        std::cout << "Pipeline queue/input wait: return gate, bounded FIFO 4096, close/abort, lost-wake prevention, timeout, stop wake and ABI passed\n";
        return 0;
    }

    int InputRecyclingSelfTest()
    {
        using AmfTexturePoolPolicy::Resolve;
        if (Resolve(128, 1920, 1080) != 128 || Resolve(128, 3840, 2160) != 32
            || Resolve(128, 7680, 4320) != 8 || Resolve(6, 16384, 16384) != 6
            || Resolve(999, 1920, 1080) != 128 || Resolve(-1, 1920, 1080) != 4
            || Resolve(128, 2147483647, 2147483647) != 8 || Resolve(128, 0, 0) != 8) return 4;
        // Expanded capacity must still bound input/output independently, and
        // cancelling a lease must release its exact ticket even after reuse.
        AmfInputRecyclePool expanded(128, 256);
        std::vector<AmfInputRecyclePool::Lease> heldExpanded;
        for (int i = 0; i < 128; ++i) { auto item = expanded.TryRent(); if (!item) return 4; heldExpanded.push_back(item); }
        if (expanded.Rent(std::chrono::milliseconds(0))) return 4;
        AmfTexturePoolStatus expandedStatus{};
        expanded.FillPoolStatus(expandedStatus);
        if (expandedStatus.peakInputsInUse != 128 || expandedStatus.inputExhaustionCount != 1) return 4;
        for (auto item : heldExpanded) expanded.Cancel(item.ticket, false);
        if (expanded.Snapshot().inputsInUse || expanded.Snapshot().pendingOutputs) return 4;
        for (int i = 0; i < 256; ++i)
        { const auto item = expanded.TryRent(); if (!item || !expanded.ReleaseInput(item.ticket)) return 4; }
        if (expanded.Rent(std::chrono::milliseconds(0))) return 4;
        expanded.FillPoolStatus(expandedStatus);
        if (expandedStatus.outputBudgetExhaustionCount != 1 || expandedStatus.inputsInUse != 0) return 4;
        AmfInputRecyclePool pool(2, 4);
        const auto first = pool.TryRent(), second = pool.TryRent();
        if (!first || !second || pool.TryRent()) return 4;
        if (!pool.ReleaseInput(first.ticket)) return 4;
        const auto third = pool.TryRent();
        if (!third || third.slot != first.slot || third.ticket == first.ticket) return 4;
        uint64_t elapsed = 0;
        if (!pool.CompleteOutput(first.ticket, elapsed) || pool.TryRent()) return 4;
        // The old bitstream must not release the new input occupying that slot.
        if (pool.Snapshot().inputsInUse != 2 || pool.Snapshot().reusesBeforeOutput != 1) return 4;
        if (!pool.CompleteOutput(third.ticket, elapsed) || pool.TryRent()) return 4;
        if (!pool.ReleaseInput(third.ticket)) return 4;
        if (!pool.ReleaseInput(second.ticket) || !pool.CompleteOutput(second.ticket, elapsed)) return 4;
        const auto done = pool.Snapshot();
        if (done.inputsInUse || done.pendingOutputs || done.inputReleases != 3 || done.outputCompletions != 3) return 4;

        AmfInputRecyclePool bounded(2, 2);
        for (int i = 0; i < 2; ++i) { const auto lease = bounded.TryRent(); if (!lease || !bounded.ReleaseInput(lease.ticket)) return 4; }
        if (bounded.TryRent() || bounded.Snapshot().peakPendingOutputs != 2) return 4;
        AmfInputRecyclePool cancelled(2, 4);
        auto lease = cancelled.TryRent(); cancelled.Cancel(lease.ticket, false);
        if (cancelled.Snapshot().inputsInUse || cancelled.Snapshot().pendingOutputs) return 4;
        lease = cancelled.TryRent(); cancelled.Cancel(lease.ticket, true); cancelled.Stop();
        if (!cancelled.ReleaseInput(lease.ticket) || cancelled.TryRent()) return 4;
        if (cancelled.Snapshot().inputsInUse || cancelled.Snapshot().pendingOutputs) return 4;
        // Stale/duplicate callbacks are rejected, not applied to a recycled slot.
        if (cancelled.ReleaseInput(lease.ticket) || cancelled.Snapshot().invalidEvents != 1) return 4;
        AmfInputRecyclePool invalidTicket(2, 4);
        const auto live = invalidTicket.TryRent();
        if (!live || invalidTicket.ReleaseInput(0) || invalidTicket.CompleteOutput(0, elapsed)) return 4;
        const auto rejected = invalidTicket.Snapshot();
        if (rejected.inputsInUse != 1 || rejected.pendingOutputs != 1 || rejected.invalidEvents != 2) return 4;
        if (!invalidTicket.ReleaseInput(live.ticket) || !invalidTicket.CompleteOutput(live.ticket, elapsed)) return 4;

        // Model callbacks and encoded output arriving on independent threads,
        // while the producer repeatedly reuses a bounded set of textures.
        AmfInputRecyclePool concurrent(4, 8);
        std::mutex queueMutex;
        std::condition_variable queued;
        std::deque<uint64_t> inputs, outputs;
        bool producerDone = false;
        std::atomic<bool> valid{true};
        auto consume = [&](bool input)
        {
            auto& queue = input ? inputs : outputs;
            for (;;)
            {
                uint64_t ticket = 0;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    if (!queued.wait_for(lock, std::chrono::seconds(5), [&]() { return producerDone || !queue.empty(); }))
                    { valid = false; concurrent.Stop(); return; }
                    if (queue.empty()) return;
                    ticket = queue.front(); queue.pop_front();
                }
                if ((ticket % 3) == (input ? 0u : 1u)) std::this_thread::yield();
                uint64_t residence = 0;
                if (!(input ? concurrent.ReleaseInput(ticket) : concurrent.CompleteOutput(ticket, residence)))
                { valid = false; concurrent.Stop(); return; }
            }
        };
        std::thread inputCallbacks(consume, true), encodedOutputs(consume, false);
        constexpr uint64_t iterations = 4096;
        for (uint64_t i = 0; i < iterations; ++i)
        {
            const auto item = concurrent.Rent(std::chrono::seconds(5));
            if (!item) { valid = false; break; }
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                inputs.push_back(item.ticket); outputs.push_back(item.ticket);
            }
            queued.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            producerDone = true;
        }
        queued.notify_all();
        inputCallbacks.join(); encodedOutputs.join();
        const auto concurrentDone = concurrent.Snapshot();
        if (!valid || concurrentDone.invalidEvents || concurrentDone.inputsInUse || concurrentDone.pendingOutputs
            || concurrentDone.inputReleases != iterations || concurrentDone.outputCompletions != iterations
            || concurrentDone.peakInputsInUse > 4 || concurrentDone.peakPendingOutputs > 8) return 4;

        // Stop must wake a blocked producer instead of waiting for its timeout.
        AmfInputRecyclePool stopping(1, 1);
        const auto held = stopping.TryRent();
        if (!held) return 4;
        bool wokeWithoutLease = false;
        const auto stopStart = std::chrono::steady_clock::now();
        std::thread waiter([&]() { wokeWithoutLease = !stopping.Rent(std::chrono::seconds(5)); });
        stopping.Stop(); waiter.join();
        if (!wokeWithoutLease || std::chrono::steady_clock::now() - stopStart > std::chrono::seconds(2)) return 4;
        if (!stopping.ReleaseInput(held.ticket) || !stopping.CompleteOutput(held.ticket, elapsed)) return 4;
        AmfInputRecycleStatus snapshot{};
        if (AmfGetInputRecycleStatus(nullptr, &snapshot, sizeof(snapshot))) return 4;
        std::wcout << L"Input recycling: independent lifetimes, ticket reuse, bounded outputs, cancellation, late/duplicate callbacks, 4096 concurrent leases, stop wakeup and ABI checks passed\n";
        return 0;
    }

    int OutputWaitSelfTest()
    {
        int checks = 0;
        // Each non-enabled case must leave a verified nonblocking property, or
        // refuse initialization. No hardware/driver needed on CI.
        for (int scenario = 0; scenario < 9; ++scenario)
        {
            int probes = 0, writes = 0, reads = 0;
            int64_t value = 0;
            AmfOutputWaitStatus status{};
            const bool ok = ConfigureAmfOutputWait(scenario != 0,
                [&](bool& supported) -> int32_t
                {
                    ++probes;
                    supported = scenario != 2;
                    return scenario == 3 ? 10 : 0;
                },
                [&](int64_t timeout) -> int32_t
                {
                    ++writes;
                    value = timeout;
                    if (timeout == 0 && scenario == 7) return 10;
                    return timeout != 0 && scenario == 4 ? 10 : 0;
                },
                [&](int64_t& timeout) -> int32_t
                {
                    ++reads;
                    timeout = value;
                    if (value != 0 && (scenario == 6 || scenario == 8)) timeout = 999;
                    if (value == 0 && scenario == 8) timeout = 999;
                    return value != 0 && (scenario == 5 || scenario == 7) ? 10 : 0;
                }, status);
            const uint32_t reasons[] = { AmfOutputWaitDisabled, AmfOutputWaitEnabled, AmfOutputWaitUnsupported,
                AmfOutputWaitCapsUnavailable, AmfOutputWaitPropertyRejected, AmfOutputWaitReadbackFailed,
                AmfOutputWaitReadbackMismatch, AmfOutputWaitResetFailed, AmfOutputWaitResetFailed };
            if (status.reason != reasons[scenario] || ok != (scenario < 7)) return 4;
            if (status.timeoutMs != (scenario == 1 ? static_cast<uint32_t>(kAmfOutputWaitTimeoutMs) : 0u)) return 4;
            if (scenario == 0 && (probes || writes || reads)) return 4;
            if ((scenario == 2 || scenario == 3) && (writes || reads)) return 4;
            if (scenario >= 4 && scenario <= 6 && (value != 0 || writes != 2)) return 4;
            ++checks;
        }
        AmfOutputWaitStatus status{};
        if (AmfGetOutputWaitStatus(nullptr, &status, sizeof(status))) return 4;
        std::cout << "Output-wait capability, property, readback, rollback and legacy checks: " << checks << " passed\n";
        return 0;
    }

    int OutputWaitStopTest(const Options& options)
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        std::wstring adapterName, error;
        if (!CreateDevice(&device, &context, adapterName, error)) return 4;
        int result = 0;
        for (int codec : { 0, 1 }) for (int submitted : { 0, 1, 8 })
        {
            const std::wstring path = options.output + L"-stop-" + std::to_wstring(codec) + L"-" + std::to_wstring(submitted) + L".mp4";
            EnsureParent(path);
            void* encoder = options.adaptiveInputWait
                ? AmfCreateWithAdaptiveInputWait(device, 640, 360, 60, 4000, codec, 1, 1, 4800,
                    3, options.poolSize, 0, path.c_str(), 1, options.recycleInput,
                    options.asyncSubmission, options.dedicatedDevice, 1)
                : options.asyncSubmission || options.dedicatedDevice
                ? AmfCreateWithPipeline(device, 640, 360, 60, 4000, codec, 1, 1, 4800, 3, options.poolSize, 0, path.c_str(), 1,
                    options.recycleInput, options.asyncSubmission, options.dedicatedDevice)
                : options.recycleInput
                ? AmfCreateWithInputRecycling(device, 640, 360, 60, 4000, codec, 1, 1, 4800, 3, options.poolSize, 0, path.c_str(), 1)
                : AmfCreateWithOutputWait(device, 640, 360, 60, 4000, codec, 1, 1, 4800, 3, options.poolSize, 0, path.c_str(), 1);
            if (!encoder || AmfGetLastError(encoder)[0]) result = 4;
            ID3D11Texture2D* texture = nullptr;
            if (result == 0 && submitted)
            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = 640; desc.Height = 360; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) result = 4;
                std::vector<uint8_t> pixels(640 * 360 * 4, 255);
                if (texture) context->UpdateSubresource(texture, 0, nullptr, pixels.data(), 640 * 4, 0);
                for (int i = 0; result == 0 && i < submitted; ++i)
                    if (!AmfEncode(encoder, texture)) result = 4;
            }
            if (submitted == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const auto start = std::chrono::steady_clock::now();
            if (encoder) AmfDestroy(encoder); // Intentionally no Drain/Finalize.
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms > 5000) result = 4;
            if (texture) texture->Release();
            std::cout << "Destroy without Finalize: codec=" << codec << " submitted=" << submitted << " ms=" << ms << '\n';
            if (result) break;
        }
        context->Release();
        device->Release();
        return result;
    }

    int ProfileSelfTest()
    {
        AmfProfiler profiler;
        { AmfProfileScope disabled(profiler, AmfProfileStage::SubmitInput); }
        if (profiler.Snapshot().metrics[AmfProfileStage::SubmitInput].count != 0) return 4;
        profiler.enabled = true;
        { AmfProfileScope enabled(profiler, AmfProfileStage::SubmitInput); enabled.Stop(); }
        if (profiler.Snapshot().metrics[AmfProfileStage::SubmitInput].count != 1) return 4;
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&]() { for (int j = 0; j < 10000; ++j) profiler.metrics[AmfProfileStage::SlotWait].Add(100); });
        for (auto& thread : threads) thread.join();
        const auto snapshot = profiler.Snapshot();
        const auto& metric = snapshot.metrics[AmfProfileStage::SlotWait];
        if (metric.count != 40000 || metric.totalNanoseconds != 4000000 || metric.maxNanoseconds != 100) return 4;
        AmfProfileSnapshot output{};
        if (AmfEnableProfiling(nullptr) || AmfGetProfile(nullptr, &output, sizeof(output))) return 4;
        std::cout << "Native profiling counters, disabled mode, concurrency and ABI checks: passed\n";
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        std::wcerr << L"Usage:\n"
            << L"  RadeonBench probe [--result probe.json]\n"
            << L"  RadeonBench profile-self-test\n"
            << L"  RadeonBench output-wait-self-test\n"
            << L"  RadeonBench output-wait-stop-test [--output path-prefix]\n"
            << L"  RadeonBench run [--output output.mp4] [--result run.json] [--codec h264|hevc]\n"
            << L"                  [--width N] [--height N] [--fps N] [--frames N] [--bitrate-kbps N]\n"
            << L"                  [--max-bitrate-kbps N] [--rate-control cbr|vbr] [--quality speed|balanced|quality]\n"
            << L"                  [--pool-size 4|6|8] [--audio] [--profile] [--no-debug-log] [--optimize-output-wait]\n"
            << L"                  [--raw-reference frames.bgra] (quality-analysis artifact; changes timing)\n"
            << L"                  [--mf-style-nv12] (cannot be combined with --dedicated-device)\n";
        return 1;
    }
    if (options.command == L"profile-self-test") return ProfileSelfTest();
    if (options.command == L"output-wait-self-test") return OutputWaitSelfTest();
    if (options.command == L"pipeline-self-test") return PipelineSelfTest();
    if (options.command == L"pipeline-contract-test") return PipelineContractTest(options);
    if (options.command == L"input-recycling-self-test") return InputRecyclingSelfTest();
    if (options.command == L"output-wait-stop-test") return OutputWaitStopTest(options);
    return options.command == L"probe" ? Probe(options) : Run(options);
}
