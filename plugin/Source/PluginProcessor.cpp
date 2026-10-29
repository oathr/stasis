#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Presets.h"

namespace {

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
    using namespace stasis::params;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const Spec& s : all()) {
        const juce::ParameterID id{s.id.c_str(), 1};
        switch (s.kind) {
            case Kind::Float: {
                auto text = s.text;
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    id, juce::String(s.name), juce::NormalisableRange<float>(0.0f, 1.0f), s.defaultValue,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction([text](float v, int) {
                        return text ? juce::String(text(v)) : juce::String(v, 2);
                    })));
                break;
            }
            case Kind::Bool:
                layout.add(std::make_unique<juce::AudioParameterBool>(id, juce::String(s.name), s.defaultValue > 0.5f));
                break;
            case Kind::Choice: {
                juce::StringArray choices;
                for (const auto& c : s.choices) choices.add(c);
                layout.add(std::make_unique<juce::AudioParameterChoice>(id, juce::String(s.name), choices, int(s.defaultValue)));
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
    for (const auto& s : stasis::params::all()) raw_.push_back(apvts.getRawParameterValue(s.id.c_str()));
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

    // The safety guard saw NaN/Inf: rebuild the engine off the audio thread.
    if (engine.needsReset() && !resetPending_.exchange(true))
        juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
            if (safe == nullptr) return;
            safe->suspendProcessing(true);
            safe->engine.prepare(float(safe->getSampleRate()));
            safe->suspendProcessing(false);
            safe->resetPending_.store(false);
        });

    const double elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    const double budget = numSamples / std::max(1.0, getSampleRate());
    const float load = float(elapsed / budget);
    cpuLoad.store(cpuLoad.load(std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* StasisProcessor::createEditor() { return new StasisEditor(*this); }

int StasisProcessor::getNumPrograms() { return int(stasis::presets::all().size()); }

const juce::String StasisProcessor::getProgramName(int index) {
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return {};
    return juce::String(list[size_t(index)].category) + " / " + list[size_t(index)].name;
}

// Every parameter goes back to its default, then the preset's own values apply.
void StasisProcessor::applyPreset(int index) {
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return;
    const auto& preset = list[size_t(index)];
    for (const auto& spec : stasis::params::all()) {
        float value = spec.defaultValue;
        for (const auto& [id, v] : preset.values)
            if (spec.id == id) value = v;
        if (auto* param = apvts.getParameter(spec.id.c_str())) param->setValueNotifyingHost(param->convertTo0to1(value));
    }
    const juce::String order(preset.order);
    setModuleOrder(juce::StringArray::fromTokens(order.isEmpty() ? juce::String("shape,derez,pitch,stretch,echo,space,focus,air,color") : order, ",", ""));
    currentPreset_ = index;
    juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
        if (safe == nullptr) return;
        if (safe->onOrderChanged) safe->onOrderChanged();
        if (safe->onPresetChanged) safe->onPresetChanged();
    });
}

juce::StringArray StasisProcessor::getModuleOrder() const {
    const uint64_t order = engine.order();
    juce::StringArray names;
    for (size_t slot = 0; slot < stasis::params::kModules.size(); ++slot)
        names.add(stasis::params::kModules[size_t((order >> (4 * slot)) & 15)]);
    return names;
}

void StasisProcessor::setModuleOrder(const juce::StringArray& names) {
    const auto& modules = stasis::params::kModules;
    if (names.size() != int(modules.size())) return;
    uint64_t order = 0, seen = 0;
    for (int slot = 0; slot < names.size(); ++slot) {
        const auto it = std::find_if(modules.begin(), modules.end(), [&](const char* m) { return names[slot] == m; });
        if (it == modules.end()) return;
        const uint64_t index = uint64_t(it - modules.begin());
        seen |= 1ull << index;
        order |= index << (4 * slot);
    }
    if (seen != (1ull << modules.size()) - 1) return;
    engine.setOrder(order);
}

void StasisProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    state.setProperty("order", getModuleOrder().joinIntoString(","), nullptr);
    state.setProperty("preset", currentPreset_, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void StasisProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (!xml->hasTagName(apvts.state.getType())) return;
        const auto state = juce::ValueTree::fromXml(*xml);
        apvts.replaceState(state);
        setModuleOrder(juce::StringArray::fromTokens(state.getProperty("order").toString(), ",", ""));
        currentPreset_ = int(state.getProperty("preset", 0));
        juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
            if (safe == nullptr) return;
            if (safe->onOrderChanged) safe->onOrderChanged();
            if (safe->onPresetChanged) safe->onPresetChanged();
        });
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StasisProcessor(); }
