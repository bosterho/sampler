#include "StreamingSamplerSound.h"

StreamingSamplerSound::StreamingSamplerSound(const juce::String& soundName,
                                           juce::AudioFormatReader& source,
                                           const juce::BigInteger& notes,
                                           int midiNoteForNormalPitch,
                                           double attackTimeSecs,
                                           double releaseTimeSecs,
                                           double maxSampleLengthSeconds)
    : juce::SamplerSound(soundName, source, notes, midiNoteForNormalPitch, 
                        attackTimeSecs, releaseTimeSecs, maxSampleLengthSeconds),
      name(soundName),
      midiNotes(notes),
      midiRootNote(midiNoteForNormalPitch),
      attackTime(attackTimeSecs),
      releaseTime(releaseTimeSecs)
{
    // Store the source format reader details to recreate it later
    if (juce::File::isAbsolutePath(source.getFile()->getFullPathName()))
        filePath = source.getFile()->getFullPathName();
    
    // Calculate how many samples to load initially (60KB worth)
    int initialSamples = bytesToSamples(INITIAL_LOAD_SIZE, 
                                        source.numChannels, 
                                        source.bitsPerSample);
    
    // Ensure we don't try to load more than the actual file length
    initialSamples = juce::jmin(initialSamples, static_cast<int>(source.lengthInSamples));
    
    // Create a buffer with the full size but only load the initial part
    data.setSize(source.numChannels, static_cast<int>(source.lengthInSamples));
    source.read(&data, 0, initialSamples, 0, true, true);
    
    // Clear the rest of the buffer (will be loaded later)
    for (int channel = 0; channel < data.getNumChannels(); ++channel)
        data.clear(channel, initialSamples, data.getNumSamples() - initialSamples);
}

StreamingSamplerSound::~StreamingSamplerSound()
{
}

bool StreamingSamplerSound::appliesToNote(int midiNoteNumber)
{
    return midiNotes[midiNoteNumber];
}

bool StreamingSamplerSound::appliesToChannel(int /*midiChannel*/)
{
    return true;
}

void StreamingSamplerSound::startBackgroundLoading()
{
    // If already fully loaded or currently loading, don't start again
    if (fullyLoaded.load() || loading.load() || filePath.isEmpty())
        return;
    
    // Create a new thread to load the complete file
    loading.store(true);
    
    // In a real implementation, we'd use a ThreadPool to manage this
    // For simplicity, we're creating a new thread each time
    juce::Thread::launch([this]() {
        loadCompleteFile();
    });
}

void StreamingSamplerSound::loadCompleteFile()
{
    juce::File file(filePath);
    
    if (!file.existsAsFile())
    {
        loading.store(false);
        return;
    }
    
    // Create a format manager to read the file
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    // Create a reader for the file
    auto reader = formatManager.createReaderFor(file);
    
    if (reader != nullptr)
    {
        // Read the entire file into our buffer
        reader->read(&data, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
        delete reader;
        
        fullyLoaded.store(true);
    }
    
    loading.store(false);
}
