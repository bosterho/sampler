#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "StreamingSamplerSound.h"

class StreamingSamplerVoice : public juce::SynthesiserVoice
{
public:
    StreamingSamplerVoice() = default;
    
    // Properly clean up resources
    ~StreamingSamplerVoice() override
    {
        clearCurrentNote();
        sound = nullptr;
    }
    
    // Prevent copying to avoid double-ownership issues
    StreamingSamplerVoice(const StreamingSamplerVoice&) = delete;
    StreamingSamplerVoice& operator=(const StreamingSamplerVoice&) = delete;
    
    bool canPlaySound(juce::SynthesiserSound* s) override
    {
        return dynamic_cast<const StreamingSamplerSound*>(s) != nullptr;
    }
    
    void startNote(int midiNoteNumber,
                   float velocity,
                   juce::SynthesiserSound* s,
                   int currentPitchWheelPosition) override
    {
        if (auto* soundPtr = dynamic_cast<StreamingSamplerSound*>(s))
        {
            pitchRatio = std::pow(2.0, (midiNoteNumber - soundPtr->midiRootNote) / 12.0)
                        * soundPtr->sourceSampleRate / getSampleRate();
            
            sourceSamplePosition = 0.0;
            lgain = velocity;
            rgain = velocity;
            
            adsr.setSampleRate(getSampleRate());
            adsr.setParameters(soundPtr->params);
            adsr.noteOn();
            
            // Store sound reference safely
            sound = soundPtr;
            playing = true;
        }
        else
        {
            playing = false;
        }
    }
    
    void stopNote(float velocity, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            clearCurrentNote();
            playing = false;
        }
    }
    
    void pitchWheelMoved(int newValue) override {}
    
    void controllerMoved(int controllerNumber, int newValue) override {}
    
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         int startSample,
                         int numSamples) override
    {
        if (!playing || sound == nullptr)
            return;
            
        // Use a weak reference check approach
        auto* streamingSound = sound;
        
        if (streamingSound == nullptr)
        {
            clearCurrentNote();
            playing = false;
            return;
        }
        
        // Cache sound properties at the start of rendering to reduce potential threading issues
        const int numChannels = outputBuffer.getNumChannels();
        const int soundNumChannels = streamingSound->getNumChannels();
        const int soundLengthSamples = streamingSound->getLengthInSamples();
        
        // Early return if the sound is invalid
        if (soundNumChannels <= 0 || soundLengthSamples <= 0)
        {
            clearCurrentNote();
            playing = false;
            return;
        }
        
        for (int i = 0; i < numSamples; ++i)
        {
            const int pos = startSample + i;
            
            // Check if we've reached the end of the sample
            if (sourceSamplePosition >= soundLengthSamples)
            {
                clearCurrentNote();
                playing = false;
                break;
            }
            
            // Get the current interpolation values
            const int samplePos = static_cast<int>(sourceSamplePosition);
            const float alpha = static_cast<float>(sourceSamplePosition - samplePos);
            
            // Make sure we don't go out of bounds
            const int samplePos2 = std::min(samplePos + 1, soundLengthSamples - 1);
            
            // Get the envelope value
            const float envAmp = adsr.getNextSample();
            
            // Get and process samples with interpolation for each channel
            for (int channel = 0; channel < numChannels; ++channel)
            {
                // Get the sample channel, wrapping if needed
                const int soundChannel = channel % soundNumChannels;
                
                try {
                    // Get the samples for interpolation with bounds checking
                    const float s1 = streamingSound->getSample(soundChannel, samplePos);
                    const float s2 = streamingSound->getSample(soundChannel, samplePos2);
                    
                    // Linear interpolation
                    const float interp = s1 + alpha * (s2 - s1);
                    
                    // Apply envelope and add to output
                    if (channel == 0)
                        outputBuffer.addSample(channel, pos, interp * lgain * envAmp);
                    else
                        outputBuffer.addSample(channel, pos, interp * rgain * envAmp);
                }
                catch (const std::exception&)
                {
                    // If we get any exception (like out of bounds), stop playback safely
                    clearCurrentNote();
                    playing = false;
                    return;
                }
            }
            
            // Increment sample position
            sourceSamplePosition += pitchRatio;
        }
        
        // Check if the envelope has finished
        if (!adsr.isActive())
        {
            clearCurrentNote();
            playing = false;
        }
    }
    
    // Add explicit cleanup method
    void clearSound()
    {
        clearCurrentNote();
        sound = nullptr;
        playing = false;
    }
    
private:
    // Use RAII where possible with strong ownership semantics
    double sourceSamplePosition = 0.0;
    double pitchRatio = 1.0;
    float lgain = 0.0f, rgain = 0.0f;
    bool playing = false;
    
    // This is a non-owning pointer - the Synthesiser owns the sounds
    StreamingSamplerSound* sound = nullptr;
    
    juce::ADSR adsr;
};