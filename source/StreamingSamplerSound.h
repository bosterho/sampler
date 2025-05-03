#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

class StreamingSamplerSound : public juce::SamplerSound
{
public:
    StreamingSamplerSound(const juce::String& name,
                         juce::AudioFormatReader& source,
                         const juce::BigInteger& midiNotes,
                         int midiNoteForNormalPitch,
                         double attackTimeSecs,
                         double releaseTimeSecs,
                         double maxSampleLengthSeconds);
    
    ~StreamingSamplerSound() override;
    
    // SamplerSound overrides
    void setName(const juce::String& newName) { name = newName; }
    bool appliesToNote(int midiNoteNumber) override;
    bool appliesToChannel(int midiChannel) override;
    
    // Streaming specific methods
    void startBackgroundLoading();
    bool isFullyLoaded() const { return fullyLoaded.load(); }
    bool isLoading() const { return loading.load(); }
    
    // Get the buffer (might be partially loaded)
    const juce::AudioBuffer<float>& getAudioData() const { return data; }
    
private:
    juce::String name;
    juce::AudioBuffer<float> data;
    juce::BigInteger midiNotes;
    int midiRootNote = 60;
    double attackTime = 0.01;
    double releaseTime = 0.01;
    
    // File path to enable reloading
    juce::String filePath;
    
    // Format reader for background loading
    std::unique_ptr<juce::AudioFormatReader> backgroundReader;
    
    // Thread synchronization
    std::atomic<bool> fullyLoaded { false };
    std::atomic<bool> loading { false };
    
    // Background loading method
    void loadCompleteFile();
    
    static constexpr int INITIAL_LOAD_SIZE = 60 * 1024; // 60 KB in samples
    
    // Convert bytes to samples based on file format
    static int bytesToSamples(int bytes, int numChannels, int bitsPerSample)
    {
        return bytes / ((bitsPerSample / 8) * numChannels);
    }
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingSamplerSound)
};
