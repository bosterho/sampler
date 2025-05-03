#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

// Forward declarations
class StreamingSamplerSound;
class PrefetchClient;

// Helper for memory tracking - simplified placeholder
class MemoryTracker
{
public:
    static void trackAllocation(size_t /*bytes*/, const char* /*description*/)
    {
        // Memory tracking disabled
    }
    
    static void trackDeallocation(size_t /*bytes*/, const char* /*description*/)
    {
        // Memory tracking disabled
    }
    
    static void resetTracking()
    {
        // Memory tracking disabled
    }
    
private:
    // Empty placeholder
    static std::atomic<size_t> totalAllocated;
};

// A class that manages a thread for stream-loading sample chunks
class StreamingThreadManager
{
public:
    StreamingThreadManager();
    ~StreamingThreadManager();
    
    // Add a sample to be managed by this thread
    void addSound(StreamingSamplerSound* sound);
    
    // Remove a sample from management
    void removeSound(StreamingSamplerSound* sound);
    
    // Prioritize loading a specific sample
    void prioritizeSound(StreamingSamplerSound* sound);
    
    // Unload inactive chunks to save memory
    void checkAndUnloadInactiveChunks();
    
    // Reference to constant from StreamingSamplerSound for consistency
    static constexpr int INITIAL_CHUNKS = StreamingSamplerSound::INITIAL_CHUNKS;
    
private:
    // The single background thread for all samples
    std::unique_ptr<juce::TimeSliceThread> prefetchThread;
    
    // Instance of the prefetch client
    std::unique_ptr<PrefetchClient> prefetchClient;
    
    // The list of sounds being managed by this thread
    juce::Array<StreamingSamplerSound*> managedSounds;
    
    // Lock for thread-safe access to the sounds list
    juce::CriticalSection soundsLock;
    
    // Allow prefetch client to access our private members
    friend class PrefetchClient;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingThreadManager)
};

// A class that manages audio data streaming in chunks
class StreamingSamplerSound : public juce::SynthesiserSound
{
public:
    StreamingSamplerSound(const juce::String& name,
                         juce::AudioFormatReader& source,
                         const juce::BigInteger& midiNotes,
                         int midiNoteForNormalPitch,
                         double attackTimeSecs,
                         double releaseTimeSecs,
                         double maxSampleLengthSeconds,
                         StreamingThreadManager* threadManager,
                         const juce::String& sourceFilePath = "");
    
    ~StreamingSamplerSound() override;
    
    // SynthesiserSound overrides (not SamplerSound)
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
    const juce::String& getName() const { return name; }
    
    // Load a specific chunk (public for thread manager)
    bool loadChunk(int chunkIndex);
    
    // Add a method to unload a chunk
    bool unloadChunk(int chunkIndex);
    
    // Debug methods
    void dumpMemoryUsage() const;
    size_t getCurrentMemoryUsage() const;
    
    // Get number of loaded chunks
    int getNumberOfChunks() const { return chunks.size(); }
    int getNumberOfLoadedChunks() const;
    
    // Constants for chunk system
    static constexpr int CHUNK_SIZE = 32768; // 32KB chunks
    static constexpr int INITIAL_CHUNKS = 2; // Number of chunks to preload
    
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
    
    struct Chunk {
        juce::AudioBuffer<float> data;
        std::atomic<bool> loaded { false };
    };
    
    // Store chunks in a vector
    std::vector<std::unique_ptr<Chunk>> chunks;
    
    // Track when chunks were last accessed
    std::vector<juce::int64> chunkLastAccessTime;
    
    // Reader for background loading
    std::unique_ptr<juce::AudioFormatReader> reader;
    
    // Mutex for thread-safe chunk access
    juce::CriticalSection chunkMutex;
    
    // Calculate which chunk contains a particular sample
    int sampleToChunkIndex(int sampleIndex) const;
    
    // Pointer to the thread manager (not owned by this class)
    StreamingThreadManager* threadManager;
    
    // Make the prefetch client and thread manager friends so they can access our private data
    friend class PrefetchClient;
    friend class StreamingThreadManager;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingSamplerSound)
};

// Definition of the PrefetchClient as a standalone class
class PrefetchClient : public juce::TimeSliceClient
{
public:
    PrefetchClient(StreamingThreadManager& owner);
    
    int useTimeSlice() override;
    
private:
    StreamingThreadManager& manager;
};
