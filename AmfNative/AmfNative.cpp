#include "AmfNative.h"
#include "AmfProfiling.h"

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>

#include "public/include/core/Factory.h"
#include "public/include/core/Context.h"
#include "public/include/core/Buffer.h"
#include "public/include/components/Component.h"
#include "public/include/components/VideoEncoderVCE.h"
#include "public/include/components/VideoEncoderHEVC.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace
{
    constexpr int kCodecH264 = 0;
    constexpr int kCodecHevc = 1;
    constexpr int kCodecAv1 = 2;

    struct FileWriter
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        uint64_t position = 0;

        bool Open(const std::wstring& path)
        {
            handle = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            position = 0;
            return true;
        }

        bool IsOpen() const
        {
            return handle != INVALID_HANDLE_VALUE;
        }

        bool Write(const void* data, size_t size)
        {
            if (!IsOpen())
            {
                return false;
            }
            DWORD written = 0;
            if (!WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr))
            {
                return false;
            }
            if (written != size)
            {
                return false;
            }
            position += size;
            return true;
        }

        bool Seek(uint64_t pos)
        {
            if (!IsOpen())
            {
                return false;
            }
            LARGE_INTEGER li{};
            li.QuadPart = static_cast<LONGLONG>(pos);
            if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN))
            {
                return false;
            }
            position = pos;
            return true;
        }

        uint64_t Tell() const
        {
            return position;
        }

        void Close()
        {
            if (IsOpen())
            {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }
    };

    struct Mp4Buffer
    {
        std::vector<uint8_t> data;

        void WriteU8(uint8_t value) { data.push_back(value); }

        void WriteU16(uint16_t value)
        {
            data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            data.push_back(static_cast<uint8_t>(value & 0xFF));
        }

        void WriteU32(uint32_t value)
        {
            data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            data.push_back(static_cast<uint8_t>(value & 0xFF));
        }

        void WriteU24(uint32_t value)
        {
            data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            data.push_back(static_cast<uint8_t>(value & 0xFF));
        }

        void WriteU64(uint64_t value)
        {
            for (int i = 7; i >= 0; --i)
            {
                data.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }

        void WriteString4(const char* value)
        {
            data.push_back(static_cast<uint8_t>(value[0]));
            data.push_back(static_cast<uint8_t>(value[1]));
            data.push_back(static_cast<uint8_t>(value[2]));
            data.push_back(static_cast<uint8_t>(value[3]));
        }

        void WriteBytes(const std::vector<uint8_t>& bytes)
        {
            data.insert(data.end(), bytes.begin(), bytes.end());
        }

        size_t BeginBox(const char* type)
        {
            size_t start = data.size();
            WriteU32(0);
            WriteString4(type);
            return start;
        }

        void EndBox(size_t start)
        {
            uint32_t size = static_cast<uint32_t>(data.size() - start);
            data[start + 0] = static_cast<uint8_t>((size >> 24) & 0xFF);
            data[start + 1] = static_cast<uint8_t>((size >> 16) & 0xFF);
            data[start + 2] = static_cast<uint8_t>((size >> 8) & 0xFF);
            data[start + 3] = static_cast<uint8_t>(size & 0xFF);
        }
    };

    struct EncoderState
    {
        AmfProfiler profile;
        HMODULE amfModule = nullptr;
        amf::AMFFactory* factory = nullptr;
        amf_uint64 runtimeVersion = 0;
        amf::AMFContextPtr context;
        amf::AMFComponentPtr encoder;
        amf::AMF_SURFACE_FORMAT surfaceFormat = amf::AMF_SURFACE_BGRA;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* deviceContext = nullptr;
        struct FrameSlot
        {
            ID3D11Texture2D* texture = nullptr;
            amf::AMFSurfacePtr surface;
            bool inFlight = false;
            uint64_t frameId = 0;
            uint64_t profileStart = 0;

            ~FrameSlot()
            {
                surface = nullptr;
                if (texture)
                {
                    texture->Release();
                    texture = nullptr;
                }
            }
        };
        std::vector<std::unique_ptr<FrameSlot>> slots;
        size_t nextSlot = 0;
        std::mutex slotMutex;
        std::condition_variable slotCv;
        std::thread outputThread;
        std::mutex outputMutex;
        std::condition_variable outputCv;
        std::atomic_bool outputDone = false;
        std::atomic_bool outputError = false;
        std::atomic_bool drainRequested = false;
        std::atomic_bool outputStop = false;
        uint64_t acceptedFrames = 0;
        uint64_t completedFrames = 0;
        uint64_t inputFullCount = 0;
        std::mutex fileMutex;
        std::mutex muxMutex;
        std::mutex logMutex;
        std::mutex writerMutex;
        std::condition_variable writerCv;
        std::thread writerThread;
        bool writerStarted = false;
        bool writerStop = false;
        std::atomic_bool writerError = false;
        struct EncodedSample
        {
            std::vector<uint8_t> data;
            bool keyframe = false;
            bool isAudio = false;
            uint32_t audioDuration = 0;
        };
        std::deque<EncodedSample> sampleQueue;
        int width = 0;
        int height = 0;
        int fps = 30;
        uint64_t frameIndex = 0;
        bool writerInitialized = false;
        bool mp4Finalized = false;
        int codec = kCodecH264;
        FileWriter file;
        uint64_t mdatHeaderOffset = 0;
        uint64_t mdatLargeSizeOffset = 0;
        uint64_t mdatDataOffset = 0;
        std::vector<uint32_t> sampleSizes;
        std::vector<uint64_t> sampleOffsets;
        std::vector<uint32_t> syncSamples;
        std::vector<uint8_t> codecPrivate;
        bool mfStarted = false;
        bool comInitialized = false;
        bool audioInitialized = false;
        int audioSampleRate = 0;
        int audioChannels = 0;
        uint32_t audioBitrate = 192000;
        uint64_t audioSampleTotal = 0;
        uint64_t audioFrameIndex = 0;
        std::vector<int16_t> audioPcmBuffer;
        size_t audioPcmRead = 0;
        std::vector<uint32_t> audioSampleSizes;
        std::vector<uint64_t> audioSampleOffsets;
        std::vector<uint32_t> audioSampleDurations;
        std::vector<uint8_t> audioSpecificConfig;
        IMFTransform* aacEncoder = nullptr;
        std::wstring outputPath;
        std::wstring lastError;
        bool logEnabled = false;
        HANDLE logFile = INVALID_HANDLE_VALUE;
    };

    bool IsHevc(const EncoderState* state)
    {
        return state && state->codec == kCodecHevc;
    }

    bool IsAv1(const EncoderState* state)
    {
        return state && state->codec == kCodecAv1;
    }

    struct Av1SequenceHeaderInfo;
    bool ExtractAv1SequenceHeaderObu(const uint8_t* data, size_t size, std::vector<uint8_t>& outObu, Av1SequenceHeaderInfo& info);
    std::vector<uint8_t> BuildAv1CFromSequenceObu(const std::vector<uint8_t>& seqObu, const Av1SequenceHeaderInfo& info);

    std::vector<uint8_t> BuildAacSpecificConfig(int sampleRate, int channels);
    bool InitializeMp4Writer(EncoderState* state, int codec, const std::vector<uint8_t>& codecPrivate);
    bool ProcessEncodedBitstream(EncoderState* state, const uint8_t* data, size_t size, bool keyframeHint);
    void OpenLog(EncoderState* state);
    void CloseLog(EncoderState* state);
    void LogLine(EncoderState* state, const std::wstring& line);
    bool ProcessAudioOutput(EncoderState* state);
    bool EncodeAudioFrame(EncoderState* state, const int16_t* pcm, uint32_t frameSamplesPerChannel);
    bool FlushAudio(EncoderState* state);
    bool InitializeAmfEncoder(EncoderState* state, ID3D11Device* device, int width, int height, int fps,
        int bitrateKbps, int codec, int quality, int rateControlMode, int maxBitrateKbps,
        amf::AMF_SURFACE_FORMAT surfaceFormat, int texturePoolSize);
    bool EncodeTexture(EncoderState* state, ID3D11Texture2D* texture);
    void StartOutputThread(EncoderState* state);
    void StopOutputThread(EncoderState* state, bool flush);
    void StartWriterThread(EncoderState* state);
    void StopWriterThread(EncoderState* state);

    void SetError(EncoderState* state, const std::wstring& message)
    {
        if (state)
        {
            state->lastError = message;
            LogLine(state, L"[error] " + message);
        }
    }

    bool CheckStatus(EncoderState* state, AMF_RESULT status, const wchar_t* message)
    {
        if (status == AMF_OK)
        {
            return true;
        }

        std::wstring error = message;
        error += L" (";
        error += std::to_wstring(static_cast<int>(status));
        error += L")";
        SetError(state, error);
        return false;
    }

    void OpenLog(EncoderState* state)
    {
        if (!state || !state->logEnabled || state->logFile != INVALID_HANDLE_VALUE || state->outputPath.empty())
        {
            return;
        }

        std::wstring path = state->outputPath + L".amf_log.txt";
        state->logFile = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }

    void CloseLog(EncoderState* state)
    {
        if (!state || state->logFile == INVALID_HANDLE_VALUE)
        {
            return;
        }
        CloseHandle(state->logFile);
        state->logFile = INVALID_HANDLE_VALUE;
    }

    void LogLine(EncoderState* state, const std::wstring& line)
    {
        if (!state || !state->logEnabled)
        {
            return;
        }
        if (state->logFile == INVALID_HANDLE_VALUE)
        {
            OpenLog(state);
        }
        if (state->logFile == INVALID_HANDLE_VALUE)
        {
            return;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t prefix[64]{};
        swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [t%lu] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId());

        std::wstring full = prefix + line + L"\r\n";
        int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()), nullptr, 0, nullptr, nullptr);
        if (bytesNeeded <= 0)
        {
            return;
        }

        std::string utf8(bytesNeeded, '\0');
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()), &utf8[0], bytesNeeded, nullptr, nullptr);

        std::lock_guard<std::mutex> lock(state->logMutex);
        DWORD written = 0;
        WriteFile(state->logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    int ClampInt(int value, int minValue, int maxValue)
    {
        if (value < minValue) return minValue;
        if (value > maxValue) return maxValue;
        return value;
    }

    float ClampFloat(float value, float minValue, float maxValue)
    {
        if (value < minValue) return minValue;
        if (value > maxValue) return maxValue;
        return value;
    }

    uint64_t MaxU64(uint64_t a, uint64_t b)
    {
        return a > b ? a : b;
    }

    bool WriteU32BE(FileWriter& file, uint32_t value)
    {
        uint8_t bytes[4] = {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        return file.Write(bytes, sizeof(bytes));
    }

    bool WriteU64BE(FileWriter& file, uint64_t value)
    {
        uint8_t bytes[8];
        for (int i = 7; i >= 0; --i)
        {
            bytes[7 - i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
        }
        return file.Write(bytes, sizeof(bytes));
    }

    bool WriteString4(FileWriter& file, const char* value)
    {
        return file.Write(value, 4);
    }

    bool WriteFtyp(FileWriter& file, int codec)
    {
        const char* brand = "avc1";
        if (codec == kCodecHevc)
        {
            brand = "hvc1";
        }
        else if (codec == kCodecAv1)
        {
            brand = "av01";
        }
        const uint32_t boxSize = 32;
        return WriteU32BE(file, boxSize)
            && WriteString4(file, "ftyp")
            && WriteString4(file, "isom")
            && WriteU32BE(file, 0x00000200)
            && WriteString4(file, "isom")
            && WriteString4(file, "iso2")
            && WriteString4(file, brand)
            && WriteString4(file, "mp41");
    }

    bool InitializeMp4Writer(EncoderState* state, int codec, const std::vector<uint8_t>& codecPrivate)
    {
        if (state->writerInitialized)
        {
            if (!codecPrivate.empty() && state->codecPrivate.empty())
            {
                state->codecPrivate = codecPrivate;
            }
            return true;
        }

        if (!state->file.Open(state->outputPath))
        {
            SetError(state, L"Failed to open output file.");
            return false;
        }

        state->codec = codec;
        if (!codecPrivate.empty())
        {
            state->codecPrivate = codecPrivate;
        }

        if (!WriteFtyp(state->file, codec))
        {
            SetError(state, L"Failed to write ftyp.");
            return false;
        }

        state->mdatHeaderOffset = state->file.Tell();
        if (!WriteU32BE(state->file, 1) || !WriteString4(state->file, "mdat"))
        {
            SetError(state, L"Failed to write mdat header.");
            return false;
        }

        state->mdatLargeSizeOffset = state->file.Tell();
        if (!WriteU64BE(state->file, 0))
        {
            SetError(state, L"Failed to write mdat size.");
            return false;
        }

        state->mdatDataOffset = state->file.Tell();
        state->writerInitialized = true;
        return true;
    }

    bool InitializeAudioEncoder(EncoderState* state, int sampleRate, int channels)
    {
        if (!state)
        {
            return false;
        }

        if (state->audioInitialized)
        {
            if (state->audioSampleRate != sampleRate || state->audioChannels != channels)
            {
                SetError(state, L"Audio format mismatch.");
                return false;
            }
            return true;
        }

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr))
        {
            SetError(state, L"MFStartup failed.");
            return false;
        }
        state->mfStarted = true;

        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr))
        {
            state->comInitialized = true;
        }

        MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Audio, MFAudioFormat_PCM };
        MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Audio, MFAudioFormat_AAC };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(
            MFT_CATEGORY_AUDIO_ENCODER,
            MFT_ENUM_FLAG_ALL,
            &inputType,
            &outputType,
            &activates,
            &count);

        if (FAILED(hr) || count == 0)
        {
            if (activates)
            {
                CoTaskMemFree(activates);
            }
            SetError(state, L"AAC encoder not found.");
            return false;
        }

        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&state->aacEncoder));
        for (UINT32 i = 0; i < count; ++i)
        {
            activates[i]->Release();
        }
        CoTaskMemFree(activates);

        if (FAILED(hr))
        {
            SetError(state, L"Failed to activate AAC encoder.");
            return false;
        }

        IMFMediaType* inType = nullptr;
        hr = MFCreateMediaType(&inType);
        if (FAILED(hr))
        {
            SetError(state, L"MFCreateMediaType failed.");
            return false;
        }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(channels * 2));
        inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(sampleRate * channels * 2));

        hr = state->aacEncoder->SetInputType(0, inType, 0);
        inType->Release();
        if (FAILED(hr))
        {
            SetError(state, L"AAC SetInputType failed.");
            return false;
        }

        IMFMediaType* outType = nullptr;
        hr = MFCreateMediaType(&outType);
        if (FAILED(hr))
        {
            SetError(state, L"MFCreateMediaType failed.");
            return false;
        }
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, state->audioBitrate / 8);
        outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        outType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        hr = state->aacEncoder->SetOutputType(0, outType, 0);
        outType->Release();
        if (FAILED(hr))
        {
            SetError(state, L"AAC SetOutputType failed.");
            return false;
        }

        state->audioSpecificConfig = BuildAacSpecificConfig(sampleRate, channels);

        state->aacEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        state->aacEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        state->audioSampleRate = sampleRate;
        state->audioChannels = channels;
        state->audioInitialized = true;
        return true;
    }

    bool ProcessAudioOutput(EncoderState* state)
    {
        if (!state || !state->aacEncoder)
        {
            return false;
        }

        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = state->aacEncoder->GetOutputStreamInfo(0, &info);
        if (FAILED(hr))
        {
            SetError(state, L"AAC GetOutputStreamInfo failed.");
            return false;
        }
        if (info.cbSize == 0)
        {
            info.cbSize = 4096;
        }

        while (true)
        {
            IMFSample* outSample = nullptr;
            IMFMediaBuffer* buffer = nullptr;

            hr = MFCreateSample(&outSample);
            if (FAILED(hr))
            {
                SetError(state, L"MFCreateSample failed.");
                return false;
            }

            hr = MFCreateMemoryBuffer(info.cbSize, &buffer);
            if (FAILED(hr))
            {
                outSample->Release();
                SetError(state, L"MFCreateMemoryBuffer failed.");
                return false;
            }
            outSample->AddBuffer(buffer);
            buffer->Release();

            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = outSample;
            DWORD status = 0;
            hr = state->aacEncoder->ProcessOutput(0, 1, &output, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                IMFMediaType* newType = nullptr;
                if (SUCCEEDED(state->aacEncoder->GetOutputAvailableType(0, 0, &newType)))
                {
                    state->aacEncoder->SetOutputType(0, newType, 0);
                    newType->Release();
                }
                outSample->Release();
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                outSample->Release();
                LogLine(state, L"audio output need more input");
                break;
            }
            if (FAILED(hr))
            {
                outSample->Release();
                SetError(state, L"AAC ProcessOutput failed.");
                return false;
            }

            IMFMediaBuffer* outBuffer = nullptr;
            hr = outSample->GetBufferByIndex(0, &outBuffer);
            if (FAILED(hr))
            {
                outSample->Release();
                SetError(state, L"AAC GetBuffer failed.");
                return false;
            }

            BYTE* data = nullptr;
            DWORD maxLen = 0;
            DWORD curLen = 0;
            hr = outBuffer->Lock(&data, &maxLen, &curLen);
            if (FAILED(hr))
            {
                outBuffer->Release();
                outSample->Release();
                SetError(state, L"AAC buffer lock failed.");
                return false;
            }

            if (curLen > 0)
            {
                if (!state->writerStarted)
                {
                    StartWriterThread(state);
                }
                if (state->writerError)
                {
                    outBuffer->Unlock();
                    outBuffer->Release();
                    outSample->Release();
                    SetError(state, L"Writer thread error.");
                    return false;
                }

                std::vector<uint8_t> payload(data, data + curLen);
                {
                    std::lock_guard<std::mutex> lock(state->writerMutex);
                    state->sampleQueue.push_back({ std::move(payload), false, true, 1024 });
                }
                state->writerCv.notify_one();
            }

            outBuffer->Unlock();
            outBuffer->Release();
            outSample->Release();
        }

        return true;
    }

    bool EncodeAudioFrame(EncoderState* state, const int16_t* pcm, uint32_t frameSamplesPerChannel)
    {
        if (!state || !state->aacEncoder)
        {
            return false;
        }

        const uint32_t channels = static_cast<uint32_t>(state->audioChannels);
        const uint32_t sampleCount = frameSamplesPerChannel * channels;
        const uint32_t byteCount = sampleCount * 2;

        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        HRESULT hr = MFCreateSample(&sample);
        if (FAILED(hr))
        {
            SetError(state, L"MFCreateSample failed.");
            return false;
        }

        hr = MFCreateMemoryBuffer(byteCount, &buffer);
        if (FAILED(hr))
        {
            sample->Release();
            SetError(state, L"MFCreateMemoryBuffer failed.");
            return false;
        }

        BYTE* dest = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        hr = buffer->Lock(&dest, &maxLen, &curLen);
        if (FAILED(hr))
        {
            buffer->Release();
            sample->Release();
            SetError(state, L"Audio buffer lock failed.");
            return false;
        }

        memcpy(dest, pcm, byteCount);
        buffer->Unlock();
        buffer->SetCurrentLength(byteCount);
        sample->AddBuffer(buffer);
        buffer->Release();

        const LONGLONG duration = static_cast<LONGLONG>(frameSamplesPerChannel) * 10000000LL / state->audioSampleRate;
        const LONGLONG time = static_cast<LONGLONG>(state->audioFrameIndex) * duration;
        sample->SetSampleTime(time);
        sample->SetSampleDuration(duration);
        state->audioFrameIndex++;

        hr = state->aacEncoder->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING)
        {
            if (!ProcessAudioOutput(state))
            {
                sample->Release();
                return false;
            }
            hr = state->aacEncoder->ProcessInput(0, sample, 0);
        }
        sample->Release();

        if (FAILED(hr))
        {
            SetError(state, L"AAC ProcessInput failed.");
            return false;
        }

        if (!ProcessAudioOutput(state))
        {
            return false;
        }

        return true;
    }

    bool FlushAudio(EncoderState* state)
    {
        if (!state || !state->audioInitialized || !state->aacEncoder)
        {
            return true;
        }

        LogLine(state, L"flush audio start");
        const uint32_t frameSamples = 1024;
        const uint32_t channels = static_cast<uint32_t>(state->audioChannels);
        const uint32_t frameCount = frameSamples * channels;

        if (state->audioPcmBuffer.size() > state->audioPcmRead)
        {
            size_t remain = state->audioPcmBuffer.size() - state->audioPcmRead;
            std::vector<int16_t> frame(frameCount, 0);
            size_t toCopy = std::min<size_t>(remain, frameCount);
            memcpy(frame.data(), state->audioPcmBuffer.data() + state->audioPcmRead, toCopy * sizeof(int16_t));
            if (!EncodeAudioFrame(state, frame.data(), frameSamples))
            {
                return false;
            }
            state->audioPcmRead += toCopy;
        }

        state->aacEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (!ProcessAudioOutput(state))
        {
            return false;
        }

        LogLine(state, L"flush audio done");
        return true;
    }

    void WriteMatrix(Mp4Buffer& buffer)
    {
        buffer.WriteU32(0x00010000);
        buffer.WriteU32(0);
        buffer.WriteU32(0);
        buffer.WriteU32(0);
        buffer.WriteU32(0x00010000);
        buffer.WriteU32(0);
        buffer.WriteU32(0);
        buffer.WriteU32(0);
        buffer.WriteU32(0x40000000);
    }

    void WriteDescriptorSize(Mp4Buffer& buffer, size_t size)
    {
        uint8_t bytes[4] = {};
        int count = 0;
        do
        {
            bytes[count++] = static_cast<uint8_t>(size & 0x7F);
            size >>= 7;
        } while (size > 0 && count < 4);

        for (int i = count - 1; i >= 0; --i)
        {
            uint8_t value = bytes[i];
            if (i != 0)
            {
                value |= 0x80;
            }
            buffer.WriteU8(value);
        }
    }

    void WriteDescriptor(Mp4Buffer& buffer, uint8_t tag, const std::vector<uint8_t>& payload)
    {
        buffer.WriteU8(tag);
        WriteDescriptorSize(buffer, payload.size());
        buffer.WriteBytes(payload);
    }

    std::vector<uint8_t> BuildAacSpecificConfig(int sampleRate, int channels)
    {
        int sampleRateIndex = 3;
        struct RateMap { int rate; int index; };
        const RateMap rates[] = {
            { 96000, 0 }, { 88200, 1 }, { 64000, 2 }, { 48000, 3 }, { 44100, 4 }, { 32000, 5 },
            { 24000, 6 }, { 22050, 7 }, { 16000, 8 }, { 12000, 9 }, { 11025, 10 }, { 8000, 11 }, { 7350, 12 }
        };
        for (const auto& r : rates)
        {
            if (r.rate == sampleRate)
            {
                sampleRateIndex = r.index;
                break;
            }
        }

        const uint8_t audioObjectType = 2; // AAC LC
        const uint8_t channelConfig = static_cast<uint8_t>(ClampInt(channels, 1, 7));

        std::vector<uint8_t> asc;
        asc.resize(2);
        asc[0] = static_cast<uint8_t>((audioObjectType << 3) | ((sampleRateIndex & 0x0E) >> 1));
        asc[1] = static_cast<uint8_t>(((sampleRateIndex & 0x01) << 7) | (channelConfig << 3));
        return asc;
    }

    std::vector<uint8_t> BuildEsds(const std::vector<uint8_t>& asc, uint32_t bitrate)
    {
        Mp4Buffer esds;
        esds.WriteU32(0);

        Mp4Buffer decSpecific;
        decSpecific.WriteBytes(asc);

        Mp4Buffer decConfig;
        decConfig.WriteU8(0x40); // objectTypeIndication
        decConfig.WriteU8(0x15); // streamType audio
        decConfig.WriteU24(0);   // bufferSizeDB
        decConfig.WriteU32(bitrate);
        decConfig.WriteU32(bitrate);
        WriteDescriptor(decConfig, 0x05, decSpecific.data);

        Mp4Buffer slConfig;
        slConfig.WriteU8(0x02);

        Mp4Buffer esDesc;
        esDesc.WriteU16(1);
        esDesc.WriteU8(0);
        WriteDescriptor(esDesc, 0x04, decConfig.data);
        WriteDescriptor(esDesc, 0x06, slConfig.data);

        WriteDescriptor(esds, 0x03, esDesc.data);
        return esds.data;
    }

    void WriteStts(Mp4Buffer& buffer, const std::vector<uint32_t>& durations)
    {
        size_t sttsStart = buffer.BeginBox("stts");
        buffer.WriteU32(0);
        if (durations.empty())
        {
            buffer.WriteU32(0);
            buffer.EndBox(sttsStart);
            return;
        }

        struct Entry { uint32_t count; uint32_t duration; };
        std::vector<Entry> entries;
        for (uint32_t d : durations)
        {
            if (entries.empty() || entries.back().duration != d)
            {
                entries.push_back({ 1, d });
            }
            else
            {
                entries.back().count++;
            }
        }

        buffer.WriteU32(static_cast<uint32_t>(entries.size()));
        for (const auto& e : entries)
        {
            buffer.WriteU32(e.count);
            buffer.WriteU32(e.duration);
        }
        buffer.EndBox(sttsStart);
    }

    void AppendAudioTrak(Mp4Buffer& moov, const EncoderState* state, uint32_t trackId)
    {
        const uint32_t timescale = static_cast<uint32_t>(state->audioSampleRate);
        const uint64_t duration = state->audioSampleTotal;
        const uint32_t sampleCount = static_cast<uint32_t>(state->audioSampleSizes.size());
        const uint32_t channels = static_cast<uint32_t>(state->audioChannels);

        size_t trakStart = moov.BeginBox("trak");

        size_t tkhdStart = moov.BeginBox("tkhd");
        moov.WriteU32(0x00000007);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(trackId);
        moov.WriteU32(0);
        moov.WriteU32(static_cast<uint32_t>(duration));
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU16(0x0100);
        moov.WriteU16(0);
        WriteMatrix(moov);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.EndBox(tkhdStart);

        size_t mdiaStart = moov.BeginBox("mdia");

        size_t mdhdStart = moov.BeginBox("mdhd");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(timescale);
        moov.WriteU32(static_cast<uint32_t>(duration));
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.EndBox(mdhdStart);

        size_t hdlrStart = moov.BeginBox("hdlr");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteString4("soun");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        const char handlerName[] = "SoundHandler";
        moov.data.insert(moov.data.end(), handlerName, handlerName + sizeof(handlerName));
        moov.EndBox(hdlrStart);

        size_t minfStart = moov.BeginBox("minf");

        size_t smhdStart = moov.BeginBox("smhd");
        moov.WriteU32(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.EndBox(smhdStart);

        size_t dinfStart = moov.BeginBox("dinf");
        size_t drefStart = moov.BeginBox("dref");
        moov.WriteU32(0);
        moov.WriteU32(1);
        size_t urlStart = moov.BeginBox("url ");
        moov.WriteU32(0x00000001);
        moov.EndBox(urlStart);
        moov.EndBox(drefStart);
        moov.EndBox(dinfStart);

        size_t stblStart = moov.BeginBox("stbl");

        size_t stsdStart = moov.BeginBox("stsd");
        moov.WriteU32(0);
        moov.WriteU32(1);
        size_t mp4aStart = moov.BeginBox("mp4a");
        for (int i = 0; i < 6; ++i) moov.WriteU8(0);
        moov.WriteU16(1);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU32(0);
        moov.WriteU16(static_cast<uint16_t>(channels));
        moov.WriteU16(16);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU32(static_cast<uint32_t>(timescale) << 16);

        auto esds = BuildEsds(state->audioSpecificConfig, state->audioBitrate);
        size_t esdsStart = moov.BeginBox("esds");
        moov.WriteBytes(esds);
        moov.EndBox(esdsStart);

        moov.EndBox(mp4aStart);
        moov.EndBox(stsdStart);

        WriteStts(moov, state->audioSampleDurations);

        size_t stscStart = moov.BeginBox("stsc");
        moov.WriteU32(0);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.EndBox(stscStart);

        size_t stszStart = moov.BeginBox("stsz");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(sampleCount);
        for (uint32_t size : state->audioSampleSizes)
        {
            moov.WriteU32(size);
        }
        moov.EndBox(stszStart);

        bool useCo64 = false;
        for (uint64_t offset : state->audioSampleOffsets)
        {
            if (offset > 0xFFFFFFFFu)
            {
                useCo64 = true;
                break;
            }
        }
        size_t stcoStart = moov.BeginBox(useCo64 ? "co64" : "stco");
        moov.WriteU32(0);
        moov.WriteU32(sampleCount);
        if (useCo64)
        {
            for (uint64_t offset : state->audioSampleOffsets)
            {
                moov.WriteU64(offset);
            }
        }
        else
        {
            for (uint64_t offset : state->audioSampleOffsets)
            {
                moov.WriteU32(static_cast<uint32_t>(offset));
            }
        }
        moov.EndBox(stcoStart);

        moov.EndBox(stblStart);
        moov.EndBox(minfStart);
        moov.EndBox(mdiaStart);
        moov.EndBox(trakStart);
    }

    std::vector<uint8_t> BuildMoov(const EncoderState* state)
    {
        Mp4Buffer moov;

        const uint32_t timescale = 90000;
        const uint32_t fps = state->fps > 0 ? static_cast<uint32_t>(state->fps) : 30;
        const uint32_t frameDuration = timescale / fps;
        const uint32_t sampleCount = static_cast<uint32_t>(state->sampleSizes.size());
        const uint64_t videoDuration = static_cast<uint64_t>(frameDuration) * sampleCount;
        uint64_t audioDuration = 0;
        if (state->audioSampleRate > 0)
        {
            audioDuration = state->audioSampleTotal * timescale / static_cast<uint64_t>(state->audioSampleRate);
        }
        const uint64_t duration = MaxU64(videoDuration, audioDuration);

        size_t moovStart = moov.BeginBox("moov");

        size_t mvhdStart = moov.BeginBox("mvhd");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(timescale);
        moov.WriteU32(static_cast<uint32_t>(duration));
        moov.WriteU32(0x00010000);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        WriteMatrix(moov);
        for (int i = 0; i < 6; ++i)
        {
            moov.WriteU32(0);
        }
        uint32_t nextTrackId = state->audioSampleSizes.empty() ? 2 : 3;
        moov.WriteU32(nextTrackId);
        moov.EndBox(mvhdStart);

        size_t trakStart = moov.BeginBox("trak");

        size_t tkhdStart = moov.BeginBox("tkhd");
        moov.WriteU32(0x00000007);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(1);
        moov.WriteU32(0);
        moov.WriteU32(static_cast<uint32_t>(duration));
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        WriteMatrix(moov);
        moov.WriteU32(static_cast<uint32_t>(state->width) << 16);
        moov.WriteU32(static_cast<uint32_t>(state->height) << 16);
        moov.EndBox(tkhdStart);

        size_t mdiaStart = moov.BeginBox("mdia");

        size_t mdhdStart = moov.BeginBox("mdhd");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(timescale);
        moov.WriteU32(static_cast<uint32_t>(duration));
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.EndBox(mdhdStart);

        size_t hdlrStart = moov.BeginBox("hdlr");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteString4("vide");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        const char handlerName[] = "VideoHandler";
        moov.data.insert(moov.data.end(), handlerName, handlerName + sizeof(handlerName));
        moov.EndBox(hdlrStart);

        size_t minfStart = moov.BeginBox("minf");

        size_t vmhdStart = moov.BeginBox("vmhd");
        moov.WriteU32(0x00000001);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.EndBox(vmhdStart);

        size_t dinfStart = moov.BeginBox("dinf");
        size_t drefStart = moov.BeginBox("dref");
        moov.WriteU32(0);
        moov.WriteU32(1);
        size_t urlStart = moov.BeginBox("url ");
        moov.WriteU32(0x00000001);
        moov.EndBox(urlStart);
        moov.EndBox(drefStart);
        moov.EndBox(dinfStart);

        size_t stblStart = moov.BeginBox("stbl");

        size_t stsdStart = moov.BeginBox("stsd");
        moov.WriteU32(0);
        moov.WriteU32(1);
        const char* sampleType = "avc1";
        const char* codecBox = "avcC";
        if (IsHevc(state))
        {
            sampleType = "hvc1";
            codecBox = "hvcC";
        }
        else if (IsAv1(state))
        {
            sampleType = "av01";
            codecBox = "av1C";
        }
        size_t sampleEntryStart = moov.BeginBox(sampleType);
        for (int i = 0; i < 6; ++i) moov.WriteU8(0);
        moov.WriteU16(1);
        moov.WriteU16(0);
        moov.WriteU16(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU16(static_cast<uint16_t>(state->width));
        moov.WriteU16(static_cast<uint16_t>(state->height));
        moov.WriteU32(0x00480000);
        moov.WriteU32(0x00480000);
        moov.WriteU32(0);
        moov.WriteU16(1);
        moov.WriteU8(0);
        for (int i = 0; i < 31; ++i) moov.WriteU8(0);
        moov.WriteU16(0x0018);
        moov.WriteU16(0xFFFF);

        size_t codecBoxStart = moov.BeginBox(codecBox);
        moov.WriteBytes(state->codecPrivate);
        moov.EndBox(codecBoxStart);

        moov.EndBox(sampleEntryStart);
        moov.EndBox(stsdStart);

        size_t sttsStart = moov.BeginBox("stts");
        moov.WriteU32(0);
        moov.WriteU32(1);
        moov.WriteU32(sampleCount);
        moov.WriteU32(frameDuration);
        moov.EndBox(sttsStart);

        size_t stscStart = moov.BeginBox("stsc");
        moov.WriteU32(0);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.WriteU32(1);
        moov.EndBox(stscStart);

        size_t stszStart = moov.BeginBox("stsz");
        moov.WriteU32(0);
        moov.WriteU32(0);
        moov.WriteU32(sampleCount);
        for (uint32_t size : state->sampleSizes)
        {
            moov.WriteU32(size);
        }
        moov.EndBox(stszStart);

        bool useCo64 = false;
        for (uint64_t offset : state->sampleOffsets)
        {
            if (offset > 0xFFFFFFFFu)
            {
                useCo64 = true;
                break;
            }
        }

        size_t stcoStart = moov.BeginBox(useCo64 ? "co64" : "stco");
        moov.WriteU32(0);
        moov.WriteU32(sampleCount);
        if (useCo64)
        {
            for (uint64_t offset : state->sampleOffsets)
            {
                moov.WriteU64(offset);
            }
        }
        else
        {
            for (uint64_t offset : state->sampleOffsets)
            {
                moov.WriteU32(static_cast<uint32_t>(offset));
            }
        }
        moov.EndBox(stcoStart);

        if (!state->syncSamples.empty())
        {
            size_t stssStart = moov.BeginBox("stss");
            moov.WriteU32(0);
            moov.WriteU32(static_cast<uint32_t>(state->syncSamples.size()));
            for (uint32_t sampleIndex : state->syncSamples)
            {
                moov.WriteU32(sampleIndex);
            }
            moov.EndBox(stssStart);
        }

        moov.EndBox(stblStart);
        moov.EndBox(minfStart);
        moov.EndBox(mdiaStart);
        moov.EndBox(trakStart);

        if (!state->audioSampleSizes.empty() && !state->audioSpecificConfig.empty())
        {
            AppendAudioTrak(moov, state, 2);
        }
        moov.EndBox(moovStart);

        return moov.data;
    }

    bool FinalizeMp4(EncoderState* state)
    {
        AmfProfileScope profileScope(state->profile, AmfProfileStage::Mp4Finalize);
        if (!state->writerInitialized || state->mp4Finalized)
        {
            return true;
        }

        LogLine(state, L"finalize mp4 start");
        if (!FlushAudio(state))
        {
            return false;
        }
        StopWriterThread(state);
        if (state->writerError)
        {
            if (state->lastError.empty())
            {
                SetError(state, L"Writer thread error.");
            }
            return false;
        }

        if (state->codecPrivate.empty())
        {
            if (state->lastError.empty())
            {
                SetError(state, L"Video codec header not found.");
            }
            else
            {
                LogLine(state, L"[error] Video codec header not found (secondary error; preserving the earlier failure).");
            }
            return false;
        }

        uint64_t dataEnd = state->file.Tell();

        auto moov = BuildMoov(state);
        if (!state->file.Write(moov.data(), moov.size()))
        {
            SetError(state, L"Failed to write moov.");
            return false;
        }

        uint64_t fileSize = state->file.Tell();
        uint64_t mdatSize = dataEnd - state->mdatHeaderOffset;
        if (!state->file.Seek(state->mdatLargeSizeOffset) || !WriteU64BE(state->file, mdatSize))
        {
            SetError(state, L"Failed to update mdat size.");
            return false;
        }

        state->file.Seek(fileSize);
        state->file.Close();
        state->mp4Finalized = true;
        LogLine(state, L"finalize mp4 done");
        return true;
    }

    struct NalUnit
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        uint8_t type = 0;
    };

    std::vector<NalUnit> ParseAnnexB(const uint8_t* data, size_t size, bool hevc)
    {
        std::vector<NalUnit> units;
        size_t i = 0;
        auto findStart = [&](size_t from) -> size_t
        {
            for (size_t j = from; j + 3 < size; ++j)
            {
                if (data[j] == 0 && data[j + 1] == 0)
                {
                    if (data[j + 2] == 1)
                    {
                        return j;
                    }
                    if (j + 3 < size && data[j + 2] == 0 && data[j + 3] == 1)
                    {
                        return j;
                    }
                }
            }
            return size;
        };

        while (i < size)
        {
            size_t start = findStart(i);
            if (start >= size)
            {
                break;
            }
            size_t scSize = (data[start + 2] == 1) ? 3 : 4;
            size_t nalStart = start + scSize;
            size_t next = findStart(nalStart);
            size_t nalEnd = (next < size) ? next : size;
            if (nalEnd > nalStart)
            {
                uint8_t type = 0;
                if (hevc)
                {
                    type = (data[nalStart] >> 1) & 0x3F;
                }
                else
                {
                    type = data[nalStart] & 0x1F;
                }
                units.push_back({ data + nalStart, nalEnd - nalStart, type });
            }
            i = nalEnd;
        }
        return units;
    }

    std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
    {
        if (sps.size() < 4)
        {
            return {};
        }
        std::vector<uint8_t> avcc;
        avcc.push_back(1);
        avcc.push_back(sps[1]);
        avcc.push_back(sps[2]);
        avcc.push_back(sps[3]);
        avcc.push_back(0xFF); // lengthSizeMinusOne=3
        avcc.push_back(0xE1); // numOfSPS=1
        avcc.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
        avcc.insert(avcc.end(), sps.begin(), sps.end());
        avcc.push_back(1); // numOfPPS=1
        avcc.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
        avcc.insert(avcc.end(), pps.begin(), pps.end());
        return avcc;
    }

    std::vector<uint8_t> BuildHvcC(const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
    {
        // Minimal hvcC. Many fields are set to defaults; VPS/SPS/PPS are included.
        std::vector<uint8_t> hvcc;
        hvcc.reserve(64 + vps.size() + sps.size() + pps.size());

        hvcc.push_back(1); // configurationVersion
        hvcc.push_back(1); // general_profile_space(0), tier(0), profile_idc(1=Main)
        hvcc.insert(hvcc.end(), 4, 0); // general_profile_compatibility_flags
        hvcc.insert(hvcc.end(), 6, 0); // general_constraint_indicator_flags
        hvcc.push_back(120); // general_level_idc (4.0)
        hvcc.push_back(0xF0); // min_spatial_segmentation_idc (upper 4 bits set)
        hvcc.push_back(0);
        hvcc.push_back(0xFC); // parallelismType (reserved)
        hvcc.push_back(0xFC); // chromaFormat (reserved)
        hvcc.push_back(0xF8); // bitDepthLumaMinus8 (reserved)
        hvcc.push_back(0xF8); // bitDepthChromaMinus8 (reserved)
        hvcc.push_back(0); // avgFrameRate
        hvcc.push_back(0);
        hvcc.push_back(0x03); // constantFrameRate=0, numTemporalLayers=0, temporalIdNested=0, lengthSizeMinusOne=3

        uint8_t numArrays = 0;
        if (!vps.empty()) numArrays++;
        if (!sps.empty()) numArrays++;
        if (!pps.empty()) numArrays++;
        hvcc.push_back(numArrays);

        auto appendArray = [&](uint8_t nalType, const std::vector<uint8_t>& data)
        {
            hvcc.push_back(0x80 | nalType); // array_completeness=1
            hvcc.push_back(0); // numNalus (hi)
            hvcc.push_back(1); // numNalus (lo)
            hvcc.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
            hvcc.push_back(static_cast<uint8_t>(data.size() & 0xFF));
            hvcc.insert(hvcc.end(), data.begin(), data.end());
        };

        if (!vps.empty()) appendArray(32, vps);
        if (!sps.empty()) appendArray(33, sps);
        if (!pps.empty()) appendArray(34, pps);

        return hvcc;
    }

    struct Av1SequenceHeaderInfo
    {
        uint8_t seqProfile = 0;
        uint8_t seqLevelIdx0 = 0;
        uint8_t seqTier0 = 0;
        uint8_t highBitdepth = 0;
        uint8_t twelveBit = 0;
        uint8_t monoChrome = 0;
        uint8_t subsamplingX = 1;
        uint8_t subsamplingY = 1;
        uint8_t chromaSamplePosition = 0;
        uint8_t initialDelayPresent = 0;
        uint8_t initialDelayMinusOne = 0;
    };

    struct BitReader
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t bitPos = 0;

        bool ReadBits(uint32_t& value, uint32_t bits)
        {
            if (bits == 0 || bits > 32)
            {
                return false;
            }
            if (bitPos + bits > size * 8)
            {
                return false;
            }
            uint32_t v = 0;
            for (uint32_t i = 0; i < bits; ++i)
            {
                size_t byteIndex = (bitPos + i) / 8;
                size_t bitIndex = 7 - ((bitPos + i) % 8);
                uint8_t bit = (data[byteIndex] >> bitIndex) & 0x1;
                v = (v << 1) | bit;
            }
            bitPos += bits;
            value = v;
            return true;
        }

        bool ReadBit(uint8_t& value)
        {
            uint32_t v = 0;
            if (!ReadBits(v, 1))
            {
                return false;
            }
            value = static_cast<uint8_t>(v);
            return true;
        }

        bool ReadUvlc(uint32_t& value)
        {
            uint32_t leadingZero = 0;
            uint8_t bit = 0;
            while (true)
            {
                if (!ReadBit(bit))
                {
                    return false;
                }
                if (bit == 1)
                {
                    break;
                }
                leadingZero++;
                if (leadingZero > 31)
                {
                    return false;
                }
            }
            if (leadingZero == 0)
            {
                value = 0;
                return true;
            }
            uint32_t suffix = 0;
            if (!ReadBits(suffix, leadingZero))
            {
                return false;
            }
            value = ((1u << leadingZero) - 1u) + suffix;
            return true;
        }
    };

    bool ParseTimingInfo(BitReader& reader)
    {
        uint32_t ignored = 0;
        if (!reader.ReadBits(ignored, 32)) return false;
        if (!reader.ReadBits(ignored, 32)) return false;
        uint8_t equalPictureInterval = 0;
        if (!reader.ReadBit(equalPictureInterval)) return false;
        if (equalPictureInterval)
        {
            if (!reader.ReadUvlc(ignored)) return false;
        }
        return true;
    }

    bool ParseDecoderModelInfo(BitReader& reader, uint8_t& bufferDelayLengthMinus1)
    {
        uint32_t temp = 0;
        if (!reader.ReadBits(temp, 5)) return false;
        bufferDelayLengthMinus1 = static_cast<uint8_t>(temp);
        if (!reader.ReadBits(temp, 32)) return false;
        if (!reader.ReadBits(temp, 5)) return false;
        if (!reader.ReadBits(temp, 5)) return false;
        return true;
    }

    bool ParseOperatingParametersInfo(BitReader& reader, uint8_t bufferDelayLengthMinus1)
    {
        uint32_t temp = 0;
        uint32_t bits = static_cast<uint32_t>(bufferDelayLengthMinus1) + 1;
        if (!reader.ReadBits(temp, bits)) return false;
        if (!reader.ReadBits(temp, bits)) return false;
        uint8_t ignored = 0;
        if (!reader.ReadBit(ignored)) return false;
        return true;
    }

    bool ParseColorConfig(BitReader& reader, uint8_t seqProfile, Av1SequenceHeaderInfo& info)
    {
        uint8_t highBitdepth = 0;
        if (!reader.ReadBit(highBitdepth)) return false;
        uint8_t twelveBit = 0;
        if (seqProfile == 2 && highBitdepth)
        {
            if (!reader.ReadBit(twelveBit)) return false;
        }
        uint8_t monoChrome = 0;
        if (seqProfile == 1)
        {
            monoChrome = 0;
        }
        else
        {
            if (!reader.ReadBit(monoChrome)) return false;
        }

        uint8_t colorDescriptionPresent = 0;
        if (!reader.ReadBit(colorDescriptionPresent)) return false;
        if (colorDescriptionPresent)
        {
            uint32_t ignored = 0;
            if (!reader.ReadBits(ignored, 8)) return false;
            if (!reader.ReadBits(ignored, 8)) return false;
            if (!reader.ReadBits(ignored, 8)) return false;
        }

        uint8_t colorRange = 0;
        if (!reader.ReadBit(colorRange)) return false;

        uint8_t subsamplingX = 1;
        uint8_t subsamplingY = 1;
        uint8_t chromaSamplePosition = 0;
        if (monoChrome)
        {
            subsamplingX = 1;
            subsamplingY = 1;
            chromaSamplePosition = 0;
        }
        else if (seqProfile == 0)
        {
            subsamplingX = 1;
            subsamplingY = 1;
        }
        else if (seqProfile == 1)
        {
            subsamplingX = 0;
            subsamplingY = 0;
        }
        else
        {
            if (!reader.ReadBit(subsamplingX)) return false;
            if (subsamplingX)
            {
                if (!reader.ReadBit(subsamplingY)) return false;
            }
            else
            {
                subsamplingY = 0;
            }
        }

        if (subsamplingX && subsamplingY)
        {
            uint32_t tmp = 0;
            if (!reader.ReadBits(tmp, 2)) return false;
            chromaSamplePosition = static_cast<uint8_t>(tmp);
        }
        else
        {
            chromaSamplePosition = 0;
        }

        info.highBitdepth = highBitdepth;
        info.twelveBit = twelveBit;
        info.monoChrome = monoChrome;
        info.subsamplingX = subsamplingX;
        info.subsamplingY = subsamplingY;
        info.chromaSamplePosition = chromaSamplePosition;
        return true;
    }

    bool ParseAv1SequenceHeader(const uint8_t* data, size_t size, Av1SequenceHeaderInfo& info)
    {
        BitReader reader{ data, size, 0 };
        uint32_t seqProfile = 0;
        if (!reader.ReadBits(seqProfile, 3)) return false;
        info.seqProfile = static_cast<uint8_t>(seqProfile);

        uint8_t stillPicture = 0;
        if (!reader.ReadBit(stillPicture)) return false;
        uint8_t reducedStillPictureHeader = 0;
        if (!reader.ReadBit(reducedStillPictureHeader)) return false;

        uint8_t timingInfoPresent = 0;
        uint8_t decoderModelInfoPresent = 0;
        uint8_t initialDisplayDelayPresent = 0;
        uint8_t bufferDelayLengthMinus1 = 0;

        uint32_t operatingPointsCntMinus1 = 0;
        uint32_t seqLevelIdx0 = 0;
        uint8_t seqTier0 = 0;
        uint8_t initialDelayMinusOne0 = 0;

        if (reducedStillPictureHeader)
        {
            if (!reader.ReadBits(seqLevelIdx0, 5)) return false;
            seqTier0 = 0;
            initialDisplayDelayPresent = 0;
        }
        else
        {
            if (!reader.ReadBit(timingInfoPresent)) return false;
            if (timingInfoPresent)
            {
                if (!ParseTimingInfo(reader)) return false;
            }
            if (!reader.ReadBit(decoderModelInfoPresent)) return false;
            if (decoderModelInfoPresent)
            {
                if (!ParseDecoderModelInfo(reader, bufferDelayLengthMinus1)) return false;
            }
            if (!reader.ReadBit(initialDisplayDelayPresent)) return false;

            if (!reader.ReadBits(operatingPointsCntMinus1, 5)) return false;
            for (uint32_t i = 0; i <= operatingPointsCntMinus1; ++i)
            {
                uint32_t opIdc = 0;
                if (!reader.ReadBits(opIdc, 12)) return false;
                uint32_t levelIdx = 0;
                if (!reader.ReadBits(levelIdx, 5)) return false;
                uint8_t tier = 0;
                if (levelIdx > 7)
                {
                    if (!reader.ReadBit(tier)) return false;
                }
                if (decoderModelInfoPresent)
                {
                    uint8_t decoderModelPresent = 0;
                    if (!reader.ReadBit(decoderModelPresent)) return false;
                    if (decoderModelPresent)
                    {
                        if (!ParseOperatingParametersInfo(reader, bufferDelayLengthMinus1)) return false;
                    }
                }
                uint8_t initialDelayPresentForOp = 0;
                uint8_t initialDelayMinusOne = 0;
                if (initialDisplayDelayPresent)
                {
                    if (!reader.ReadBit(initialDelayPresentForOp)) return false;
                    if (initialDelayPresentForOp)
                    {
                        uint32_t tmp = 0;
                        if (!reader.ReadBits(tmp, 4)) return false;
                        initialDelayMinusOne = static_cast<uint8_t>(tmp);
                    }
                }
                if (i == 0)
                {
                    seqLevelIdx0 = levelIdx;
                    seqTier0 = tier;
                    info.initialDelayPresent = initialDelayPresentForOp;
                    info.initialDelayMinusOne = initialDelayMinusOne;
                }
            }
        }

        info.seqLevelIdx0 = static_cast<uint8_t>(seqLevelIdx0 & 0x1F);
        info.seqTier0 = seqTier0;

        uint32_t temp = 0;
        if (!reader.ReadBits(temp, 4)) return false;
        uint32_t frameWidthBitsMinus1 = temp;
        if (!reader.ReadBits(temp, 4)) return false;
        uint32_t frameHeightBitsMinus1 = temp;
        if (!reader.ReadBits(temp, frameWidthBitsMinus1 + 1)) return false;
        if (!reader.ReadBits(temp, frameHeightBitsMinus1 + 1)) return false;

        uint8_t frameIdNumbersPresent = 0;
        if (!reducedStillPictureHeader)
        {
            if (!reader.ReadBit(frameIdNumbersPresent)) return false;
            if (frameIdNumbersPresent)
            {
                if (!reader.ReadBits(temp, 4)) return false;
                if (!reader.ReadBits(temp, 3)) return false;
            }
        }

        uint8_t use128x128Superblock = 0;
        if (!reader.ReadBit(use128x128Superblock)) return false;
        uint8_t enableFilterIntra = 0;
        if (!reader.ReadBit(enableFilterIntra)) return false;
        uint8_t enableIntraEdgeFilter = 0;
        if (!reader.ReadBit(enableIntraEdgeFilter)) return false;

        if (reducedStillPictureHeader)
        {
            // skip defaults for reduced header
        }
        else
        {
            uint8_t enableInterintraCompound = 0;
            if (!reader.ReadBit(enableInterintraCompound)) return false;
            uint8_t enableMaskedCompound = 0;
            if (!reader.ReadBit(enableMaskedCompound)) return false;
            uint8_t enableWarpedMotion = 0;
            if (!reader.ReadBit(enableWarpedMotion)) return false;
            uint8_t enableDualFilter = 0;
            if (!reader.ReadBit(enableDualFilter)) return false;
            uint8_t enableOrderHint = 0;
            if (!reader.ReadBit(enableOrderHint)) return false;
            if (enableOrderHint)
            {
                uint8_t enableJntComp = 0;
                if (!reader.ReadBit(enableJntComp)) return false;
                uint8_t enableRefFrameMvs = 0;
                if (!reader.ReadBit(enableRefFrameMvs)) return false;
            }
            uint8_t seqChooseScreenContentTools = 0;
            if (!reader.ReadBit(seqChooseScreenContentTools)) return false;
            uint8_t seqForceScreenContentTools = 0;
            if (!seqChooseScreenContentTools)
            {
                if (!reader.ReadBit(seqForceScreenContentTools)) return false;
            }
            if (seqForceScreenContentTools)
            {
                uint8_t seqChooseIntegerMv = 0;
                if (!reader.ReadBit(seqChooseIntegerMv)) return false;
                if (!seqChooseIntegerMv)
                {
                    uint8_t seqForceIntegerMv = 0;
                    if (!reader.ReadBit(seqForceIntegerMv)) return false;
                }
            }
            if (enableOrderHint)
            {
                if (!reader.ReadBits(temp, 3)) return false;
            }
        }

        uint8_t enableSuperres = 0;
        if (!reader.ReadBit(enableSuperres)) return false;
        uint8_t enableCdef = 0;
        if (!reader.ReadBit(enableCdef)) return false;
        uint8_t enableRestoration = 0;
        if (!reader.ReadBit(enableRestoration)) return false;

        if (!ParseColorConfig(reader, info.seqProfile, info)) return false;

        uint8_t filmGrainParamsPresent = 0;
        if (!reader.ReadBit(filmGrainParamsPresent)) return false;

        return true;
    }

    struct Av1ObuHeader
    {
        uint8_t type = 0;
        bool hasSizeField = false;
        bool extensionFlag = false;
        uint8_t headerByte = 0;
        uint8_t extensionByte = 0;
        size_t headerSize = 0;
        size_t sizeFieldBytes = 0;
        size_t payloadSize = 0;
        size_t totalSize = 0;
        const uint8_t* payload = nullptr;
    };

    bool ReadLeb128(const uint8_t* data, size_t size, size_t& offset, uint64_t& value)
    {
        value = 0;
        uint32_t shift = 0;
        while (offset < size && shift < 56)
        {
            uint8_t byte = data[offset++];
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
            {
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool WriteLeb128(std::vector<uint8_t>& out, uint64_t value)
    {
        do
        {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0)
            {
                byte |= 0x80;
            }
            out.push_back(byte);
        } while (value != 0);
        return true;
    }

    bool ParseAv1Obu(const uint8_t* data, size_t size, size_t offset, Av1ObuHeader& obu)
    {
        if (offset >= size)
        {
            return false;
        }
        uint8_t header = data[offset];
        if (header & 0x80)
        {
            return false;
        }
        obu.headerByte = header;
        obu.type = (header >> 3) & 0x0F;
        obu.extensionFlag = ((header >> 2) & 0x01) != 0;
        obu.hasSizeField = ((header >> 1) & 0x01) != 0;
        obu.headerSize = 1 + (obu.extensionFlag ? 1 : 0);
        size_t cursor = offset + 1;
        if (obu.extensionFlag)
        {
            if (cursor >= size)
            {
                return false;
            }
            obu.extensionByte = data[cursor++];
        }
        if (!obu.hasSizeField)
        {
            return false;
        }
        uint64_t payloadSize = 0;
        size_t sizeFieldStart = cursor;
        if (!ReadLeb128(data, size, cursor, payloadSize))
        {
            return false;
        }
        obu.sizeFieldBytes = cursor - sizeFieldStart;
        obu.payloadSize = static_cast<size_t>(payloadSize);
        obu.totalSize = obu.headerSize + obu.sizeFieldBytes + obu.payloadSize;
        if (offset + obu.totalSize > size)
        {
            return false;
        }
        obu.payload = data + offset + obu.headerSize + obu.sizeFieldBytes;
        return true;
    }

    bool ExtractAv1SequenceHeaderObu(const uint8_t* data, size_t size, std::vector<uint8_t>& outObu, Av1SequenceHeaderInfo& info)
    {
        if (!data || size == 0)
        {
            return false;
        }

        uint8_t firstHeader = data[0];
        bool firstHasSize = ((firstHeader >> 1) & 0x01) != 0;
        bool firstExt = ((firstHeader >> 2) & 0x01) != 0;
        if (!firstHasSize)
        {
            size_t headerSize = 1 + (firstExt ? 1 : 0);
            if (size <= headerSize)
            {
                return false;
            }
            uint8_t type = (firstHeader >> 3) & 0x0F;
            const uint8_t* payload = data + headerSize;
            size_t payloadSize = size - headerSize;
            if (type != 1)
            {
                return false;
            }
            if (!ParseAv1SequenceHeader(payload, payloadSize, info))
            {
                return false;
            }

            outObu.clear();
            uint8_t header = static_cast<uint8_t>((firstHeader | 0x02) & 0xFE);
            outObu.push_back(header);
            if (firstExt)
            {
                outObu.push_back(data[1]);
            }
            WriteLeb128(outObu, payloadSize);
            outObu.insert(outObu.end(), payload, payload + payloadSize);
            return true;
        }

        size_t offset = 0;
        while (offset < size)
        {
            Av1ObuHeader obu{};
            if (!ParseAv1Obu(data, size, offset, obu))
            {
                return false;
            }
            if (obu.type == 1)
            {
                if (!ParseAv1SequenceHeader(obu.payload, obu.payloadSize, info))
                {
                    return false;
                }
                outObu.assign(data + offset, data + offset + obu.totalSize);
                return true;
            }
            offset += obu.totalSize;
        }
        return false;
    }

    std::vector<uint8_t> BuildAv1CFromSequenceObu(const std::vector<uint8_t>& seqObu, const Av1SequenceHeaderInfo& info)
    {
        std::vector<uint8_t> av1c;
        av1c.reserve(4 + seqObu.size());
        av1c.push_back(0x80 | 0x01); // marker=1, version=1
        av1c.push_back(static_cast<uint8_t>(((info.seqProfile & 0x07) << 5) | (info.seqLevelIdx0 & 0x1F)));
        av1c.push_back(static_cast<uint8_t>(((info.seqTier0 & 0x01) << 7)
            | ((info.highBitdepth & 0x01) << 6)
            | ((info.twelveBit & 0x01) << 5)
            | ((info.monoChrome & 0x01) << 4)
            | ((info.subsamplingX & 0x01) << 3)
            | ((info.subsamplingY & 0x01) << 2)
            | (info.chromaSamplePosition & 0x03)));
        av1c.push_back(static_cast<uint8_t>(((info.initialDelayPresent & 0x01) << 4)
            | (info.initialDelayMinusOne & 0x0F)));
        av1c.insert(av1c.end(), seqObu.begin(), seqObu.end());
        return av1c;
    }

    bool BuildAv1CFromBitstream(const std::vector<uint8_t>& bitstream, std::vector<uint8_t>& out)
    {
        if (bitstream.empty())
        {
            return false;
        }
        std::vector<uint8_t> seqObu;
        Av1SequenceHeaderInfo info{};
        if (!ExtractAv1SequenceHeaderObu(bitstream.data(), bitstream.size(), seqObu, info))
        {
            return false;
        }
        out = BuildAv1CFromSequenceObu(seqObu, info);
        return !out.empty();
    }

    std::vector<uint8_t> ConvertToLengthPrefixed(const std::vector<NalUnit>& units, bool keepParameterSets)
    {
        std::vector<uint8_t> output;
        for (const auto& unit : units)
        {
            if (!keepParameterSets)
            {
                if (unit.type == 7 || unit.type == 8 || unit.type == 32 || unit.type == 33 || unit.type == 34)
                {
                    continue;
                }
            }
            uint32_t len = static_cast<uint32_t>(unit.size);
            output.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
            output.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
            output.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            output.push_back(static_cast<uint8_t>(len & 0xFF));
            output.insert(output.end(), unit.data, unit.data + unit.size);
        }
        return output;
    }

    bool ProcessEncodedBitstream(EncoderState* state, const uint8_t* data, size_t size, bool keyframeHint)
    {
        if (!state || !data || size == 0)
        {
            return true;
        }

        std::vector<uint8_t> buffer(data, data + size);
        const bool hevc = IsHevc(state);
        const bool av1 = IsAv1(state);
        if (av1)
        {
            // AV1 uses OBU bitstream; do not parse as Annex B.
            bool isKeyframe = keyframeHint;

            if (state->codecPrivate.empty())
            {
                std::vector<uint8_t> codecPrivate;
                if (BuildAv1CFromBitstream(buffer, codecPrivate))
                {
                    state->codecPrivate = codecPrivate;
                }
            }

            if (!state->writerInitialized)
            {
                if (!InitializeMp4Writer(state, state->codec, state->codecPrivate))
                {
                    return false;
                }
            }

            if (!state->writerStarted)
            {
                StartWriterThread(state);
            }
            if (state->writerError)
            {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(state->writerMutex);
                state->sampleQueue.push_back({ std::move(buffer), isKeyframe, false, 0 });
            }
            state->writerCv.notify_one();
            return true;
        }

        auto units = ParseAnnexB(buffer.data(), buffer.size(), hevc);

        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        std::vector<uint8_t> vps;
        bool isKeyframe = false;
        for (const auto& unit : units)
        {
            if (!hevc)
            {
                if (unit.type == 7 && sps.empty())
                {
                    sps.assign(unit.data, unit.data + unit.size);
                }
                else if (unit.type == 8 && pps.empty())
                {
                    pps.assign(unit.data, unit.data + unit.size);
                }
                else if (unit.type == 5)
                {
                    isKeyframe = true;
                }
            }
            else
            {
                if (unit.type == 32 && vps.empty())
                {
                    vps.assign(unit.data, unit.data + unit.size);
                }
                else if (unit.type == 33 && sps.empty())
                {
                    sps.assign(unit.data, unit.data + unit.size);
                }
                else if (unit.type == 34 && pps.empty())
                {
                    pps.assign(unit.data, unit.data + unit.size);
                }
                else if (unit.type == 19 || unit.type == 20)
                {
                    isKeyframe = true;
                }
            }
        }

        if (!state->writerInitialized)
        {
            std::vector<uint8_t> codecPrivate = hevc ? BuildHvcC(vps, sps, pps) : BuildAvcC(sps, pps);
            if (codecPrivate.empty())
            {
                return true;
            }
            if (!InitializeMp4Writer(state, state->codec, codecPrivate))
            {
                return false;
            }
        }
        else if (state->codecPrivate.empty())
        {
            std::vector<uint8_t> codecPrivate = hevc ? BuildHvcC(vps, sps, pps) : BuildAvcC(sps, pps);
            if (!codecPrivate.empty())
            {
                state->codecPrivate = codecPrivate;
            }
        }

        auto sampleData = ConvertToLengthPrefixed(units, false);
        if (sampleData.empty())
        {
            return true;
        }

        if (!state->writerStarted)
        {
            StartWriterThread(state);
        }
        if (state->writerError)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(state->writerMutex);
            state->sampleQueue.push_back({ std::move(sampleData), isKeyframe, false, 0 });
        }
        state->writerCv.notify_one();
        return true;
    }

#if 0 // Original NVENC backend; retained only as upstream-reference text in this derived file.
    bool ConsumeAsyncBitstream(EncoderState* state, size_t index)
    {
        if (!state || !state->asyncEnabled || index >= state->asyncBitstreams.size())
        {
            return false;
        }

        HANDLE eventHandle = state->asyncEvents[index];
        if (eventHandle)
        {
            DWORD result = WaitForSingleObject(eventHandle, 5000);
            if (result == WAIT_OBJECT_0)
            {
                LogLine(state, L"async event signaled slot=" + std::to_wstring(index));
            }
            else if (result != WAIT_TIMEOUT)
            {
                SetError(state, L"nvEnc async wait failed.");
                return false;
            }
            else
            {
                LogLine(state, L"async wait timeout slot=" + std::to_wstring(index));
            }
        }

        const DWORD maxWaitMs = 5000;
        DWORD waited = 0;
        LogLine(state, L"async lock start slot=" + std::to_wstring(index));
        while (waited < maxWaitMs)
        {
            NV_ENC_LOCK_BITSTREAM lockBitstream{};
            lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
            lockBitstream.outputBitstream = state->asyncBitstreams[index];
            lockBitstream.doNotWait = 1;
            auto status = state->funcs.nvEncLockBitstream(state->session, &lockBitstream);
            if (status == NV_ENC_SUCCESS)
            {
                LogLine(state, L"async bitstream lock ok slot=" + std::to_wstring(index));
                bool ok = true;
                if (lockBitstream.bitstreamSizeInBytes > 0)
                {
                    ok = ProcessEncodedBitstream(state,
                        static_cast<uint8_t*>(lockBitstream.bitstreamBufferPtr),
                        lockBitstream.bitstreamSizeInBytes,
                        lockBitstream.pictureType);
                }

                auto unlockStatus = state->funcs.nvEncUnlockBitstream(state->session, state->asyncBitstreams[index]);
                if (!CheckStatus(state, unlockStatus, L"nvEncUnlockBitstream failed"))
                {
                    return false;
                }

                state->asyncPending[index] = false;
                return ok;
            }
            if (status != NV_ENC_ERR_LOCK_BUSY)
            {
                CheckStatus(state, status, L"nvEncLockBitstream failed");
                return false;
            }

            Sleep(2);
            waited += 2;
        }

        LogLine(state, L"async lock timeout slot=" + std::to_wstring(index));
        SetError(state, L"nvEnc async timeout.");
        return false;
    }

    bool InitializeAsyncResources(EncoderState* state, uint32_t depth)
    {
        if (!state || !state->session || depth < 2)
        {
            return false;
        }

        state->asyncBitstreams.clear();
        state->asyncEvents.clear();
        state->asyncPending.clear();
        state->asyncBitstreams.resize(depth, nullptr);
        state->asyncEvents.resize(depth, nullptr);
        state->asyncPending.resize(depth, false);

        for (uint32_t i = 0; i < depth; ++i)
        {
            NV_ENC_CREATE_BITSTREAM_BUFFER createBitstream{};
            createBitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            auto status = state->funcs.nvEncCreateBitstreamBuffer(state->session, &createBitstream);
            if (!CheckStatus(state, status, L"nvEncCreateBitstreamBuffer failed"))
            {
                ReleaseAsyncResources(state);
                return false;
            }
            state->asyncBitstreams[i] = createBitstream.bitstreamBuffer;

            HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!evt)
            {
                SetError(state, L"Failed to create async event.");
                ReleaseAsyncResources(state);
                return false;
            }

            NV_ENC_EVENT_PARAMS eventParams{};
            eventParams.version = NV_ENC_EVENT_PARAMS_VER;
            eventParams.completionEvent = evt;
            status = state->funcs.nvEncRegisterAsyncEvent(state->session, &eventParams);
            if (!CheckStatus(state, status, L"nvEncRegisterAsyncEvent failed"))
            {
                CloseHandle(evt);
                ReleaseAsyncResources(state);
                return false;
            }

            state->asyncEvents[i] = evt;
        }

        state->asyncDepth = depth;
        state->asyncIndex = 0;
        state->asyncEnabled = true;
        LogLine(state, L"async initialized");
        return true;
    }

    void ReleaseAsyncResources(EncoderState* state)
    {
        if (!state)
        {
            return;
        }

        for (size_t i = 0; i < state->asyncBitstreams.size(); ++i)
        {
            if (state->asyncBitstreams[i])
            {
                state->funcs.nvEncDestroyBitstreamBuffer(state->session, state->asyncBitstreams[i]);
                state->asyncBitstreams[i] = nullptr;
            }
            if (state->asyncEvents[i])
            {
                NV_ENC_EVENT_PARAMS eventParams{};
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = state->asyncEvents[i];
                state->funcs.nvEncUnregisterAsyncEvent(state->session, &eventParams);
                CloseHandle(state->asyncEvents[i]);
                state->asyncEvents[i] = nullptr;
            }
        }

        state->asyncBitstreams.clear();
        state->asyncEvents.clear();
        state->asyncPending.clear();
        state->asyncDepth = 0;
        state->asyncIndex = 0;
        state->asyncEnabled = false;
    }

    bool DrainAsyncBitstreams(EncoderState* state)
    {
        if (!state || !state->asyncEnabled)
        {
            return true;
        }

        LogLine(state, L"drain async bitstreams");
        const size_t depth = state->asyncPending.size();
        const size_t start = depth > 0 ? state->asyncIndex % depth : 0;
        for (size_t offset = 0; offset < depth; ++offset)
        {
            const size_t i = (start + offset) % depth;
            if (state->asyncPending[i])
            {
                LogLine(state, L"drain slot start=" + std::to_wstring(i));
                if (!ConsumeAsyncBitstream(state, i))
                {
                    return false;
                }
                LogLine(state, L"drain slot done=" + std::to_wstring(i));
            }
        }
        state->asyncIndex = start;
        return true;
    }

    bool WaitForAsyncEvent(EncoderState* state, HANDLE eventHandle, const wchar_t* context)
    {
        if (!eventHandle)
        {
            return true;
        }

        LogLine(state, std::wstring(context) + L" start");
        DWORD result = WaitForSingleObject(eventHandle, 5000);
        if (result == WAIT_OBJECT_0)
        {
            LogLine(state, std::wstring(context) + L" done");
            return true;
        }
        if (result == WAIT_TIMEOUT)
        {
            SetError(state, std::wstring(context) + L" timed out.");
            return false;
        }

        SetError(state, std::wstring(context) + L" failed.");
        return false;
    }

    bool TryInitAv1CodecPrivate(EncoderState* state)
    {
        if (!state || !IsAv1(state) || !state->codecPrivate.empty() || !state->funcs.nvEncGetSequenceParams)
        {
            return true;
        }

        std::vector<uint8_t> buffer(NV_MAX_SEQ_HDR_LEN);
        uint32_t outSize = 0;
        NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
        payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
        payload.inBufferSize = static_cast<uint32_t>(buffer.size());
        payload.spsId = 0;
        payload.ppsId = 0;
        payload.spsppsBuffer = buffer.data();
        payload.outSPSPPSPayloadSize = &outSize;

        auto status = state->funcs.nvEncGetSequenceParams(state->session, &payload);
        if (status != NV_ENC_SUCCESS)
        {
            LogLine(state, L"nvEncGetSequenceParams failed for AV1");
            return true;
        }
        if (outSize == 0 || outSize > buffer.size())
        {
            return true;
        }

        std::vector<uint8_t> seqObu;
        Av1SequenceHeaderInfo info{};
        if (!ExtractAv1SequenceHeaderObu(buffer.data(), outSize, seqObu, info))
        {
            return true;
        }

        auto av1c = BuildAv1CFromSequenceObu(seqObu, info);
        if (!av1c.empty())
        {
            state->codecPrivate = std::move(av1c);
        }
        return true;
    }


    bool InitializeEncoder(EncoderState* state, ID3D11Device* device, int width, int height, int fps, int bitrateKbps, int codec, int quality, int fastPreset, int rateControlMode, int maxBitrateKbps, NV_ENC_BUFFER_FORMAT bufferFormat, int hevcAsync)
    {
        state->width = width;
        state->height = height;
        state->fps = fps;
        state->fastPreset = fastPreset;
        state->originalBufferFormat = bufferFormat;
        state->bufferFormat = bufferFormat;
        state->codec = codec;
        state->device = device;
        if (state->device)
        {
            state->device->AddRef();
        }

        const bool hevcAsyncOptIn = (codec == kCodecHevc && hevcAsync != 0);

        if (state->fastPreset != 0)
        {
            if (EnsureVideoProcessor(state))
            {
                state->bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
            }
            else
            {
                state->fastPreset = 0;
                state->bufferFormat = state->originalBufferFormat;
            }
        }

        // Load only from System32 to avoid DLL hijacking via current/plugin directories.
        state->nvencModule = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!state->nvencModule)
        {
            SetError(state, L"nvEncodeAPI64.dll not found. Check NVIDIA driver.");
            return false;
        }

        state->createInstance = reinterpret_cast<decltype(state->createInstance)>(
            GetProcAddress(state->nvencModule, "NvEncodeAPICreateInstance"));
        if (!state->createInstance)
        {
            SetError(state, L"Failed to get NvEncodeAPICreateInstance.");
            return false;
        }

        state->funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        auto status = state->createInstance(&state->funcs);
        if (!CheckStatus(state, status, L"NvEncodeAPICreateInstance failed"))
        {
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        openParams.device = device;
        openParams.apiVersion = NVENCAPI_VERSION;

        status = state->funcs.nvEncOpenEncodeSessionEx(&openParams, &state->session);
        if (!CheckStatus(state, status, L"nvEncOpenEncodeSessionEx failed"))
        {
            return false;
        }

        state->initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        state->config.version = NV_ENC_CONFIG_VER;

        GUID encodeGuid = NV_ENC_CODEC_H264_GUID;
        if (codec == kCodecHevc)
        {
            encodeGuid = NV_ENC_CODEC_HEVC_GUID;
        }
        else if (codec == kCodecAv1)
        {
            encodeGuid = NV_ENC_CODEC_AV1_GUID;
        }
        const GUID presetGuid = (state->fastPreset != 0)
            ? NV_ENC_PRESET_P1_GUID
            : (quality <= 0 ? NV_ENC_PRESET_P1_GUID : (quality == 2 ? NV_ENC_PRESET_P7_GUID : NV_ENC_PRESET_P3_GUID));
        const NV_ENC_TUNING_INFO tuningInfo = (state->fastPreset != 0)
            ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
            : NV_ENC_TUNING_INFO_HIGH_QUALITY;

        NV_ENC_PRESET_CONFIG presetConfig{};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
        status = state->funcs.nvEncGetEncodePresetConfigEx(state->session, encodeGuid, presetGuid, tuningInfo, &presetConfig);
        if (!CheckStatus(state, status, L"nvEncGetEncodePresetConfigEx failed"))
        {
            return false;
        }

        state->config = presetConfig.presetCfg;

        state->initParams.encodeGUID = encodeGuid;
        state->initParams.presetGUID = presetGuid;
        state->initParams.tuningInfo = tuningInfo;
        state->initParams.encodeWidth = width;
        state->initParams.encodeHeight = height;
        state->initParams.maxEncodeWidth = width;
        state->initParams.maxEncodeHeight = height;
        state->initParams.darWidth = width;
        state->initParams.darHeight = height;
        state->initParams.frameRateNum = fps;
        state->initParams.frameRateDen = 1;
        state->initParams.enablePTD = 1;
        state->initParams.reportSliceOffsets = 0;
        state->initParams.enableSubFrameWrite = 0;
        const bool allowAsync = (codec == kCodecH264) || (codec == kCodecAv1) || (codec == kCodecHevc && hevcAsyncOptIn);
        state->initParams.enableEncodeAsync = allowAsync ? 1 : 0;
        state->initParams.encodeConfig = &state->config;

        state->config.rcParams.rateControlMode = (rateControlMode == 1) ? NV_ENC_PARAMS_RC_VBR : NV_ENC_PARAMS_RC_CBR;
        state->config.rcParams.averageBitRate = static_cast<uint32_t>(bitrateKbps) * 1000;
        state->config.rcParams.maxBitRate = (rateControlMode == 1 && maxBitrateKbps > 0)
            ? static_cast<uint32_t>(maxBitrateKbps) * 1000
            : state->config.rcParams.averageBitRate;
        state->config.gopLength = state->fps * 2;
        state->config.frameIntervalP = 1;
        state->config.rcParams.enableLookahead = 0;
        state->config.rcParams.lookaheadDepth = 0;
        if (state->fastPreset != 0)
        {
            state->initParams.enableSubFrameWrite = 1;
        }
        if (state->fastPreset != 0)
        {
            state->config.gopLength = state->fps * 4;
            state->config.rcParams.enableAQ = 0;
            state->config.rcParams.enableTemporalAQ = 0;
            state->config.rcParams.enableLookahead = 0;
            state->config.rcParams.lookaheadDepth = 0;
        }

        if (codec == kCodecHevc)
        {
            state->config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
            state->config.encodeCodecConfig.hevcConfig.idrPeriod = state->config.gopLength;
        }
        else if (codec == kCodecAv1)
        {
            state->config.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
            state->config.encodeCodecConfig.av1Config.outputAnnexBFormat = 0;
            state->config.encodeCodecConfig.av1Config.idrPeriod = state->config.gopLength;
            if (state->config.encodeCodecConfig.av1Config.chromaFormatIDC == 0)
            {
                state->config.encodeCodecConfig.av1Config.chromaFormatIDC = 1;
            }
            if (state->config.encodeCodecConfig.av1Config.outputBitDepth == 0)
            {
                state->config.encodeCodecConfig.av1Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
            }
            if (state->config.encodeCodecConfig.av1Config.inputBitDepth == 0)
            {
                state->config.encodeCodecConfig.av1Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
            }
        }
        else
        {
            state->config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            state->config.encodeCodecConfig.h264Config.idrPeriod = state->config.gopLength;
        }

        status = state->funcs.nvEncInitializeEncoder(state->session, &state->initParams);
        if (!CheckStatus(state, status, L"nvEncInitializeEncoder failed"))
        {
            return false;
        }

        if (!TryInitAv1CodecPrivate(state))
        {
            return false;
        }

        if (!allowAsync)
        {
            state->initParams.enableEncodeAsync = 0;
            state->asyncEnabled = false;
            if (codec == kCodecHevc)
            {
                LogLine(state, L"HEVC async disabled (sync mode)");
            }
            NV_ENC_CREATE_BITSTREAM_BUFFER createBitstream{};
            createBitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            status = state->funcs.nvEncCreateBitstreamBuffer(state->session, &createBitstream);
            if (!CheckStatus(state, status, L"nvEncCreateBitstreamBuffer failed"))
            {
                return false;
            }
            state->bitstream = createBitstream.bitstreamBuffer;
        }
        else
        {
            uint32_t asyncDepth = 4;
            if (state->config.rcParams.enableLookahead && state->config.rcParams.lookaheadDepth > 0)
            {
                asyncDepth = std::max<uint32_t>(asyncDepth, state->config.rcParams.lookaheadDepth + 2);
            }
            asyncDepth = std::min<uint32_t>(asyncDepth, 32);
            LogLine(state, L"async depth=" + std::to_wstring(asyncDepth)
                + L" lookahead=" + std::to_wstring(state->config.rcParams.enableLookahead)
                + L" depth=" + std::to_wstring(state->config.rcParams.lookaheadDepth));

            if (!InitializeAsyncResources(state, asyncDepth))
            {
                state->initParams.enableEncodeAsync = 0;
                state->asyncEnabled = false;
            if (codec == kCodecHevc)
            {
                LogLine(state, L"HEVC async failed, fallback to sync");
            }
                NV_ENC_CREATE_BITSTREAM_BUFFER createBitstream{};
                createBitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                status = state->funcs.nvEncCreateBitstreamBuffer(state->session, &createBitstream);
                if (!CheckStatus(state, status, L"nvEncCreateBitstreamBuffer failed"))
                {
                    return false;
                }
                state->bitstream = createBitstream.bitstreamBuffer;
            }
        }
        return true;
    }

    bool EncodeTexture(EncoderState* state, ID3D11Texture2D* texture)
    {
        if (!state || !texture)
        {
            return false;
        }

        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_BUFFER_FORMAT usedBufferFormat = state->bufferFormat;
        if (state->fastPreset != 0)
        {
            auto* converted = ConvertToNv12(state, texture);
            if (!converted)
            {
                // Fall back to RGB path when NV12 conversion is unavailable.
                state->fastPreset = 0;
                state->bufferFormat = state->originalBufferFormat;
            }
            else
            {
                texture = converted;

                if (!state->registeredNv12)
                {
                    NV_ENC_REGISTER_RESOURCE registerRes{};
                    registerRes.version = NV_ENC_REGISTER_RESOURCE_VER;
                    registerRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                    registerRes.resourceToRegister = texture;
                    registerRes.width = state->width;
                    registerRes.height = state->height;
                    registerRes.bufferFormat = state->bufferFormat;
                    registerRes.bufferUsage = NV_ENC_INPUT_IMAGE;
                    auto status = state->funcs.nvEncRegisterResource(state->session, &registerRes);
                    if (!CheckStatus(state, status, L"nvEncRegisterResource failed"))
                    {
                        return false;
                    }
                    state->registeredNv12 = registerRes.registeredResource;
                }
                registered = state->registeredNv12;
                usedBufferFormat = state->bufferFormat;
            }
        }

        if (!registered)
        {
            if (!EnsureRgbResource(state, texture))
            {
                SetError(state, L"Failed to prepare RGB input resource.");
                return false;
            }

            state->deviceContext->CopyResource(state->rgbTexture, texture);
            registered = state->registeredRgb;
            usedBufferFormat = state->bufferFormat;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        auto status = state->funcs.nvEncMapInputResource(state->session, &map);
        if (!CheckStatus(state, status, L"nvEncMapInputResource failed"))
        {
            return false;
        }

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = usedBufferFormat;
        pic.inputWidth = state->width;
        pic.inputHeight = state->height;
        size_t asyncSlot = 0;
        if (state->asyncEnabled)
        {
            asyncSlot = state->asyncIndex % state->asyncBitstreams.size();
            if (state->asyncPending[asyncSlot])
            {
                if (!ConsumeAsyncBitstream(state, asyncSlot))
                {
                    state->funcs.nvEncUnmapInputResource(state->session, map.mappedResource);
                    return false;
                }
            }
            pic.outputBitstream = state->asyncBitstreams[asyncSlot];
            pic.completionEvent = state->asyncEvents[asyncSlot];
        }
        else
        {
            pic.outputBitstream = state->bitstream;
        }
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = state->frameIndex++;
        pic.inputDuration = 1;

        status = state->funcs.nvEncEncodePicture(state->session, &pic);
        state->funcs.nvEncUnmapInputResource(state->session, map.mappedResource);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT)
        {
            LogLine(state, L"encode needs more input");
            return true;
        }
        if (!CheckStatus(state, status, L"nvEncEncodePicture failed"))
        {
            return false;
        }

        if (state->asyncEnabled)
        {
            state->asyncPending[asyncSlot] = true;
            state->asyncIndex = (asyncSlot + 1) % state->asyncBitstreams.size();
            return true;
        }

        NV_ENC_LOCK_BITSTREAM lockBitstream{};
        lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBitstream.outputBitstream = state->bitstream;
        status = state->funcs.nvEncLockBitstream(state->session, &lockBitstream);
        if (!CheckStatus(state, status, L"nvEncLockBitstream failed"))
        {
            return false;
        }

        bool ok = ProcessEncodedBitstream(state,
            static_cast<uint8_t*>(lockBitstream.bitstreamBufferPtr),
            lockBitstream.bitstreamSizeInBytes,
            lockBitstream.pictureType);

        status = state->funcs.nvEncUnlockBitstream(state->session, state->bitstream);
        if (!CheckStatus(state, status, L"nvEncUnlockBitstream failed"))
        {
            return false;
        }

        return ok;
    }

    bool EnsureRgbResource(EncoderState* state, ID3D11Texture2D* texture)
    {
        if (!state || !state->device || !texture)
        {
            return false;
        }

        if (!state->deviceContext)
        {
            state->device->GetImmediateContext(&state->deviceContext);
            if (!state->deviceContext)
            {
                return false;
            }
        }

        D3D11_TEXTURE2D_DESC srcDesc{};
        texture->GetDesc(&srcDesc);

        bool recreate = false;
        if (!state->rgbTexture)
        {
            recreate = true;
        }
        else
        {
            D3D11_TEXTURE2D_DESC dstDesc{};
            state->rgbTexture->GetDesc(&dstDesc);
            if (dstDesc.Width != srcDesc.Width || dstDesc.Height != srcDesc.Height || dstDesc.Format != srcDesc.Format)
            {
                recreate = true;
            }
        }

        if (recreate)
        {
            if (state->registeredRgb)
            {
                state->funcs.nvEncUnregisterResource(state->session, state->registeredRgb);
                state->registeredRgb = nullptr;
            }
            if (state->rgbTexture)
            {
                state->rgbTexture->Release();
                state->rgbTexture = nullptr;
            }

            D3D11_TEXTURE2D_DESC desc = srcDesc;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;

            if (FAILED(state->device->CreateTexture2D(&desc, nullptr, &state->rgbTexture)) || !state->rgbTexture)
            {
                return false;
            }
        }

        if (!state->registeredRgb)
        {
            NV_ENC_REGISTER_RESOURCE registerRes{};
            registerRes.version = NV_ENC_REGISTER_RESOURCE_VER;
            registerRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            registerRes.resourceToRegister = state->rgbTexture;
            registerRes.width = state->width;
            registerRes.height = state->height;
            registerRes.bufferFormat = state->bufferFormat;
            registerRes.bufferUsage = NV_ENC_INPUT_IMAGE;

            auto status = state->funcs.nvEncRegisterResource(state->session, &registerRes);
            if (!CheckStatus(state, status, L"nvEncRegisterResource failed"))
            {
                return false;
            }
            state->registeredRgb = registerRes.registeredResource;
        }

        return true;
    }

    bool EnsureVideoProcessor(EncoderState* state)
    {
        if (!state || !state->device)
        {
            return false;
        }
        if (state->videoProcessor && state->videoDevice && state->videoContext && state->videoEnumerator && state->nv12Texture && state->vpOutputView)
        {
            return true;
        }

        state->device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&state->videoDevice));
        if (!state->videoDevice)
        {
            return false;
        }

        ID3D11DeviceContext* context = nullptr;
        state->device->GetImmediateContext(&context);
        if (context)
        {
            context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&state->videoContext));
            context->Release();
        }
        if (!state->videoContext)
        {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = static_cast<UINT>(state->width);
        desc.InputHeight = static_cast<UINT>(state->height);
        desc.OutputWidth = static_cast<UINT>(state->width);
        desc.OutputHeight = static_cast<UINT>(state->height);
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(state->videoDevice->CreateVideoProcessorEnumerator(&desc, &state->videoEnumerator)) || !state->videoEnumerator)
        {
            return false;
        }

        if (FAILED(state->videoDevice->CreateVideoProcessor(state->videoEnumerator, 0, &state->videoProcessor)) || !state->videoProcessor)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = static_cast<UINT>(state->width);
        texDesc.Height = static_cast<UINT>(state->height);
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_NV12;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(state->device->CreateTexture2D(&texDesc, nullptr, &state->nv12Texture)) || !state->nv12Texture)
        {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outDesc.Texture2D.MipSlice = 0;
        if (FAILED(state->videoDevice->CreateVideoProcessorOutputView(state->nv12Texture, state->videoEnumerator, &outDesc, &state->vpOutputView)) || !state->vpOutputView)
        {
            return false;
        }

        return true;
    }

    ID3D11Texture2D* ConvertToNv12(EncoderState* state, ID3D11Texture2D* texture)
    {
        if (!state || !texture)
        {
            return nullptr;
        }
        if (!EnsureVideoProcessor(state))
        {
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;
        inDesc.Texture2D.ArraySlice = 0;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        if (FAILED(state->videoDevice->CreateVideoProcessorInputView(texture, state->videoEnumerator, &inDesc, &inputView)) || !inputView)
        {
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView;
        state->videoContext->VideoProcessorBlt(state->videoProcessor, state->vpOutputView, 0, 1, &stream);
        inputView->Release();

        return state->nv12Texture;
    }

#endif

    constexpr const wchar_t* kSlotIndexProperty = L"YMM4.AMF.SlotIndex";

    bool SetEncoderProperties(EncoderState* state, int bitrateKbps, int quality, int rateControlMode, int maxBitrateKbps)
    {
        const amf_int64 bitrate = static_cast<amf_int64>(bitrateKbps) * 1000;
        const amf_int64 maxBitrate = static_cast<amf_int64>(maxBitrateKbps) * 1000;
        AMF_RESULT result = AMF_OK;

        if (state->codec == kCodecHevc)
        {
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING);
            if (!CheckStatus(state, result, L"AMF HEVC usage configuration failed")) return false;

            const auto preset = quality <= 0 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
                : quality >= 2 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
                : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED;
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, preset);
            if (!CheckStatus(state, result, L"AMF HEVC quality configuration failed")) return false;

            const auto rateControl = rateControlMode == 0
                ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR
                : AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, rateControl);
            if (!CheckStatus(state, result, L"AMF HEVC rate-control configuration failed")) return false;
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
            if (!CheckStatus(state, result, L"AMF HEVC target bitrate configuration failed")) return false;
            if (rateControlMode != 0)
            {
                result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, maxBitrate);
                if (!CheckStatus(state, result, L"AMF HEVC peak bitrate configuration failed")) return false;
            }
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(state->fps, 1));
            if (!CheckStatus(state, result, L"AMF HEVC frame-rate configuration failed")) return false;
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, AMFConstructSize(state->width, state->height));
            if (!CheckStatus(state, result, L"AMF HEVC frame-size configuration failed")) return false;
            return true;
        }

        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCODING);
        if (!CheckStatus(state, result, L"AMF H.264 usage configuration failed")) return false;

        const auto preset = quality <= 0 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED
            : quality >= 2 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
            : AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, preset);
        if (!CheckStatus(state, result, L"AMF H.264 quality configuration failed")) return false;

        const auto rateControl = rateControlMode == 0
            ? AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR
            : AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, rateControl);
        if (!CheckStatus(state, result, L"AMF H.264 rate-control configuration failed")) return false;
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
        if (!CheckStatus(state, result, L"AMF H.264 target bitrate configuration failed")) return false;
        if (rateControlMode != 0)
        {
            result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, maxBitrate);
            if (!CheckStatus(state, result, L"AMF H.264 peak bitrate configuration failed")) return false;
        }
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(state->fps, 1));
        if (!CheckStatus(state, result, L"AMF H.264 frame-rate configuration failed")) return false;
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(state->width, state->height));
        if (!CheckStatus(state, result, L"AMF H.264 frame-size configuration failed")) return false;
        result = state->encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
        if (!CheckStatus(state, result, L"AMF H.264 B-frame configuration failed")) return false;
        return true;
    }

    void ReleaseOutputSlot(EncoderState* state, amf::AMFData* data)
    {
        amf_int64 slotIndex = -1;
        AMF_RESULT propertyResult = data->GetProperty(kSlotIndexProperty, &slotIndex);
        if (propertyResult != AMF_OK || slotIndex < 0 || static_cast<size_t>(slotIndex) >= state->slots.size())
        {
            SetError(state, L"AMF output did not preserve the input slot identifier.");
            state->outputError = true;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->slotMutex);
            auto& slot = state->slots[static_cast<size_t>(slotIndex)];
            if (slot->profileStart != 0)
            {
                state->profile.metrics[AmfProfileStage::SlotResidence].Add(AmfProfiler::Now() - slot->profileStart);
                state->profile.completed.fetch_add(1, std::memory_order_relaxed);
                slot->profileStart = 0;
            }
            slot->inFlight = false;
            ++state->completedFrames;
        }
        state->slotCv.notify_all();
    }

    void StartOutputThread(EncoderState* state)
    {
        state->outputStop = false;
        state->outputDone = false;
        state->outputThread = std::thread([state]()
        {
            LogLine(state, L"AMF output thread start");
            while (!state->outputStop)
            {
                amf::AMFDataPtr data;
                AmfProfileScope queryScope(state->profile, AmfProfileStage::QueryOutput);
                AMF_RESULT result = state->encoder->QueryOutput(&data);
                queryScope.Stop();
                if (result == AMF_EOF)
                {
                    break;
                }
                if (result == AMF_REPEAT || result == AMF_NEED_MORE_INPUT || (result == AMF_OK && data == nullptr))
                {
                    AmfProfileScope pollScope(state->profile, AmfProfileStage::OutputPollWait);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (result != AMF_OK)
                {
                    CheckStatus(state, result, L"AMF QueryOutput failed");
                    state->outputError = true;
                    break;
                }

                amf::AMFBufferPtr buffer(data);
                if (buffer == nullptr || buffer->GetNative() == nullptr || buffer->GetSize() == 0)
                {
                    SetError(state, L"AMF returned an invalid encoded buffer.");
                    state->outputError = true;
                    break;
                }

                amf_int64 outputType = -1;
                bool keyframe = false;
                if (state->codec == kCodecHevc)
                {
                    if (data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &outputType) == AMF_OK)
                    {
                        keyframe = outputType == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR
                            || outputType == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I;
                    }
                }
                else if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &outputType) == AMF_OK)
                {
                    keyframe = outputType == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR
                        || outputType == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
                }

                {
                    AmfProfileScope muxWaitScope(state->profile, AmfProfileStage::OutputMuxWait);
                    std::lock_guard<std::mutex> muxLock(state->muxMutex);
                    muxWaitScope.Stop();
                    AmfProfileScope bitstreamScope(state->profile, AmfProfileStage::BitstreamMux);
                    if (!ProcessEncodedBitstream(state, static_cast<const uint8_t*>(buffer->GetNative()), buffer->GetSize(), keyframe))
                    {
                        state->outputError = true;
                    }
                }
                ReleaseOutputSlot(state, data);
                if (state->outputError)
                {
                    break;
                }
            }

            state->outputDone = true;
            state->outputCv.notify_all();
            state->slotCv.notify_all();
            LogLine(state, L"AMF output thread exit");
        });
    }

    void StopOutputThread(EncoderState* state, bool flush)
    {
        if (!state || !state->outputThread.joinable())
        {
            return;
        }
        if (flush && state->encoder)
        {
            state->encoder->Flush();
        }
        state->outputStop = true;
        state->slotCv.notify_all();
        state->outputCv.notify_all();
        state->outputThread.join();
    }

    bool InitializeAmfEncoder(EncoderState* state, ID3D11Device* device, int width, int height, int fps,
        int bitrateKbps, int codec, int quality, int rateControlMode, int maxBitrateKbps,
        amf::AMF_SURFACE_FORMAT surfaceFormat, int texturePoolSize)
    {
        state->amfModule = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!state->amfModule)
        {
            SetError(state, L"AMD AMF runtime was not found in System32 (amfrt64.dll).");
            return false;
        }

        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(state->amfModule, AMF_INIT_FUNCTION_NAME));
        auto queryVersion = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(state->amfModule, AMF_QUERY_VERSION_FUNCTION_NAME));
        if (!init || !queryVersion)
        {
            SetError(state, L"AMD AMF runtime entry points are unavailable.");
            return false;
        }
        if (!CheckStatus(state, init(AMF_FULL_VERSION, &state->factory), L"AMFInit failed")) return false;
        if (!CheckStatus(state, queryVersion(&state->runtimeVersion), L"AMFQueryVersion failed")) return false;
        if (!CheckStatus(state, state->factory->CreateContext(&state->context), L"AMF CreateContext failed")) return false;
        if (!CheckStatus(state, state->context->InitDX11(device), L"AMF InitDX11 failed")) return false;

        state->width = width;
        state->height = height;
        state->fps = fps;
        state->codec = codec;
        state->surfaceFormat = surfaceFormat;
        state->device = device;
        state->device->AddRef();
        state->device->GetImmediateContext(&state->deviceContext);
        if (!state->deviceContext)
        {
            SetError(state, L"D3D11 immediate context is unavailable.");
            return false;
        }

        const wchar_t* componentId = codec == kCodecHevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC;
        if (!CheckStatus(state, state->factory->CreateComponent(state->context, componentId, &state->encoder), L"AMF encoder creation failed")) return false;
        if (!SetEncoderProperties(state, bitrateKbps, quality, rateControlMode, maxBitrateKbps)) return false;
        if (!CheckStatus(state, state->encoder->Init(surfaceFormat, width, height), L"AMF encoder initialization failed")) return false;

        const DXGI_FORMAT dxgiFormat = surfaceFormat == amf::AMF_SURFACE_RGBA
            ? DXGI_FORMAT_R8G8B8A8_UNORM
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = dxgiFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        state->slots.reserve(static_cast<size_t>(texturePoolSize));
        for (int i = 0; i < texturePoolSize; ++i)
        {
            auto slot = std::make_unique<EncoderState::FrameSlot>();
            HRESULT hr = state->device->CreateTexture2D(&desc, nullptr, &slot->texture);
            if (FAILED(hr) || !slot->texture)
            {
                SetError(state, L"Failed to create an owned AMF input texture.");
                return false;
            }
            if (!CheckStatus(state, state->context->CreateSurfaceFromDX11Native(slot->texture, &slot->surface, nullptr),
                L"AMF failed to wrap an owned D3D11 texture")) return false;
            state->slots.push_back(std::move(slot));
        }

        LogLine(state, L"AMF runtime version=" + std::to_wstring(state->runtimeVersion));
        LogLine(state, L"owned texture pool=" + std::to_wstring(texturePoolSize));
        LogLine(state, L"input path=copy_bgra_efc");
        StartOutputThread(state);
        return true;
    }

    bool EncodeTexture(EncoderState* state, ID3D11Texture2D* texture)
    {
        if (!state || !texture || !state->encoder || state->drainRequested || state->outputError)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);
        const DXGI_FORMAT expectedFormat = state->surfaceFormat == amf::AMF_SURFACE_RGBA
            ? DXGI_FORMAT_R8G8B8A8_UNORM
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (sourceDesc.Width != static_cast<UINT>(state->width)
            || sourceDesc.Height != static_cast<UINT>(state->height)
            || (sourceDesc.Format != expectedFormat
                && sourceDesc.Format != (expectedFormat == DXGI_FORMAT_B8G8R8A8_UNORM
                    ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                    : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)))
        {
            SetError(state, L"Input texture dimensions or format changed during export.");
            return false;
        }

        size_t slotIndex = 0;
        {
            AmfProfileScope slotWaitScope(state->profile, AmfProfileStage::SlotWait);
            std::unique_lock<std::mutex> lock(state->slotMutex);
            const bool ready = state->slotCv.wait_for(lock, std::chrono::seconds(30), [state]()
            {
                if (state->outputError || state->outputDone) return true;
                for (const auto& slot : state->slots) if (!slot->inFlight) return true;
                return false;
            });
            if (!ready || state->outputError || state->outputDone)
            {
                SetError(state, ready ? L"AMF output stopped before input completed." : L"Timed out waiting for an AMF texture slot.");
                return false;
            }
            bool found = false;
            for (size_t offset = 0; offset < state->slots.size(); ++offset)
            {
                const size_t candidate = (state->nextSlot + offset) % state->slots.size();
                if (!state->slots[candidate]->inFlight)
                {
                    slotIndex = candidate;
                    state->slots[candidate]->inFlight = true;
                    state->slots[candidate]->frameId = state->frameIndex;
                    if (state->profile.enabled.load(std::memory_order_relaxed))
                        state->slots[candidate]->profileStart = AmfProfiler::Now();
                    state->nextSlot = (candidate + 1) % state->slots.size();
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SetError(state, L"AMF texture pool entered an invalid state.");
                return false;
            }
        }

        auto& slot = state->slots[slotIndex];
        AmfProfileScope copyScope(state->profile, AmfProfileStage::CopyResourceCpu);
        state->deviceContext->CopyResource(slot->texture, texture);
        copyScope.Stop();
        const amf_pts pts = static_cast<amf_pts>((state->frameIndex * AMF_SECOND) / static_cast<uint64_t>(state->fps));
        const amf_pts duration = static_cast<amf_pts>(AMF_SECOND / state->fps);
        slot->surface->SetPts(pts);
        slot->surface->SetDuration(duration);
        if (!CheckStatus(state, slot->surface->SetProperty(kSlotIndexProperty, static_cast<amf_int64>(slotIndex)),
            L"Failed to attach AMF slot metadata"))
        {
            std::lock_guard<std::mutex> lock(state->slotMutex);
            slot->inFlight = false;
            state->slotCv.notify_all();
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;)
        {
            AmfProfileScope submitScope(state->profile, AmfProfileStage::SubmitInput);
            AMF_RESULT result = state->encoder->SubmitInput(slot->surface);
            submitScope.Stop();
            if (result == AMF_OK)
            {
                if (state->profile.enabled.load(std::memory_order_relaxed))
                    state->profile.accepted.fetch_add(1, std::memory_order_relaxed);
                ++state->acceptedFrames;
                ++state->frameIndex;
                return true;
            }
            if (result != AMF_INPUT_FULL && result != AMF_NEED_MORE_INPUT)
            {
                CheckStatus(state, result, L"AMF SubmitInput failed");
                break;
            }
            ++state->inputFullCount;
            if (state->profile.enabled.load(std::memory_order_relaxed))
                state->profile.retries.fetch_add(1, std::memory_order_relaxed);
            if (state->outputError || std::chrono::steady_clock::now() >= deadline)
            {
                SetError(state, L"Timed out while the AMF input queue was full.");
                break;
            }
            AmfProfileScope retryScope(state->profile, AmfProfileStage::InputRetryWait);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        {
            std::lock_guard<std::mutex> lock(state->slotMutex);
            slot->inFlight = false;
        }
        state->slotCv.notify_all();
        return false;
    }

    void StartWriterThread(EncoderState* state)
    {
        if (!state || state->writerStarted)
        {
            return;
        }
        LogLine(state, L"writer thread start");
        state->writerStop = false;
        state->writerError = false;
        state->writerStarted = true;
        state->writerThread = std::thread([state]()
        {
            for (;;)
            {
                EncoderState::EncodedSample sample;
                {
                    std::unique_lock<std::mutex> lock(state->writerMutex);
                    state->writerCv.wait(lock, [state]()
                    {
                        return state->writerStop || !state->sampleQueue.empty();
                    });
                    if (state->writerStop && state->sampleQueue.empty())
                    {
                        break;
                    }
                    sample = std::move(state->sampleQueue.front());
                    state->sampleQueue.pop_front();
                }

                if (sample.data.empty())
                {
                    continue;
                }

                AmfProfileScope sampleScope(state->profile, AmfProfileStage::WriterSample);
                std::lock_guard<std::mutex> fileLock(state->fileMutex);
                uint64_t offset = state->file.Tell();
                if (!state->file.Write(sample.data.data(), sample.data.size()))
                {
                    SetError(state, L"Failed to write sample data.");
                    state->writerError = true;
                    break;
                }
                if (sample.isAudio)
                {
                    state->audioSampleOffsets.push_back(offset);
                    state->audioSampleSizes.push_back(static_cast<uint32_t>(sample.data.size()));
                    state->audioSampleDurations.push_back(sample.audioDuration);
                    state->audioSampleTotal += sample.audioDuration;
                }
                else
                {
                    state->sampleOffsets.push_back(offset);
                    state->sampleSizes.push_back(static_cast<uint32_t>(sample.data.size()));
                    if (sample.keyframe)
                    {
                        state->syncSamples.push_back(static_cast<uint32_t>(state->sampleSizes.size()));
                    }
                }
            }
            LogLine(state, L"writer thread exit");
        });
    }

    void StopWriterThread(EncoderState* state)
    {
        if (!state || !state->writerStarted)
        {
            return;
        }
        LogLine(state, L"writer thread stop request");
        {
            std::lock_guard<std::mutex> lock(state->writerMutex);
            state->writerStop = true;
        }
        state->writerCv.notify_all();
        if (state->writerThread.joinable())
        {
            state->writerThread.join();
        }
        state->writerStarted = false;
        LogLine(state, L"writer thread stopped");
    }
}

#if 0 // Original exported NVENC ABI retained as upstream-reference text.
void* NvencCreate(ID3D11Device* device, int width, int height, int fps, int bitrateKbps, int codec, int quality, int fastPreset, int rateControlMode, int maxBitrateKbps, int bufferFormat, int hevcAsync, int enableDebugLog, const wchar_t* outputPath)
{
    if (!device || !outputPath)
    {
        return nullptr;
    }

    auto* state = new EncoderState();
    state->outputPath = outputPath;
    state->logEnabled = enableDebugLog != 0;
    OpenLog(state);
    LogLine(state, L"create encoder");

    if (!InitializeEncoder(state, device, width, height, fps, bitrateKbps, codec, quality, fastPreset, rateControlMode, maxBitrateKbps, static_cast<NV_ENC_BUFFER_FORMAT>(bufferFormat), hevcAsync))
    {
        return state;
    }

    std::vector<uint8_t> empty;
    if (!InitializeMp4Writer(state, codec, empty))
    {
        return state;
    }

    LogLine(state, L"encoder initialized");
    return state;
}

int NvencEncode(void* handle, ID3D11Texture2D* texture)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || !texture)
    {
        return 0;
    }

    if (!EncodeTexture(state, texture))
    {
        return 0;
    }

    return 1;
}

int NvencWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || !samples || sampleCount <= 0)
    {
        return 1;
    }

    if (!InitializeAudioEncoder(state, sampleRate, channels))
    {
        return 0;
    }

    const uint32_t frameSamples = 1024;
    state->audioPcmBuffer.reserve(state->audioPcmBuffer.size() + static_cast<size_t>(sampleCount));
    for (int i = 0; i < sampleCount; ++i)
    {
        float v = samples[i];
        v = ClampFloat(v, -1.0f, 1.0f);
        int16_t s = static_cast<int16_t>(v * 32767.0f);
        state->audioPcmBuffer.push_back(s);
    }

    const uint32_t channelsCount = static_cast<uint32_t>(channels);
    const size_t frameCount = static_cast<size_t>(frameSamples) * channelsCount;
    while (state->audioPcmBuffer.size() - state->audioPcmRead >= frameCount)
    {
        const int16_t* frame = state->audioPcmBuffer.data() + state->audioPcmRead;
        if (!EncodeAudioFrame(state, frame, frameSamples))
        {
            return 0;
        }
        state->audioPcmRead += frameCount;
    }

    if (state->audioPcmRead > 0 && state->audioPcmRead > 8192)
    {
        state->audioPcmBuffer.erase(state->audioPcmBuffer.begin(), state->audioPcmBuffer.begin() + static_cast<long long>(state->audioPcmRead));
        state->audioPcmRead = 0;
    }

    return 1;
}

int NvencFinalize(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state)
    {
        return 0;
    }

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    if (state->asyncEnabled)
    {
        if (state->asyncBitstreams.empty())
        {
            SetError(state, L"NVENC async resources are unavailable.");
            return 0;
        }

        const size_t asyncSlot = state->asyncIndex % state->asyncBitstreams.size();
        if (state->asyncPending[asyncSlot] && !ConsumeAsyncBitstream(state, asyncSlot))
        {
            return 0;
        }

        pic.outputBitstream = state->asyncBitstreams[asyncSlot];
        pic.completionEvent = state->asyncEvents[asyncSlot];
        auto status = state->funcs.nvEncEncodePicture(state->session, &pic);
        if (status != NV_ENC_SUCCESS)
        {
            SetError(state, L"nvEncEncodePicture (EOS) failed");
            return 0;
        }

        state->asyncIndex = (asyncSlot + 1) % state->asyncBitstreams.size();
        LogLine(state, L"encode EOS submitted (async)");
        state->asyncPending[asyncSlot] = false;
        if (!WaitForAsyncEvent(state, state->asyncEvents[asyncSlot], L"nvEnc async EOS wait"))
        {
            return 0;
        }
        if (!DrainAsyncBitstreams(state))
        {
            return 0;
        }
    }
    else
    {
        pic.outputBitstream = state->bitstream;
        auto status = state->funcs.nvEncEncodePicture(state->session, &pic);
        if (status != NV_ENC_SUCCESS)
        {
            SetError(state, L"nvEncEncodePicture (EOS) failed");
            return 0;
        }
        LogLine(state, L"encode EOS submitted");
    }

    if (!FinalizeMp4(state))
    {
        return 0;
    }

    return 1;
}

void NvencDestroy(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state)
    {
        return;
    }

    LogLine(state, L"destroy");
    if (!state->mp4Finalized)
    {
        DrainAsyncBitstreams(state);
        FinalizeMp4(state);
    }

    if (state->session)
    {
        ReleaseAsyncResources(state);
        if (state->registeredRgb)
        {
            state->funcs.nvEncUnregisterResource(state->session, state->registeredRgb);
            state->registeredRgb = nullptr;
        }
        if (state->registeredNv12)
        {
            state->funcs.nvEncUnregisterResource(state->session, state->registeredNv12);
            state->registeredNv12 = nullptr;
        }
        if (state->bitstream)
        {
            state->funcs.nvEncDestroyBitstreamBuffer(state->session, state->bitstream);
            state->bitstream = nullptr;
        }
        state->funcs.nvEncDestroyEncoder(state->session);
        state->session = nullptr;
    }

    if (state->aacEncoder)
    {
        state->aacEncoder->Release();
        state->aacEncoder = nullptr;
    }

    if (state->mfStarted)
    {
        MFShutdown();
        state->mfStarted = false;
    }

    if (state->comInitialized)
    {
        CoUninitialize();
        state->comInitialized = false;
    }

    if (state->nvencModule)
    {
        FreeLibrary(state->nvencModule);
        state->nvencModule = nullptr;
    }

    if (state->vpOutputView)
    {
        state->vpOutputView->Release();
        state->vpOutputView = nullptr;
    }
    if (state->rgbTexture)
    {
        state->rgbTexture->Release();
        state->rgbTexture = nullptr;
    }
    if (state->nv12Texture)
    {
        state->nv12Texture->Release();
        state->nv12Texture = nullptr;
    }
    if (state->videoProcessor)
    {
        state->videoProcessor->Release();
        state->videoProcessor = nullptr;
    }
    if (state->videoEnumerator)
    {
        state->videoEnumerator->Release();
        state->videoEnumerator = nullptr;
    }
    if (state->videoContext)
    {
        state->videoContext->Release();
        state->videoContext = nullptr;
    }
    if (state->videoDevice)
    {
        state->videoDevice->Release();
        state->videoDevice = nullptr;
    }
    if (state->deviceContext)
    {
        state->deviceContext->Release();
        state->deviceContext = nullptr;
    }
    if (state->device)
    {
        state->device->Release();
        state->device = nullptr;
    }

    StopWriterThread(state);
    // Ensure output file handle is released even if finalize failed or was skipped.
    state->file.Close();

    CloseLog(state);
    delete state;
}

const wchar_t* NvencGetLastError(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state)
    {
        return L"";
    }
    return state->lastError.c_str();
}
#endif

void* AmfCreate(ID3D11Device* device, int width, int height, int fps, int bitrateKbps, int codec, int quality,
    int rateControlMode, int maxBitrateKbps, int surfaceFormat, int texturePoolSize, int enableDebugLog,
    const wchar_t* outputPath)
{
    if (!device || !outputPath || width <= 0 || height <= 0 || fps <= 0)
    {
        return nullptr;
    }

    auto* state = new EncoderState();
    state->outputPath = outputPath;
    state->logEnabled = enableDebugLog != 0;
    OpenLog(state);
    LogLine(state, L"create AMF encoder");

    if (codec != kCodecH264 && codec != kCodecHevc)
    {
        SetError(state, L"Only H.264 and HEVC are supported by this AMF plugin version.");
        return state;
    }
    if (surfaceFormat != amf::AMF_SURFACE_BGRA && surfaceFormat != amf::AMF_SURFACE_RGBA)
    {
        SetError(state, L"Only BGRA and RGBA D3D11 input textures are supported.");
        return state;
    }
    texturePoolSize = ClampInt(texturePoolSize, 4, 8);

    if (!InitializeAmfEncoder(state, device, width, height, fps, bitrateKbps, codec, quality,
        rateControlMode, maxBitrateKbps, static_cast<amf::AMF_SURFACE_FORMAT>(surfaceFormat), texturePoolSize))
    {
        return state;
    }

    std::vector<uint8_t> empty;
    if (!InitializeMp4Writer(state, codec, empty))
    {
        return state;
    }

    LogLine(state, L"AMF encoder initialized");
    return state;
}

int AmfEncode(void* handle, ID3D11Texture2D* texture)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    return state && texture && EncodeTexture(state, texture) ? 1 : 0;
}

int AmfWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || !samples || sampleCount <= 0)
    {
        return 1;
    }

    AmfProfileScope audioScope(state->profile, AmfProfileStage::AudioWrite);
    std::lock_guard<std::mutex> muxLock(state->muxMutex);
    if (!InitializeAudioEncoder(state, sampleRate, channels))
    {
        return 0;
    }

    constexpr uint32_t frameSamples = 1024;
    state->audioPcmBuffer.reserve(state->audioPcmBuffer.size() + static_cast<size_t>(sampleCount));
    for (int i = 0; i < sampleCount; ++i)
    {
        const float value = ClampFloat(samples[i], -1.0f, 1.0f);
        state->audioPcmBuffer.push_back(static_cast<int16_t>(value * 32767.0f));
    }

    const size_t frameCount = static_cast<size_t>(frameSamples) * static_cast<uint32_t>(channels);
    while (state->audioPcmBuffer.size() - state->audioPcmRead >= frameCount)
    {
        const int16_t* frame = state->audioPcmBuffer.data() + state->audioPcmRead;
        if (!EncodeAudioFrame(state, frame, frameSamples))
        {
            return 0;
        }
        state->audioPcmRead += frameCount;
    }

    if (state->audioPcmRead > 8192)
    {
        state->audioPcmBuffer.erase(state->audioPcmBuffer.begin(),
            state->audioPcmBuffer.begin() + static_cast<long long>(state->audioPcmRead));
        state->audioPcmRead = 0;
    }
    return 1;
}

int AmfFinalize(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || !state->encoder)
    {
        return 0;
    }
    AmfProfileScope finalizeScope(state->profile, AmfProfileStage::Finalize);
    if (state->mp4Finalized)
    {
        return state->lastError.empty() ? 1 : 0;
    }

    state->drainRequested = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        AMF_RESULT result = state->encoder->Drain();
        if (result == AMF_OK)
        {
            break;
        }
        if (result != AMF_INPUT_FULL || std::chrono::steady_clock::now() >= deadline)
        {
            CheckStatus(state, result, L"AMF Drain failed");
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::unique_lock<std::mutex> lock(state->outputMutex);
        if (!state->outputCv.wait_until(lock, deadline, [state]() { return state->outputDone.load(); }))
        {
            SetError(state, L"Timed out waiting for AMF output drain.");
            return 0;
        }
    }
    if (state->outputThread.joinable())
    {
        state->outputThread.join();
    }
    if (state->outputError)
    {
        return 0;
    }
    if (state->acceptedFrames != state->completedFrames)
    {
        SetError(state, L"AMF completed-frame count does not match accepted input count.");
        return 0;
    }

    std::lock_guard<std::mutex> muxLock(state->muxMutex);
    if (!FinalizeMp4(state))
    {
        return 0;
    }
    LogLine(state, L"AMF finalize completed frames=" + std::to_wstring(state->completedFrames)
        + L" input_full=" + std::to_wstring(state->inputFullCount));
    return 1;
}

void AmfDestroy(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state)
    {
        return;
    }

    LogLine(state, L"destroy AMF encoder");
    StopOutputThread(state, true);
    if (!state->mp4Finalized && state->writerInitialized)
    {
        std::lock_guard<std::mutex> muxLock(state->muxMutex);
        FinalizeMp4(state);
    }

    if (state->encoder)
    {
        state->encoder->Terminate();
        state->encoder = nullptr;
    }
    state->slots.clear();
    if (state->context)
    {
        state->context->Terminate();
        state->context = nullptr;
    }
    state->factory = nullptr;

    if (state->aacEncoder)
    {
        state->aacEncoder->Release();
        state->aacEncoder = nullptr;
    }
    if (state->mfStarted)
    {
        MFShutdown();
        state->mfStarted = false;
    }
    if (state->comInitialized)
    {
        CoUninitialize();
        state->comInitialized = false;
    }
    if (state->deviceContext)
    {
        state->deviceContext->Release();
        state->deviceContext = nullptr;
    }
    if (state->device)
    {
        state->device->Release();
        state->device = nullptr;
    }
    if (state->amfModule)
    {
        FreeLibrary(state->amfModule);
        state->amfModule = nullptr;
    }

    StopWriterThread(state);
    state->file.Close();
    CloseLog(state);
    delete state;
}

const wchar_t* AmfGetLastError(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    return state ? state->lastError.c_str() : L"";
}

int AmfEnableProfiling(void* handle)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || state->frameIndex != 0 || state->drainRequested) return 0;
    state->profile.enabled.store(true, std::memory_order_relaxed);
    return 1;
}

int AmfGetProfile(void* handle, AmfProfileSnapshot* result, uint32_t resultSize)
{
    auto* state = reinterpret_cast<EncoderState*>(handle);
    if (!state || !result || resultSize != sizeof(AmfProfileSnapshot)) return 0;
    *result = state->profile.Snapshot();
    return 1;
}
