#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#if (MSVC)
#include "ipps.h"
#endif

class PluginProcessor : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void loadFile(const juce::String& path);
    void loadFile(const juce::File& file);
    juce::AudioFormatManager& getFormatManager() { return formatManager; }
    juce::Synthesiser& getSampler() { return sampler; }
    juce::String getLoadedFilePath() const { return currentlyLoadedFilePath; }
    
    juce::File getDefaultSampleDirectory() const;
    juce::File getDefaultSampleFile() const;
    void loadDefaultSample();
    
private:
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::AudioFormatReader* formatReader { nullptr };
    juce::String currentlyLoadedFilePath;
    int voiceCount { 128 };  // Maximum number of voices
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
