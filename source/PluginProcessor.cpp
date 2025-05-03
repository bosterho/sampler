#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PluginProcessor::PluginProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
{
    formatManager.registerBasicFormats();
    
    // Initialize the sampler with voices
    for (int i = 0; i < voiceCount; ++i)
        sampler.addVoice(new juce::SamplerVoice());
        
    // Try to load the default sample
    loadDefaultSample();
}

PluginProcessor::~PluginProcessor()
{
}

//==============================================================================
const juce::String PluginProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PluginProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PluginProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int PluginProcessor::getCurrentProgram()
{
    return 0;
}

void PluginProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String PluginProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void PluginProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampler.setCurrentPlaybackSampleRate(sampleRate);
}

void PluginProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    // juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, clear any output
    // channels that didn't contain input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Check if we have MIDI input and should start loading the full sample
    if (!midiMessages.isEmpty() && !sampleTriggered.load())
    {
        auto* sound = getCurrentStreamingSound();
        if (sound != nullptr && !sound->isFullyLoaded())
        {
            sound->startBackgroundLoading();
            sampleTriggered.store(true);
        }
    }

    // Process the MIDI and generate audio
    sampler.renderNextBlock(buffer, midiMessages, 0, buffer.getNumSamples());
}

//==============================================================================
bool PluginProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (*this);
}

//==============================================================================
void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state("SamplerPluginState");
    state.setProperty("filePath", currentlyLoadedFilePath, nullptr);
    
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    
    if (xmlState != nullptr)
    {
        juce::ValueTree state = juce::ValueTree::fromXml(*xmlState);
        juce::String savedFilePath = state.getProperty("filePath", "");
        
        if (savedFilePath.isNotEmpty())
        {
            juce::File file(savedFilePath);
            if (file.existsAsFile())
            {
                loadFile(file);
            }
            else
            {
                // If the saved file doesn't exist, try to load the default sample
                loadDefaultSample();
            }
        }
        else
        {
            loadDefaultSample();
        }
    }
    else
    {
        loadDefaultSample();
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}

void PluginProcessor::loadFile(const juce::String& path)
{
    loadFile(juce::File(path));
}

void PluginProcessor::loadFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return;

    // Clear existing sounds
    sampler.clearSounds();
    
    // Create a reader for the file
    auto* reader = formatManager.createReaderFor(file);
    
    if (reader != nullptr)
    {
        // Reset the triggered flag when loading a new sample
        sampleTriggered.store(false);
        
        // Get the length of the audio file
        auto sampleLength = static_cast<int>(reader->lengthInSamples);
        
        // Create a setup for all MIDI notes
        juce::BigInteger allNotes;
        allNotes.setRange(0, 128, true);
        
        // Create a streaming sound
        auto sound = new StreamingSamplerSound(file.getFileName(),
                                               *reader,
                                               allNotes,
                                               60,   // Root note (Middle C)
                                               0.1,  // Attack time
                                               0.1,  // Release time
                                               10.0); // Maximum sample length
        
        sampler.addSound(sound);
        currentlyLoadedFilePath = file.getFullPathName();
        
        delete reader;
    }
}

// Added method to get the current streaming sampler sound
StreamingSamplerSound* PluginProcessor::getCurrentStreamingSound()
{
    if (sampler.getNumSounds() > 0)
    {
        auto* sound = dynamic_cast<StreamingSamplerSound*>(sampler.getSound(0).get());
        return sound;
    }
    return nullptr;
}

// Added method to check if the sample is fully loaded
bool PluginProcessor::isSampleFullyLoaded() const
{
    if (sampler.getNumSounds() > 0)
    {
        auto* sound = dynamic_cast<StreamingSamplerSound*>(sampler.getSound(0).get());
        if (sound != nullptr)
            return sound->isFullyLoaded();
    }
    return false;
}

void PluginProcessor::loadDefaultSample()
{
    auto sampleFile = getDefaultSampleFile();
    
    if (sampleFile.existsAsFile())
    {
        loadFile(sampleFile);
    }
    else
    {
        // If the default sample doesn't exist, check if there's any sample in the directory
        auto directory = getDefaultSampleDirectory();
        
        if (directory.isDirectory())
        {
            // Look for any supported audio file
            for (const auto& entry : juce::RangedDirectoryIterator(directory, false, "*.wav;*.aif;*.aiff"))
            {
                auto file = entry.getFile();
                if (file.existsAsFile())
                {
                    loadFile(file);
                    break;
                }
            }
        }
    }
}

juce::File PluginProcessor::getDefaultSampleDirectory() const
{
    // Common locations for application data
    #if JUCE_MAC
        // On macOS, a good place is in the Application Support directory
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("Osterhouse Sounds")
                .getChildFile("Sampler")
                .getChildFile("Samples");
    #elif JUCE_WINDOWS
        // On Windows, use the AppData directory
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("Osterhouse Sounds")
                .getChildFile("Sampler")
                .getChildFile("Samples");
    #else
        // Linux or other platforms
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile(".osterhousesounds")
                .getChildFile("sampler")
                .getChildFile("samples");
    #endif
}

juce::File PluginProcessor::getDefaultSampleFile() const
{
    // Return the path to the default sample file
    return getDefaultSampleDirectory().getChildFile("default_sample.wav");
}
