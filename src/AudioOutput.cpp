#include "PCH.h"

#include "AudioOutput.h"

#include <xaudio2.h>

namespace
{
    class VoiceCallback final : public IXAudio2VoiceCallback
    {
    public:
        VoiceCallback() :
            bufferFinished_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {}

        ~VoiceCallback()
        {
            if (bufferFinished_) {
                CloseHandle(bufferFinished_);
            }
        }

        HANDLE BufferFinishedEvent() const
        {
            return bufferFinished_;
        }

        std::vector<std::uint8_t>* Own(
            std::vector<std::uint8_t> samples)
        {
            auto buffer =
                std::make_unique<std::vector<std::uint8_t>>(
                    std::move(samples));
            auto* context = buffer.get();
            std::scoped_lock lock(buffersMutex_);
            buffers_.push_back(std::move(buffer));
            return context;
        }

        void Release(void* context) noexcept
        {
            std::scoped_lock lock(buffersMutex_);
            const auto buffer = std::ranges::find_if(
                buffers_,
                [context](const auto& candidate) {
                    return candidate.get() == context;
                });
            if (buffer != buffers_.end()) {
                buffers_.erase(buffer);
            }
        }

        void Clear() noexcept
        {
            std::scoped_lock lock(buffersMutex_);
            buffers_.clear();
        }

        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32)
            noexcept override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()
            noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}

        void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override
        {
            Release(context);
            SetEvent(bufferFinished_);
        }

        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void* context, HRESULT error)
            noexcept override
        {
            Release(context);
            spdlog::error(
                "XAudio2 source voice error: {:08X}",
                static_cast<std::uint32_t>(error));
            SetEvent(bufferFinished_);
        }

    private:
        HANDLE bufferFinished_{ nullptr };
        std::mutex buffersMutex_;
        std::vector<
            std::unique_ptr<std::vector<std::uint8_t>>> buffers_;
    };
}

class AudioOutput::Impl
{
public:
    ~Impl()
    {
        Reset();
        if (engine_) {
            engine_->Release();
            engine_ = nullptr;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    bool Initialize(
        const std::uint32_t sampleRate,
        const std::uint16_t channels)
    {
        Reset();
        if (!comInitialized_) {
            const HRESULT comResult = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            if (FAILED(comResult) &&
                comResult != RPC_E_CHANGED_MODE) {
                spdlog::error(
                    "CoInitializeEx for XAudio2 failed: {:08X}",
                    static_cast<std::uint32_t>(comResult));
                return false;
            }
            comInitialized_ = SUCCEEDED(comResult);
        }
        if (!engine_) {
            const HRESULT result = XAudio2Create(
                &engine_,
                0,
                XAUDIO2_DEFAULT_PROCESSOR);
            if (FAILED(result)) {
                spdlog::error(
                    "XAudio2Create failed: {:08X}",
                    static_cast<std::uint32_t>(result));
                return false;
            }
        }

        HRESULT result = engine_->CreateMasteringVoice(&masteringVoice_);
        if (FAILED(result)) {
            spdlog::error(
                "CreateMasteringVoice failed: {:08X}",
                static_cast<std::uint32_t>(result));
            Reset();
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(
            channels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec =
            sampleRate * format.nBlockAlign;

        result = engine_->CreateSourceVoice(
            &sourceVoice_,
            &format,
            0,
            XAUDIO2_DEFAULT_FREQ_RATIO,
            &callback_);
        if (FAILED(result)) {
            spdlog::error(
                "CreateSourceVoice failed: {:08X}",
                static_cast<std::uint32_t>(result));
            Reset();
            return false;
        }

        result = sourceVoice_->Start();
        if (FAILED(result)) {
            spdlog::error(
                "Starting the XAudio2 source voice failed: {:08X}",
                static_cast<std::uint32_t>(result));
            Reset();
            return false;
        }
        spdlog::info(
            "Started XAudio2 output: {} Hz, {} channels, signed 16-bit",
            sampleRate,
            channels);
        paused_.store(false, std::memory_order_release);
        channels_ = channels;
        SetVolume(volume_);
        SetPan(pan_);
        return true;
    }

    bool Submit(std::vector<std::uint8_t> samples)
    {
        if (!sourceVoice_ || samples.empty()) {
            return false;
        }

        XAUDIO2_VOICE_STATE state{};
        sourceVoice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (state.BuffersQueued >= kMaximumQueuedBuffers) {
            WaitForSingleObject(
                callback_.BufferFinishedEvent(),
                25);
            if (!sourceVoice_) {
                return false;
            }
            sourceVoice_->GetState(
                &state,
                XAUDIO2_VOICE_NOSAMPLESPLAYED);
        }

        auto* ownedSamples = callback_.Own(std::move(samples));
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes =
            static_cast<UINT32>(ownedSamples->size());
        buffer.pAudioData = ownedSamples->data();
        buffer.pContext = ownedSamples;
        const HRESULT result = sourceVoice_->SubmitSourceBuffer(&buffer);
        if (FAILED(result)) {
            callback_.Release(ownedSamples);
            spdlog::error(
                "SubmitSourceBuffer failed: {:08X}",
                static_cast<std::uint32_t>(result));
            return false;
        }
        return true;
    }

    void Pause()
    {
        if (sourceVoice_ &&
            !paused_.load(std::memory_order_acquire)) {
            const HRESULT result = sourceVoice_->Stop();
            if (SUCCEEDED(result)) {
                paused_.store(true, std::memory_order_release);
            } else {
                spdlog::warn(
                    "Pausing XAudio2 failed: {:08X}",
                    static_cast<std::uint32_t>(result));
            }
        }
    }

    void Resume()
    {
        if (sourceVoice_ &&
            paused_.load(std::memory_order_acquire)) {
            const HRESULT result = sourceVoice_->Start();
            if (SUCCEEDED(result)) {
                paused_.store(false, std::memory_order_release);
            } else {
                spdlog::warn(
                    "Resuming XAudio2 failed: {:08X}",
                    static_cast<std::uint32_t>(result));
            }
        }
    }

    void SetVolume(const float volume)
    {
        volume_ = std::clamp(volume, 0.0F, 2.0F);
        if (sourceVoice_) {
            const HRESULT result = sourceVoice_->SetVolume(volume_);
            if (FAILED(result)) {
                spdlog::warn(
                    "Setting XAudio2 volume failed: {:08X}",
                    static_cast<std::uint32_t>(result));
            }
        }
    }

    void SetPan(const float pan)
    {
        pan_ = std::clamp(pan, -1.0F, 1.0F);
        if (!sourceVoice_ || !masteringVoice_ || channels_ != 2) {
            return;
        }
        // Let XAudio route ordinary stereo directly. In particular, Wine's
        // XAudio implementation can expose a multichannel mastering voice
        // even for headphones, and overriding its default matrix at center
        // pan can collapse the right channel.
        if (std::abs(pan_) < 0.0001F) {
            return;
        }

        XAUDIO2_VOICE_DETAILS destinationDetails{};
        masteringVoice_->GetVoiceDetails(&destinationDetails);
        if (destinationDetails.InputChannels < 2) {
            return;
        }

        const float left = pan_ > 0.0F ? 1.0F - pan_ : 1.0F;
        const float right = pan_ < 0.0F ? 1.0F + pan_ : 1.0F;
        std::vector<float> matrix(
            static_cast<std::size_t>(2) *
                destinationDetails.InputChannels,
            0.0F);
        matrix[0] = left;
        matrix[destinationDetails.InputChannels + 1] = right;
        const HRESULT result = sourceVoice_->SetOutputMatrix(
            masteringVoice_,
            2,
            destinationDetails.InputChannels,
            matrix.data());
        if (FAILED(result)) {
            spdlog::warn(
                "Setting XAudio2 pan matrix failed: {:08X}",
                static_cast<std::uint32_t>(result));
        }
    }

    void Reset()
    {
        if (sourceVoice_) {
            sourceVoice_->Stop();
            sourceVoice_->FlushSourceBuffers();
            sourceVoice_->DestroyVoice();
            sourceVoice_ = nullptr;
        }
        callback_.Clear();
        paused_.store(false, std::memory_order_release);
        channels_ = 0;
        if (masteringVoice_) {
            masteringVoice_->DestroyVoice();
            masteringVoice_ = nullptr;
        }
    }

private:
    // Keep latency low enough that a seek can flush stale audio promptly.
    static constexpr UINT32 kMaximumQueuedBuffers{ 2 };

    IXAudio2* engine_{ nullptr };
    IXAudio2MasteringVoice* masteringVoice_{ nullptr };
    IXAudio2SourceVoice* sourceVoice_{ nullptr };
    VoiceCallback callback_;
    bool comInitialized_{ false };
    std::atomic<bool> paused_{ false };
    std::uint16_t channels_{ 0 };
    float volume_{ 1.0F };
    float pan_{ 0.0F };
};

AudioOutput::AudioOutput() :
    implementation_(std::make_unique<Impl>())
{}

AudioOutput::~AudioOutput() = default;

bool AudioOutput::Initialize(
    const std::uint32_t sampleRate,
    const std::uint16_t channels)
{
    return implementation_->Initialize(sampleRate, channels);
}

bool AudioOutput::Submit(std::vector<std::uint8_t> samples)
{
    return implementation_->Submit(std::move(samples));
}

void AudioOutput::Pause()
{
    implementation_->Pause();
}

void AudioOutput::Resume()
{
    implementation_->Resume();
}

void AudioOutput::SetVolume(const float volume)
{
    implementation_->SetVolume(volume);
}

void AudioOutput::SetPan(const float pan)
{
    implementation_->SetPan(pan);
}

void AudioOutput::Reset()
{
    implementation_->Reset();
}
