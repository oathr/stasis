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

    // Factory presets (Presets.h) are the host's programs too.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentPreset_; }
    void setCurrentProgram(int index) override { applyPreset(index); }
    const juce::String getProgramName(int index) override;
    void applyPreset(int index);
    std::function<void()> onPresetChanged;  // message thread, set by the editor
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Module order, as names from stasis::params::kModules. setModuleOrder ignores
    // anything that is not a full permutation.
    juce::StringArray getModuleOrder() const;
    void setModuleOrder(const juce::StringArray& names);
    std::function<void()> onOrderChanged;  // message thread, set by the editor

    juce::AudioProcessorValueTreeState apvts;
    stasis::Engine engine;
    std::atomic<float> cpuLoad{0.0f};  // share of the block duration spent processing

private:
    std::vector<std::atomic<float>*> raw_;
    std::vector<float> values_;
    int currentPreset_ = 0;
    std::atomic<bool> resetPending_{false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(StasisProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StasisProcessor)
};
