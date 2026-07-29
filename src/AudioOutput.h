#pragma once

class AudioOutput
{
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Initialize(
        std::uint32_t sampleRate,
        std::uint16_t channels);
    bool Submit(std::vector<std::uint8_t> samples);
    void Pause();
    void Resume();
    void SetVolume(float volume);
    void SetPan(float pan);
    void Reset();

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
