#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <dxgicommon.h>
#include <wrl/client.h>
#include <string>

// MF-style GPU conversion boundary used only by the opt-in experiment:
// host BGRA/RGBA -> owned compatible RGB texture -> owned NV12 texture.
// The first copy intentionally removes dependencies on the host texture's bind
// flags. VideoProcessorBlt and AMF then operate only on plugin-owned resources.
class AmfNv12Processor
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoContext1> videoContext1;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> compatibleInput;
    ComPtr<ID3D11VideoProcessorInputView> inputView;

public:
    bool Initialize(ID3D11Device* inputDevice, UINT width, UINT height, UINT fps,
        DXGI_FORMAT sourceFormat, std::wstring& error)
    {
        if (!inputDevice || width == 0 || height == 0 || fps == 0)
        {
            error = L"MF-style NV12 conversion received invalid initialization parameters.";
            return false;
        }

        device = inputDevice;
        device->GetImmediateContext(&context);
        HRESULT hr = device.As(&videoDevice);
        if (SUCCEEDED(hr)) hr = context.As(&videoContext);
        if (SUCCEEDED(hr)) hr = context.As(&videoContext1);
        if (FAILED(hr) || !videoDevice || !videoContext || !videoContext1)
        {
            error = L"D3D11.1 VideoProcessor color-space control is unavailable for MF-style NV12 conversion.";
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = { fps, 1 };
        content.InputWidth = width;
        content.InputHeight = height;
        content.OutputFrameRate = { fps, 1 };
        content.OutputWidth = width;
        content.OutputHeight = height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator);
        if (FAILED(hr) || !enumerator)
        {
            error = L"Could not create a D3D11 VideoProcessor enumerator for MF-style NV12 conversion.";
            return false;
        }

        UINT sourceSupport = 0, outputSupport = 0;
        hr = enumerator->CheckVideoProcessorFormat(sourceFormat, &sourceSupport);
        if (SUCCEEDED(hr)) hr = enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputSupport);
        if (FAILED(hr) || (sourceSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0
            || (outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
        {
            error = L"The current GPU driver does not expose the required RGB-input/NV12-output VideoProcessor formats.";
            return false;
        }

        hr = videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor);
        if (FAILED(hr) || !processor)
        {
            error = L"Could not create the D3D11 VideoProcessor for MF-style NV12 conversion.";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = sourceFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // BindFlags=0 is deliberate: the VideoProcessor input-view API accepts
        // ordinary decoder-compatible textures and this avoids host bind-flag assumptions.
        hr = device->CreateTexture2D(&desc, nullptr, &compatibleInput);
        if (FAILED(hr) || !compatibleInput)
        {
            error = L"Could not create the compatible RGB staging texture for MF-style NV12 conversion.";
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;
        hr = videoDevice->CreateVideoProcessorInputView(compatibleInput.Get(), enumerator.Get(), &inputDesc, &inputView);
        if (FAILED(hr) || !inputView)
        {
            error = L"Could not create the compatible RGB VideoProcessor input view.";
            return false;
        }

        videoContext1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        videoContext1->VideoProcessorSetOutputColorSpace1(processor.Get(),
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        return true;
    }

    bool CreateOutputView(ID3D11Texture2D* texture,
        ID3D11VideoProcessorOutputView** output, std::wstring& error)
    {
        if (!texture || !output || !videoDevice || !enumerator)
        {
            error = L"MF-style NV12 output-view creation received invalid state.";
            return false;
        }
        *output = nullptr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        const HRESULT hr = videoDevice->CreateVideoProcessorOutputView(texture, enumerator.Get(), &desc, output);
        if (FAILED(hr) || !*output)
        {
            error = L"Could not create an NV12 VideoProcessor output view.";
            return false;
        }
        return true;
    }

    bool Convert(ID3D11VideoProcessorOutputView* output, ID3D11Texture2D* source,
        std::wstring& error)
    {
        if (!output || !source || !context || !videoContext || !processor || !inputView)
        {
            error = L"MF-style NV12 conversion received invalid state.";
            return false;
        }

        context->CopyResource(compatibleInput.Get(), source);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = inputView.Get();
        const HRESULT hr = videoContext->VideoProcessorBlt(processor.Get(), output, 0, 1, &stream);
        if (FAILED(hr))
        {
            error = L"D3D11 VideoProcessorBlt failed during MF-style NV12 conversion.";
            return false;
        }
        return true;
    }
};
