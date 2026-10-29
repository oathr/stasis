#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace {

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
    using namespace stasis::params;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const Spec& s : all()) {
        const juce::ParameterID id{s.id, 1};
        switch (s.kind) {
            case Kind::Float: {
                auto text = s.text;
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    id, s.name, juce::NormalisableRange<float>(0.0f, 1.0f), s.defaultValue,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction([text](float v, int) {
                        return text ? juce::String(text(v)) : juce::String(v, 2);
                    })));
                break;
            }
            case Kind::Bool:
                layout.add(std::make_unique<juce::AudioParameterBool>(id, s.name, s.defaultValue > 0.5f));
                break;
            case Kind::Choice: {
                juce::StringArray choices;
                for (const auto& c : s.choices) choices.add(c);
                layout.add(std::make_unique<juce::AudioParameterChoice>(id, s.name, choices, int(s.defaultValue)));
                break;
            }
        }
    }
    return layout;
}

}  // namespace

StasisProcessor::StasisProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "STASIS", createLayout()) {
    for (const auto& s : stasis::params::all()) raw_.push_back(apvts.getRawParameterValue(s.id));
    values_.resize(raw_.size());
}

bool StasisProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    return out == juce::AudioChannelSet::stereo() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void StasisProcessor::prepareToPlay(double sampleRate, int) {
    engine.prepare(float(sampleRate));
    setLatencySamples(0);  // the dry is never delayed; only the wet carries latency
}

void StasisProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const auto start = juce::Time::getHighResolutionTicks();
    const int numSamples = buffer.getNumSamples();
    if (getTotalNumInputChannels() == 1) buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        if (m.isNoteOn()) engine.noteOn(m.getNoteNumber());
        else if (m.isNoteOff()) engine.noteOff(m.getNoteNumber());
    }

    float bpm = 120.0f;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto hostBpm = position->getBpm()) bpm = float(*hostBpm);

    for (size_t i = 0; i < raw_.size(); ++i) values_[i] = raw_[i]->load(std::memory_order_relaxed);
    engine.process(values_.data(), bpm, buffer.getWritePointer(0), buffer.getWritePointer(1), numSamples);

    const double elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    const double budget = numSamples / std::max(1.0, getSampleRate());
    const float load = float(elapsed / budget);
    cpuLoad.store(cpuLoad.load(std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* StasisProcessor::createEditor() { return new StasisEditor(*this); }

void StasisProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void StasisProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StasisProcessor(); }
