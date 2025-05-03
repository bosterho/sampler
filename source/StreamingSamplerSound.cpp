#include "StreamingSamplerSound.h"

// Inner class for background prefetching of chunks
class StreamingSamplerSound::PrefetchJob : public juce::TimeSliceClient
{
public:
    PrefetchJob(StreamingSamplerSound& owner) : sound(owner) {}
    
    int useTimeSlice() override
    {
        // Try to prefetch the next chunk that's needed
        int numChunks = sound.chunks.size();
        
        for (int i = 0; i < numChunks; ++i)
        {
            if (!sound.chunks[i]->loaded.load())
            {
                sound.loadChunk(i);
                return 250; // Wait for 250ms before prefetching the next chunk
            }
        }
        
        // Check if we've loaded all chunks
        bool allLoaded = true;
        for (int i = 0; i < numChunks; ++i)
        {
            if (!sound.chunks[i]->loaded.load())
            {
                allLoaded = false;
                break;
            }
        }
        
        if (allLoaded)
            sound.fullyLoaded.store(true);
            
        return 1000; // Check again in 1 second if no chunks needed loading
    }
    
private:
    StreamingSamplerSound& sound;
};

StreamingSamplerSound::StreamingSamplerSound(const juce::String& soundName,
                                           juce::AudioFormatReader& source,
                                           const juce::BigInteger& notes,
                                           int midiNoteForNormalPitch,
                                           double attackTimeSecs,
                                           double releaseTimeSecs,
                                           double maxSampleLengthSeconds,
                                           const juce::String& sourceFilePath)
    : juce::SamplerSound(soundName, source, notes, midiNoteForNormalPitch, 
                        attackTimeSecs, releaseTimeSecs, maxSampleLengthSeconds),
      name(soundName),
      midiNotes(notes),
      midiRootNote(midiNoteForNormalPitch),
      attackTime(attackTimeSecs),
      releaseTime(releaseTimeSecs),
      filePath(sourceFilePath),
      lengthInSamples(static_cast<int>(source.lengthInSamples)),
      numChannels(source.numChannels),
      bitsPerSample(source.bitsPerSample),
      sourceSampleRate(source.sampleRate),
      prefetchThread(new juce::TimeSliceThread("Sample Prefetch Thread"))
{
    // Setup ADSR parameters
    params.attack = static_cast<float>(attackTime);
    params.release = static_cast<float>(releaseTime);
    params.decay = 0.1f;
    params.sustain = 1.0f;

    // Start the prefetch thread
    prefetchThread->startThread(juce::Thread::Priority::normal);
    
    // Calculate how many chunks we need
    int numChunksNeeded = (lengthInSamples + CHUNK_SIZE - 1) / CHUNK_SIZE;
    chunks.resize(numChunksNeeded);
    
    // Create empty chunks
    for (int i = 0; i < numChunksNeeded; ++i)
    {
        chunks[i] = std::make_unique<Chunk>();
    }
    
    // Create a reader for the file
    if (!filePath.isEmpty())
    {
        juce::File file(filePath);
        if (file.existsAsFile())
        {
            juce::AudioFormatManager formatManager;
            formatManager.registerBasicFormats();
            reader.reset(formatManager.createReaderFor(file));
        }
    }
    
    // Load the initial chunks
    for (int i = 0; i < juce::jmin(INITIAL_CHUNKS, numChunksNeeded); ++i)
    {
        loadChunk(i);
    }
    
    // Create and start the prefetch job
    prefetchJob.reset(new PrefetchJob(*this));
    prefetchThread->addTimeSliceClient(prefetchJob.get());
}

StreamingSamplerSound::~StreamingSamplerSound()
{
    // Stop background loading
    if (prefetchThread != nullptr)
    {
        if (prefetchJob != nullptr)
            prefetchThread->removeTimeSliceClient(prefetchJob.get());
            
        prefetchThread->stopThread(1000);
    }
}

bool StreamingSamplerSound::appliesToNote(int midiNoteNumber)
{
    return midiNotes[midiNoteNumber];
}

bool StreamingSamplerSound::appliesToChannel(int /*midiChannel*/)
{
    return true;
}

float StreamingSamplerSound::getSample(int channel, int sampleIndex)
{
    // Make sure the requested sample is within range
    if (sampleIndex < 0 || sampleIndex >= lengthInSamples || channel < 0 || channel >= numChannels)
        return 0.0f;
    
    // Calculate which chunk contains this sample
    int chunkIndex = sampleToChunkIndex(sampleIndex);
    
    // Calculate the position within the chunk
    int sampleInChunk = sampleIndex - (chunkIndex * CHUNK_SIZE);
    
    // Check if the chunk is loaded
    const juce::ScopedLock sl(chunkMutex);
    
    if (chunkIndex < chunks.size())
    {
        auto& chunk = chunks[chunkIndex];
        
        // If this chunk isn't loaded yet, load it now
        if (!chunk->loaded.load())
        {
            loadChunk(chunkIndex);
        }
        
        // Return the sample data if the chunk has data
        if (chunk->loaded.load() && chunk->data.getNumSamples() > 0)
        {
            // Make sure we're not reading past the end of the chunk
            sampleInChunk = juce::jmin(sampleInChunk, chunk->data.getNumSamples() - 1);
            
            if (chunk->data.getNumChannels() > channel)
                return chunk->data.getSample(channel, sampleInChunk);
        }
    }
    
    // If we couldn't get the sample, return silence
    return 0.0f;
}

void StreamingSamplerSound::loadChunk(int chunkIndex)
{
    if (chunkIndex < 0 || chunkIndex >= chunks.size())
        return;
        
    // Skip if this chunk is already loaded
    if (chunks[chunkIndex]->loaded.load())
        return;
        
    // Skip if we don't have a valid reader
    if (reader == nullptr)
        return;
        
    const juce::ScopedLock sl(chunkMutex);
    
    // Calculate the start sample for this chunk
    auto startSample = chunkIndex * CHUNK_SIZE;
    
    // Calculate the number of samples for this chunk (might be less for the last chunk)
    auto samplesThisChunk = juce::jmin(CHUNK_SIZE, lengthInSamples - startSample);
    
    if (samplesThisChunk <= 0)
        return;
    
    // Resize the buffer for this chunk
    chunks[chunkIndex]->data.setSize(numChannels, samplesThisChunk);
    
    // Read the data for this chunk
    reader->read(&(chunks[chunkIndex]->data), 0, samplesThisChunk, startSample, true, true);
    
    // Mark the chunk as loaded
    chunks[chunkIndex]->loaded.store(true);
}

int StreamingSamplerSound::sampleToChunkIndex(int sampleIndex) const
{
    return sampleIndex / CHUNK_SIZE;
}

void StreamingSamplerSound::startBackgroundLoading()
{
    // If already fully loaded, don't do anything
    if (fullyLoaded.load())
        return;
        
    // Make sure all chunks are marked for loading
    if (prefetchJob != nullptr && prefetchThread != nullptr)
    {
        // Bump up the priority of this sound in the prefetch queue
        // The prefetch thread will take care of loading the chunks
        prefetchThread->moveToFrontOfQueue(prefetchJob.get());
    }
}
