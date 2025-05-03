#include "StreamingSamplerSound.h"

// Implementation of the PrefetchClient
PrefetchClient::PrefetchClient(StreamingThreadManager& owner) 
    : manager(owner) 
{
}

int PrefetchClient::useTimeSlice()
{
    // Loop through all sounds and try to load a chunk
    const juce::ScopedLock sl(manager.soundsLock);
    
    for (auto* sound : manager.managedSounds)
    {
        if (sound == nullptr)
            continue;
            
        // Check if this sound needs any chunks loaded
        int numChunks = sound->chunks.size();
        bool loadedChunk = false;
        
        for (int i = 0; i < numChunks; ++i)
        {
            if (!sound->chunks[i]->loaded.load())
            {
                if (sound->loadChunk(i))
                {
                    loadedChunk = true;
                    break; // Only load one chunk per time slice
                }
            }
        }
        
        if (loadedChunk)
            return 10; // Small pause before loading the next chunk
    }
    
    // Update fully loaded status for all sounds
    for (auto* sound : manager.managedSounds)
    {
        if (sound == nullptr)
            continue;
            
        // Check if all chunks are loaded for this sound
        bool allLoaded = true;
        int numChunks = sound->chunks.size();
        
        for (int i = 0; i < numChunks; ++i)
        {
            if (!sound->chunks[i]->loaded.load())
            {
                allLoaded = false;
                break;
            }
        }
        
        if (allLoaded)
            sound->fullyLoaded.store(true);
    }
    
    return 100; // Longer pause if no chunks needed loading
}

// Implementation of StreamingThreadManager
StreamingThreadManager::StreamingThreadManager()
    : prefetchThread(new juce::TimeSliceThread("Sample Prefetch Thread"))
{
    prefetchThread->startThread(juce::Thread::Priority::background);
    prefetchClient.reset(new PrefetchClient(*this));
    prefetchThread->addTimeSliceClient(prefetchClient.get());
}

StreamingThreadManager::~StreamingThreadManager()
{
    if (prefetchThread != nullptr)
    {
        if (prefetchClient != nullptr)
            prefetchThread->removeTimeSliceClient(prefetchClient.get());
            
        prefetchThread->stopThread(500);
    }
}

void StreamingThreadManager::addSound(StreamingSamplerSound* sound)
{
    if (sound == nullptr)
        return;
        
    const juce::ScopedLock sl(soundsLock);
    if (!managedSounds.contains(sound))
        managedSounds.add(sound);
}

void StreamingThreadManager::removeSound(StreamingSamplerSound* sound)
{
    if (sound == nullptr)
        return;
        
    const juce::ScopedLock sl(soundsLock);
    managedSounds.removeAllInstancesOf(sound);
}

void StreamingThreadManager::prioritizeSound(StreamingSamplerSound* sound)
{
    if (sound == nullptr)
        return;
        
    const juce::ScopedLock sl(soundsLock);
    
    // Move this sound to the front of the array so it gets processed first
    if (managedSounds.contains(sound))
    {
        managedSounds.removeAllInstancesOf(sound);
        managedSounds.insert(0, sound);
    }
    else
    {
        // Sound wasn't in the list yet, so add it at the front
        managedSounds.insert(0, sound);
    }
}

// Implementation of StreamingSamplerSound
StreamingSamplerSound::StreamingSamplerSound(const juce::String& soundName,
                                         juce::AudioFormatReader& source,
                                         const juce::BigInteger& notes,
                                         int midiNoteForNormalPitch,
                                         double attackTimeSecs,
                                         double releaseTimeSecs,
                                         double maxSampleLengthSeconds,
                                         StreamingThreadManager* thManager,
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
      threadManager(thManager)
{
    // Setup ADSR parameters
    params.attack = static_cast<float>(attackTime);
    params.release = static_cast<float>(releaseTime);
    params.decay = 0.1f;
    params.sustain = 1.0f;

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
    
    // Register with the thread manager if available
    if (threadManager != nullptr)
        threadManager->addSound(this);
}

StreamingSamplerSound::~StreamingSamplerSound()
{
    // Unregister from the thread manager if available
    if (threadManager != nullptr)
        threadManager->removeSound(this);
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

bool StreamingSamplerSound::loadChunk(int chunkIndex)
{
    if (chunkIndex < 0 || chunkIndex >= chunks.size())
        return false;
        
    // Skip if this chunk is already loaded
    if (chunks[chunkIndex]->loaded.load())
        return false;
        
    // Skip if we don't have a valid reader
    if (reader == nullptr)
        return false;
        
    const juce::ScopedLock sl(chunkMutex);
    
    // Calculate the start sample for this chunk
    auto startSample = chunkIndex * CHUNK_SIZE;
    
    // Calculate the number of samples for this chunk (might be less for the last chunk)
    auto samplesThisChunk = juce::jmin(CHUNK_SIZE, lengthInSamples - startSample);
    
    if (samplesThisChunk <= 0)
        return false;
    
    // Resize the buffer for this chunk
    chunks[chunkIndex]->data.setSize(numChannels, samplesThisChunk);
    
    // Read the data for this chunk
    reader->read(&(chunks[chunkIndex]->data), 0, samplesThisChunk, startSample, true, true);
    
    // Mark the chunk as loaded
    chunks[chunkIndex]->loaded.store(true);
    
    return true;
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
        
    // Prioritize this sound in the thread manager if available
    if (threadManager != nullptr)
        threadManager->prioritizeSound(this);
}
