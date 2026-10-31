#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "VoiceCleaner.h"

class LimpaVozProcessor : public juce::AudioProcessor
{
public:
    LimpaVozProcessor();
    ~LimpaVozProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LimpaVoz"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::array<limpavoz::VoiceCleaner, 2> cleaners;
    juce::SmoothedValue<float> gainSmoothed;

    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* lowCutParam = nullptr;
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimpaVozProcessor)
};
