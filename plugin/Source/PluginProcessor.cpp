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
    double ppq = 0.0;
    bool playing = false;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition()) {
            if (const auto hostBpm = position->getBpm()) bpm = float(*hostBpm);
            if (const auto hostPpq = position->getPpqPosition()) ppq = *hostPpq;
            playing = position->getIsPlaying();
        }

    for (size_t i = 0; i < raw_.size(); ++i) values_[i] = raw_[i]->load(std::memory_order_relaxed);
    engine.process(values_.data(), bpm, ppq, playing, buffer.getWritePointer(0), buffer.getWritePointer(1), numSamples);

    // The safety guard saw NaN/Inf: rebuild the engine off the audio thread.
    if (engine.needsReset() && !resetPending_.exchange(true))
        juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
            if (safe == nullptr) return;
            safe->resetEngine();
            safe->resetPending_.store(false);
        });

    const double elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    const double budget = numSamples / std::max(1.0, getSampleRate());
    const float load = float(elapsed / budget);
    cpuLoad.store(cpuLoad.load(std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* StasisProcessor::createEditor() { return new StasisEditor(*this); }

void StasisProcessor::resetEngine() {
    suspendProcessing(true);  // waits for the audio callback, then keeps it out
    engine.prepare(float(getSampleRate()));
    suspendProcessing(false);
}

void StasisProcessor::panic() {
    if (auto* freeze = apvts.getParameter("freeze")) freeze->setValueNotifyingHost(0.0f);
    resetEngine();
}

juce::StringArray StasisProcessor::getLaneTargets() const {
    juce::StringArray ids;
    const auto& specs = stasis::params::all();
    for (int lane = 0; lane < 12; ++lane) {
        const int index = engine.laneTargetIndex(lane);
        ids.add(index >= 0 ? juce::String(specs[size_t(index)].id) : juce::String());
    }
    return ids;
}

void StasisProcessor::setLaneTarget(int lane, const juce::String& id) {
    if (lane < 0 || lane >= 12) return;
    if (id.isEmpty()) return engine.setLaneTarget(lane, -1);
    const int index = stasis::params::indexOf(id.toStdString());
    if (index < 0) return;
    const auto& spec = stasis::params::all()[size_t(index)];
    if (spec.kind != stasis::params::Kind::Float || stasis::params::isMotionId(spec.id)) return;
    engine.setLaneTarget(lane, index);
}

int StasisProcessor::getNumPrograms() { return int(stasis::presets::all().size()); }

const juce::String StasisProcessor::getProgramName(int index) {
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return {};
    return juce::String(list[size_t(index)].category) + " / " + list[size_t(index)].name;
}

// Every parameter goes back to its default, then the preset's own values apply. MOTION
// lanes 1, 2, 3… take the preset's modulations; the others go back to their defaults.
void StasisProcessor::applyPreset(int index) {
    using stasis::params::laneId;
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return;
    const auto& preset = list[size_t(index)];
    std::vector<std::pair<std::string, float>> values(preset.values.begin(), preset.values.end());
    for (size_t lane = 0; lane < preset.motion.size() && lane < 12; ++lane) {
        const auto& mod = preset.motion[lane];
        values.push_back({laneId(int(lane), "rate"), stasis::params::laneRateT(mod.hz)});
        values.push_back({laneId(int(lane), "shape"), float(mod.shape)});
        values.push_back({laneId(int(lane), "depth"), mod.depth});
    }
    for (const auto& spec : stasis::params::all()) {
        float value = spec.defaultValue;
        for (const auto& [id, v] : values)
            if (spec.id == id) value = v;
        if (auto* param = apvts.getParameter(spec.id.c_str())) param->setValueNotifyingHost(param->convertTo0to1(value));
    }
    for (int lane = 0; lane < 12; ++lane)
        setLaneTarget(lane, lane < int(preset.motion.size()) ? juce::String(preset.motion[size_t(lane)].target)
                                                             : juce::String(stasis::params::kDefaultTargets[size_t(lane)]));
    const juce::String order(preset.order);
    setModuleOrder(juce::StringArray::fromTokens(order.isEmpty() ? juce::String(stasis::params::kDefaultOrderNames) : order, ",", ""));
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

void StasisProcessor::setModuleOrder(const juce::StringArray& input) {
    const auto& modules = stasis::params::kModules;
    // An order saved before a module existed: the new module takes its default place
    // (GRAIN goes right after STRETCH).
    juce::StringArray names = input;
    if (!names.contains("grain") && names.size() == int(modules.size()) - 1) names.insert(names.indexOf("stretch") + 1, "grain");
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
    state.setProperty("targets", getLaneTargets().joinIntoString(","), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void StasisProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (!xml->hasTagName(apvts.state.getType())) return;
        const auto state = juce::ValueTree::fromXml(*xml);
        apvts.replaceState(state);
        setModuleOrder(juce::StringArray::fromTokens(state.getProperty("order").toString(), ",", ""));
        currentPreset_ = int(state.getProperty("preset", 0));
        if (state.hasProperty("targets")) {
            const auto ids = juce::StringArray::fromTokens(state.getProperty("targets").toString(), ",", "");
            for (int lane = 0; lane < 12; ++lane) setLaneTarget(lane, lane < ids.size() ? ids[lane] : juce::String());
        } else {
            // Saved before MOTION was assignable (v0.3 and earlier): its lanes had fixed
            // targets and non-zero depths, which would now land on other knobs. Start
            // MOTION from scratch: default targets, every depth at 0.
            for (int lane = 0; lane < 12; ++lane) {
                setLaneTarget(lane, stasis::params::kDefaultTargets[size_t(lane)]);
                if (auto* depth = apvts.getParameter(stasis::params::laneId(lane, "depth").c_str())) depth->setValueNotifyingHost(0.0f);
            }
        }
        juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
            if (safe == nullptr) return;
            if (safe->onOrderChanged) safe->onOrderChanged();
            if (safe->onPresetChanged) safe->onPresetChanged();
        });
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StasisProcessor(); }
