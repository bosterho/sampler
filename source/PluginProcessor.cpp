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
    
    // Initialize the sampler with our custom streaming voices
    for (int i = 0; i < voiceCount; ++i)
        sampler.addVoice(new StreamingSamplerVoice());
    
    // Initialize sample playback parameters
    addParameter(attackParam = new juce::AudioParameterFloat("attack", "Attack", 0.0f, 5.0f, 0.1f));
    addParameter(releaseParam = new juce::AudioParameterFloat("release", "Release", 0.0f, 5.0f, 0.1f));
    addParameter(rootNoteParam = new juce::AudioParameterFloat("rootNote", "Root Note", 0.0f, 127.0f, 60.0f));
    addParameter(oneShot = new juce::AudioParameterBool("oneShot", "One Shot", false));
    
    // Initialize arpeggiator parameters
    addParameter(arpEnabled = new juce::AudioParameterBool("arpEnabled", "Arpeggiator Enabled", true));
    addParameter(arpSpeed = new juce::AudioParameterFloat("arpSpeed", "Arpeggiator Speed", 0.0f, 1.0f, 0.5f));
    
    juce::StringArray patternNames = { "Up", "Down", "Up-Down", "Random", "As Played" };
    addParameter(arpPattern = new juce::AudioParameterChoice("arpPattern", "Arpeggiator Pattern", patternNames, 0));
    
    // Load samples from user's documents folder instead of default sample
    loadAllSamplesFromUserFolder();
    
    // If no samples were found in the user folder, try to load the default sample
    if (sampler.getNumSounds() == 0)
    {
        loadDefaultSample();
    }
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
    
    // Initialize arpeggiator
    arpNotes.clear();
    arpCurrentNote = 0;
    arpLastNoteValue = -1;
    arpTime = 0;
    arpRate = static_cast<float>(sampleRate);
    arpeggiatorIsActive = *arpEnabled;
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

    // Process MIDI through the arpeggiator if enabled and not disabled for debugging
    if (*arpEnabled)
    {
        // Process the arpeggiator on the MIDI messages
        processArpeggiator(buffer, midiMessages);
    }

    // Process MIDI events for velocity-based sample selection
    juce::MidiBuffer processedMidi;
    
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        
        // Check if this is a note-on message
        if (message.isNoteOn())
        {
            int noteNumber = message.getNoteNumber();
            int velocity = message.getVelocity();
            
            DBG("MIDI Note ON: Note=" + juce::String(noteNumber) + 
                " Velocity=" + juce::String(velocity) +
                (*arpEnabled ? " (Arpeggiator active)" : ""));
            
            // Check which sample would be triggered for this note and velocity
            bool foundMatchingSample = false;
            for (auto* info : sampleInfos)
            {
                if (info->midiNote == noteNumber && 
                    info->velocityRange.contains(velocity))
                {
                    // DBG("  Matching sample: " + info->file.getFileName() + 
                        // " (Note: " + juce::String(info->midiNote) + 
                        // ", Velocity range: " + juce::String(info->velocityRange.getStart()) + 
                        // "-" + juce::String(info->velocityRange.getEnd()) + ")");
                    foundMatchingSample = true;
                }
            }
            
            // if (!foundMatchingSample)
            // {
                // DBG("  No matching sample found for this note/velocity combination");
            // }
            
            // Check if we should start loading full samples in the background
            if (!sampleTriggered.load() && !disableDiskStreaming)
            {
                for (auto* sound : sampleInfos)
                {
                    if (sound->midiNote == noteNumber && 
                        sound->velocityRange.contains(velocity))
                    {
                        auto* streamingSound = dynamic_cast<StreamingSamplerSound*>(
                            sampler.getSound(sampleInfos.indexOf(sound)).get());
                        
                        if (streamingSound != nullptr && !streamingSound->isFullyLoaded())
                        {
                            streamingSound->startBackgroundLoading();
                            sampleTriggered.store(true);
                            // DBG("  Started background loading for: " + sound->file.getFileName());
                            break;
                        }
                    }
                }
            }
            
            // Add the message to our processed MIDI buffer
            processedMidi.addEvent(message, metadata.samplePosition);
        }
        else if (message.isNoteOff())
        {
            int noteNumber = message.getNoteNumber();
            // DBG("MIDI Note OFF: Note=" + juce::String(noteNumber));
            processedMidi.addEvent(message, metadata.samplePosition);
        }
        else
        {
            // For all other MIDI messages, pass them through unchanged
            processedMidi.addEvent(message, metadata.samplePosition);
        }
    }

    // Print streaming status for all samples periodically (every 30 blocks)
    static int blockCounter = 0;
    if (++blockCounter >= 30)
    {
        blockCounter = 0;
        
        // Output loading status of samples
        if (sampler.getNumSounds() > 0)
        {
            // DBG("Sample loading status:");
            for (int i = 0; i < sampler.getNumSounds(); ++i)
            {
                auto* streamingSound = dynamic_cast<StreamingSamplerSound*>(sampler.getSound(i).get());
                if (streamingSound != nullptr)
                {
                    // DBG("  Sample " + juce::String(i+1) + ": " + 
                        // (streamingSound->isFullyLoaded() ? "FULLY LOADED" : 
                        //  (streamingSound->isLoading() ? "LOADING..." : "PARTIALLY LOADED")));
                }
            }
        }
    }

    // Process the MIDI through the sampler to generate audio
    sampler.renderNextBlock(buffer, processedMidi, 0, buffer.getNumSamples());
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
                                               10.0, // Maximum sample length
                                               file.getFullPathName()); // Pass the file path
        
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

juce::File PluginProcessor::getUserSamplesFolder() const
{
    // Return the path to the user's documents folder with the specified subfolders
    #if JUCE_WINDOWS
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("Osterhouse Sounds")
                .getChildFile("sampler")
                .getChildFile("samples");
    #else
        // On Mac or other platforms, adjust accordingly
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("Osterhouse Sounds")
                .getChildFile("sampler")
                .getChildFile("samples");
    #endif
}

void PluginProcessor::loadAllSamplesFromUserFolder()
{
    // Clear existing sounds and sample information
    sampler.clearSounds();
    sampleInfos.clear();
    
    // Get the samples folder
    juce::File samplesFolder = getUserSamplesFolder();
    
    // Check if the directory exists
    if (!samplesFolder.isDirectory())
    {
        // Create the directory if it doesn't exist
        samplesFolder.createDirectory();
        return; // No samples to load yet
    }
    
    // Look for all supported audio files
    juce::Array<juce::File> sampleFiles;
    int numFilesFound = samplesFolder.findChildFiles(sampleFiles, 
                                                     juce::File::findFiles, 
                                                     true, // search recursively
                                                     "*.wav;*.aif;*.aiff;*.mp3");
    
    // If no samples found, return
    if (numFilesFound == 0)
        return;
    
    // Process each found file
    for (auto& file : sampleFiles)
    {
        SampleInfo* info = new SampleInfo();
        parseSampleFilename(file, *info);
        sampleInfos.add(info);
        addSampleToSampler(*info);
    }
    
    // Log how many samples were loaded
    juce::Logger::writeToLog("Loaded " + juce::String(sampleInfos.size()) + " samples");
}

void PluginProcessor::parseSampleFilename(const juce::File& file, SampleInfo& info)
{
    // Store the file
    info.file = file;
    
    // Get the filename without extension
    juce::String filename = file.getFileNameWithoutExtension();
    
    // Split the filename by underscore
    juce::StringArray parts;
    parts.addTokens(filename, "_", "");
    
    // Default values
    info.midiNote = 60; // Default to middle C
    info.velocityRange = juce::Range<int>(0, 127); // Full velocity range
    info.roundRobin = ""; // No round robin
    
    // Parse the filename parts
    if (parts.size() >= 1)
    {
        // First part is the sample name
        info.name = parts[0];
    }
    
    if (parts.size() >= 2)
    {
        // Second part is typically the sample type (e.g., Damp)
        info.type = parts[1];
    }
    
    // Look for velocity range
    if (parts.size() >= 4)
    {
        // Format: ..._minVel_maxVel_midiNote_...
        try {
            int minVel = parts[2].getIntValue();
            int maxVel = parts[3].getIntValue();
            
            // Clamp to valid MIDI velocity range (0-127)
            minVel = juce::jlimit(0, 127, minVel);
            maxVel = juce::jlimit(0, 127, maxVel);
            
            info.velocityRange = juce::Range<int>(minVel, maxVel);
            
            // If there's a fifth part, it's the MIDI note
            if (parts.size() >= 5)
            {
                info.midiNote = parts[4].getIntValue();
            }
            
            // Check for round robin (RR) information
            if (parts.size() >= 6 && parts[5].startsWith("RR"))
            {
                info.roundRobin = parts[5];
            }
        }
        catch (...) {
            // If parsing fails, use defaults
            info.velocityRange = juce::Range<int>(0, 127);
            info.midiNote = 60;
        }
    }
}

void PluginProcessor::addSampleToSampler(const SampleInfo& info)
{
    // Create a reader for the file
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(info.file));
    
    if (reader != nullptr)
    {
        // Create a BigInteger representing the MIDI note this sample applies to
        juce::BigInteger midiNotes;
        midiNotes.setRange(0, 128, false); // Clear all notes
        midiNotes.setBit(info.midiNote, true); // Set just this one note
        
        // Create a streaming sound for this sample
        auto sound = new StreamingSamplerSound(
            info.file.getFileNameWithoutExtension(),
            *reader,
            midiNotes,
            info.midiNote,   // Root note is the midi note from the filename
            0.01,  // Default attack time
            0.1,   // Default release time
            10.0,  // Maximum sample length in seconds
            info.file.getFullPathName()); // Pass the file path
        
        // Add the sound to the sampler
        sampler.addSound(sound);
        
        // Store the loaded file path
        currentlyLoadedFilePath = info.file.getFullPathName();
    }
}

void PluginProcessor::processArpeggiator(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Check if arpeggiator is enabled
    arpeggiatorIsActive = *arpEnabled;
    if (!arpeggiatorIsActive)
        return;

    // Clear the arpeggiator MIDI buffer
    arpMidiBuffer.clear();
    
    // Process incoming MIDI messages to update the arp note list
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();
        
        // Add notes to our arpeggiator list on note-on, remove on note-off
        if (msg.isNoteOn())
        {
            arpNotes.add(msg.getNoteNumber());
            // DBG("Arp: Added note " + juce::String(msg.getNoteNumber()) + " to arp list");
        }
        else if (msg.isNoteOff())
        {
            arpNotes.removeValue(msg.getNoteNumber());
            // DBG("Arp: Removed note " + juce::String(msg.getNoteNumber()) + " from arp list");
        }
        else
        {
            // Pass through other MIDI messages (controllers, etc.)
            arpMidiBuffer.addEvent(msg, metadata.samplePosition);
        }
    }
    
    // Calculate note duration based on arpeggiator speed
    auto noteDuration = static_cast<int>(std::ceil(arpRate * 0.25f * (0.1f + (1.0f - (*arpSpeed)))));
    
    // If we've reached the end of current note duration or don't have a note playing
    if ((arpTime + buffer.getNumSamples()) >= noteDuration || arpLastNoteValue < 0)
    {
        // Calculate the offset for the new notes
        auto offset = juce::jmax(0, juce::jmin((int)(noteDuration - arpTime), buffer.getNumSamples() - 1));
        
        // Turn off the previous note if there was one
        if (arpLastNoteValue > 0)
        {
            arpMidiBuffer.addEvent(juce::MidiMessage::noteOff(1, arpLastNoteValue), offset);
            // DBG("Arp: Note OFF - " + juce::String(arpLastNoteValue) + " at offset " + juce::String(offset));
            arpLastNoteValue = -1;
        }
        
        // Only trigger a new note if we have notes in our list
        if (arpNotes.size() > 0)
        {
            // Select the next note according to the selected pattern
            int patternType = arpPattern->getIndex();
            
            switch (patternType)
            {
                case ArpPatterns::Up:
                    // Go up through the notes
                    arpCurrentNote = (arpCurrentNote + 1) % arpNotes.size();
                    break;
                    
                case ArpPatterns::Down:
                    // Go down through the notes
                    arpCurrentNote = (arpCurrentNote - 1 + arpNotes.size()) % arpNotes.size();
                    break;
                    
                case ArpPatterns::UpDown:
                {
                    // Go up then down
                    static bool goingUp = true;
                    
                    if (goingUp)
                    {
                        arpCurrentNote++;
                        if (arpCurrentNote >= arpNotes.size() - 1)
                            goingUp = false;
                    }
                    else
                    {
                        arpCurrentNote--;
                        if (arpCurrentNote <= 0)
                            goingUp = true;
                    }
                    
                    arpCurrentNote = juce::jlimit(0, arpNotes.size() - 1, arpCurrentNote);
                    break;
                }
                
                case ArpPatterns::Random:
                    // Random note selection
                    arpCurrentNote = juce::Random::getSystemRandom().nextInt(arpNotes.size());
                    break;
                    
                case ArpPatterns::AsPlayed:
                default:
                    // Cycle through notes in the order they were played
                    arpCurrentNote = (arpCurrentNote + 1) % arpNotes.size();
                    break;
            }
            
            // Trigger the selected note
            arpLastNoteValue = arpNotes[arpCurrentNote];
            arpMidiBuffer.addEvent(juce::MidiMessage::noteOn(1, arpLastNoteValue, (juce::uint8)100), offset);
            // DBG("Arp: Note ON - " + juce::String(arpLastNoteValue) + " at offset " + juce::String(offset) + 
                // " (pattern: " + arpPattern->getCurrentChoiceName() + ")");
        }
        
        // Reset time if we've gone past the duration
        if ((arpTime + buffer.getNumSamples()) >= noteDuration)
            arpTime = (arpTime + buffer.getNumSamples()) % noteDuration;
    }
    else
    {
        // Update time counter
        arpTime += buffer.getNumSamples();
    }
    
    // Replace the input MIDI with our arpeggiator MIDI
    midiMessages.swapWith(arpMidiBuffer);
}
