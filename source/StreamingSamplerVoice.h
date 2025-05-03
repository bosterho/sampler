#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "StreamingSamplerSound.h"

class StreamingSamplerVoice : public juce::SamplerVoice
{
public:
    StreamingSamplerVoice() {}
    
    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<const StreamingSamplerSound*>(sound) != nullptr;
    }
    
    void startNote(int midiNoteNumber,
                   float velocity,
                   juce::SynthesiserSound* s,
                   int currentPitchWheelPosition) override
    {
        if (auto* sound = dynamic_cast<StreamingSamplerSound*>(s))
        {
            pitchRatio = std::pow(2.0, (midiNoteNumber - sound->midiRootNote) / 12.0)
                        * sound->sourceSampleRate / getSampleRate();
            
            sourceSamplePosition = 0.0;
            lgain = velocity;
            rgain = velocity;
            
            adsr.setSampleRate(getSampleRate());
            adsr.setParameters(sound->params);
            adsr.noteOn();
            
            this->sound = sound;
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
        if (!playing)
            return;
            
        auto* streamingSound = static_cast<StreamingSamplerSound*>(sound);
        
        if (streamingSound == nullptr)
        {
            playing = false;
            return;
        }
        
        const int numChannels = outputBuffer.getNumChannels();
        const int soundNumChannels = streamingSound->getNumChannels();
        const int soundLengthSamples = streamingSound->getLengthInSamples();
        
        for (int i = 0; i < numSamples; ++i)
        {
            const int pos = startSample + i;
            
            // Check if we've reached the end of the sample
            if (sourceSamplePosition >= soundLengthSamples)
            {
                playing = false;
                break;
            }
            
            // Get the current interpolation values
            int samplePos = (int)sourceSamplePosition;
            float alpha = (float)(sourceSamplePosition - samplePos);
            
            // Make sure we don't go out of bounds
            int samplePos2 = samplePos + 1;
            if (samplePos2 >= soundLengthSamples)
                samplePos2 = soundLengthSamples - 1;
            
            // Get the envelope value
            float envAmp = adsr.getNextSample();
            
            // Get and process samples with interpolation for each channel
            for (int channel = 0; channel < numChannels; ++channel)
            {
                // Get the sample channel, wrapping if needed
                int soundChannel = channel % soundNumChannels;
                
                // Get the samples for interpolation
                float s1 = streamingSound->getSample(soundChannel, samplePos);
                float s2 = streamingSound->getSample(soundChannel, samplePos2);
                
                // Linear interpolation
                float interp = s1 + alpha * (s2 - s1);
                
                // Apply envelope and add to output
                if (channel == 0)
                    outputBuffer.addSample(channel, pos, interp * lgain * envAmp);
                else
                    outputBuffer.addSample(channel, pos, interp * rgain * envAmp);
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
    
private:
    double sourceSamplePosition = 0.0;
    double pitchRatio = 1.0;
    float lgain = 0.0f, rgain = 0.0f;
    bool playing = false;
    StreamingSamplerSound* sound = nullptr;
    
    juce::ADSR adsr;
};