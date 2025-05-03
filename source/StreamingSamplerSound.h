#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

// A class that manages audio data streaming in chunks
class StreamingSamplerSound : public juce::SamplerSound
{
public:
    StreamingSamplerSound(const juce::String& name,
                         juce::AudioFormatReader& source,
                         const juce::BigInteger& midiNotes,
                         int midiNoteForNormalPitch,
                         double attackTimeSecs,
                         double releaseTimeSecs,
                         double maxSampleLengthSeconds,
                         const juce::String& sourceFilePath = "");
    
    ~StreamingSamplerSound() override;
    
    // SamplerSound overrides
    void setName(const juce::String& newName) { name = newName; }
    bool appliesToNote(int midiNoteNumber) override;
    bool appliesToChannel(int midiChannel) override;
    
    // Get a specific sample from the audio data, loading it if needed
    float getSample(int channel, int sampleIndex);
    
    // Streaming specific methods
    bool isFullyLoaded() const { return fullyLoaded.load(); }
    bool isLoading() const { return loading.load(); }
    
    // Manually trigger background loading of all chunks
    void startBackgroundLoading();
    
    // Get the original file length in samples
    int getLengthInSamples() const { return lengthInSamples; }
    int getNumChannels() const { return numChannels; }
    
    // Public properties needed by StreamingSamplerVoice
    juce::ADSR::Parameters params;
    double sourceSampleRate;
    int midiRootNote;
    
private:
    juce::String name;
    juce::BigInteger midiNotes;
    double attackTime = 0.01;
    double releaseTime = 0.01;
    
    // File path to enable streaming
    juce::String filePath;
    
    // File information
    int lengthInSamples = 0;
    int numChannels = 0;
    int bitsPerSample = 0;
    
    // Thread synchronization
    std::atomic<bool> fullyLoaded { false };
    std::atomic<bool> loading { false };
    
    // Chunk-based streaming system
    static constexpr int CHUNK_SIZE = 32768; // 32KB chunks
    static constexpr int INITIAL_CHUNKS = 2; // Number of chunks to preload
    
    struct Chunk {
        juce::AudioBuffer<float> data;
        std::atomic<bool> loaded { false };
    };
    
    // Store chunks in a vector
    std::vector<std::unique_ptr<Chunk>> chunks;
    
    // Reader for background loading
    std::unique_ptr<juce::AudioFormatReader> reader;
    
    // Mutex for thread-safe chunk access
    juce::CriticalSection chunkMutex;
    
    // Load a specific chunk
    void loadChunk(int chunkIndex);
    
    // Calculate which chunk contains a particular sample
    int sampleToChunkIndex(int sampleIndex) const;
    
    // Background thread for prefetching chunks
    std::unique_ptr<juce::TimeSliceThread> prefetchThread;
    class PrefetchJob;
    std::unique_ptr<PrefetchJob> prefetchJob;
    
    friend class PrefetchJob;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingSamplerSound)
};
