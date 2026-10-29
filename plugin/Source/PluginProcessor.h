#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine.h"

class StasisProcessor : public juce::AudioProcessor {
public:
    StasisProcessor();

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 30.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    stasis::Engine engine;
    std::atomic<float> cpuLoad{0.0f};  // share of the block duration spent processing

private:
    std::vector<std::atomic<float>*> raw_;
    std::vector<float> values_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StasisProcessor)
};
