#pragma once

#include <stdint.h>

struct ID3D11Device;
struct ID3D11Texture2D;

// Append-only profiling ABI; existing encoder entry points are unchanged.
enum AmfProfileStage : uint32_t
{
    SlotWait, CopyResourceCpu, SubmitInput, InputRetryWait, QueryOutput,
    OutputPollWait, OutputMuxWait, BitstreamMux, AudioWrite, WriterSample,
    Finalize, Mp4Finalize, SlotResidence, AmfProfileStageCount
};

struct AmfProfileMetric
{
    uint64_t count;
    uint64_t totalNanoseconds;
    uint64_t maxNanoseconds;
};

struct AmfProfileSnapshot
{
    uint32_t version;
    uint32_t metricCount;
    uint64_t acceptedFrames;
    uint64_t completedFrames;
    uint64_t inputRetries;
    AmfProfileMetric metrics[AmfProfileStageCount];
};

enum AmfOutputWaitReason : uint32_t
{
    AmfOutputWaitDisabled, AmfOutputWaitEnabled, AmfOutputWaitCapsUnavailable,
    AmfOutputWaitUnsupported, AmfOutputWaitPropertyRejected, AmfOutputWaitReadbackFailed,
    AmfOutputWaitReadbackMismatch, AmfOutputWaitResetFailed, AmfOutputWaitInitializationChanged
};

struct AmfOutputWaitStatus
{
    uint32_t version;
    uint32_t requested;
    uint32_t timeoutMs;
    uint32_t reason;
    int32_t capabilityResult;
    int32_t propertyResult;
    uint64_t earlyPollWaits;
    uint64_t highResolutionPollWaits;
};

struct AmfInputRecycleStatus
{
    uint32_t version, requested, poolSize, maxPendingOutputs;
    uint32_t inputsInUse, peakInputsInUse, pendingOutputs, peakPendingOutputs;
    uint64_t inputReleases, outputCompletions, reusesBeforeOutput, inputReleasesBeforeOutput, invalidEvents;
    AmfProfileMetric inputResidence;
};

struct AmfTexturePoolStatus
{
    uint32_t version, requestedSize, effectiveSize, inputsInUse;
    uint32_t peakInputsInUse, pendingOutputLimit, reserved0, reserved1;
    uint64_t nominalBytes, expandedPayloadLimitBytes, inputExhaustionCount, outputBudgetExhaustionCount;
};

struct AmfPipelineStatus
{
    uint32_t version, asyncSubmission, dedicatedDevice, initialized;
    uint32_t queueCapacity, queueDepth, peakQueueDepth, gpuCopiesPerFrame;
    uint64_t queuedFrames, submittedFrames, extraSharedTextureBytes, adapterLuid;
    AmfProfileMetric queueResidence;
};

enum AmfInputWaitMode : uint32_t
{
    AmfInputWaitLegacySleep,
    AmfInputWaitProgressNotification,
};

struct AmfInputWaitStatus
{
    uint32_t version, requested, mode, fallbackTimeoutMs;
    uint64_t waits, signaledWaits, timedOutWaits;
    uint64_t inputReleaseNotifications, outputNotifications;
    AmfProfileMetric wait;
};

struct AmfMfStyleNv12Status
{
    uint32_t version, requested, active, sourceDxgiFormat;
    uint32_t encoderSurfaceFormat, gpuOperationsPerFrame, reserved0, reserved1;
    uint64_t conversions, failures;
    AmfProfileMetric conversionCpu;
};

extern "C" {
    __declspec(dllexport) void* AmfCreateWithPipeline(
        ID3D11Device* device, int width, int height, int fps, int bitrateKbps,
        int codec, int quality, int rateControlMode, int maxBitrateKbps,
        int surfaceFormat, int texturePoolSize, int enableDebugLog, const wchar_t* outputPath,
        int optimizeOutputWait, int recycleInput, int asyncSubmission, int dedicatedDevice);
    // Additive ABI for the input-full wait experiment. Older entry points and
    // their retry policy remain unchanged.
    __declspec(dllexport) void* AmfCreateWithAdaptiveInputWait(
        ID3D11Device* device, int width, int height, int fps, int bitrateKbps,
        int codec, int quality, int rateControlMode, int maxBitrateKbps,
        int surfaceFormat, int texturePoolSize, int enableDebugLog, const wchar_t* outputPath,
        int optimizeOutputWait, int recycleInput, int asyncSubmission, int dedicatedDevice,
        int adaptiveInputWait);
    // Additive ABI for the opt-in MF-style D3D11 VideoProcessor experiment.
    // Existing creators retain the BGRA/RGBA input path unchanged.
    __declspec(dllexport) void* AmfCreateWithMfStyleNv12(
        ID3D11Device* device, int width, int height, int fps, int bitrateKbps,
        int codec, int quality, int rateControlMode, int maxBitrateKbps,
        int surfaceFormat, int texturePoolSize, int enableDebugLog, const wchar_t* outputPath,
        int optimizeOutputWait, int recycleInput, int asyncSubmission, int dedicatedDevice,
        int adaptiveInputWait, int mfStyleNv12);
    __declspec(dllexport) int AmfGetMfStyleNv12Status(
        void* handle, AmfMfStyleNv12Status* result, uint32_t resultSize);
    __declspec(dllexport) int AmfGetInputWaitStatus(
        void* handle, AmfInputWaitStatus* result, uint32_t resultSize);
    __declspec(dllexport) int AmfGetPipelineStatus(void* handle, AmfPipelineStatus* result, uint32_t resultSize);
    __declspec(dllexport) int AmfGetTexturePoolStatus(
        void* handle, AmfTexturePoolStatus* result, uint32_t resultSize);
    __declspec(dllexport) void* AmfCreate(
        ID3D11Device* device,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int codec,
        int quality,
        int rateControlMode,
        int maxBitrateKbps,
        int surfaceFormat,
        int texturePoolSize,
        int enableDebugLog,
        const wchar_t* outputPath);

    // Additive ABI: AmfCreate keeps its original signature and polling behavior.
    __declspec(dllexport) void* AmfCreateWithOutputWait(
        ID3D11Device* device, int width, int height, int fps, int bitrateKbps,
        int codec, int quality, int rateControlMode, int maxBitrateKbps,
        int surfaceFormat, int texturePoolSize, int enableDebugLog,
        const wchar_t* outputPath, int optimizeOutputWait);
    __declspec(dllexport) int AmfGetOutputWaitStatus(
        void* handle, AmfOutputWaitStatus* result, uint32_t resultSize);

    __declspec(dllexport) void* AmfCreateWithInputRecycling(
        ID3D11Device* device, int width, int height, int fps, int bitrateKbps,
        int codec, int quality, int rateControlMode, int maxBitrateKbps,
        int surfaceFormat, int texturePoolSize, int enableDebugLog,
        const wchar_t* outputPath, int optimizeOutputWait);
    __declspec(dllexport) int AmfGetInputRecycleStatus(
        void* handle, AmfInputRecycleStatus* result, uint32_t resultSize);

    __declspec(dllexport) int AmfEncode(void* handle, ID3D11Texture2D* texture);

    __declspec(dllexport) int AmfWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels);

    __declspec(dllexport) int AmfFinalize(void* handle);

    __declspec(dllexport) void AmfDestroy(void* handle);

    __declspec(dllexport) const wchar_t* AmfGetLastError(void* handle);

    __declspec(dllexport) int AmfEnableProfiling(void* handle);
    __declspec(dllexport) int AmfGetProfile(void* handle, AmfProfileSnapshot* result, uint32_t resultSize);
}
