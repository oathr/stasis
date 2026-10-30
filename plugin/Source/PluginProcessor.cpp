#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Presets.h"

namespace {

// The DAW's view: each module's knobs in a group (VST3 units: Reaper, Cubase, Bitwig show
// them; Live lists them flat), every value as text with its unit, and typing a value back
// ("500 ms", "-6 dB", "40 %") finds the knob position that shows it.
juce::String groupOf(const std::string& id) {
    static const std::vector<std::pair<std::string, const char*>> prefixes = {
        {"shape_", "SHAPE"}, {"pitch_", "PITCH"}, {"air_", "AIR"}, {"grain_", "GRAIN"}, {"derez_", "DEREZ"}, {"stretch_", "STRETCH"},
        {"echo_", "ECHO"}, {"space_", "SPACE"}, {"shimmer_", "SPACE"}, {"focus_", "FOCUS"}, {"color_", "COLOR"}, {"tonal", "COLOR"},
        {"motion_", "MOTION"}};
    for (const auto& [prefix, name] : prefixes)
        if (id.rfind(prefix, 0) == 0) return name;
    if (stasis::params::laneOf(id) >= 0) return "MOTION LFO " + juce::String(stasis::params::laneOf(id) + 1);
    return "GLOBAL";
}

float valueFromText(const std::function<std::string(float)>& text, const juce::String& typed) {
    const double wanted = typed.retainCharacters("+-.0123456789").getDoubleValue();
    float best = 0.0f;
    double bestDistance = 1.0e30;
    for (int i = 0; i <= 1000; ++i) {
        const float t = float(i) / 1000.0f;
        const double shown = juce::String(text(t)).retainCharacters("+-.0123456789").getDoubleValue();
        if (std::abs(shown - wanted) < bestDistance) bestDistance = std::abs(shown - wanted), best = t;
    }
    return best;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
    using namespace stasis::params;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::unique_ptr<juce::AudioProcessorParameterGroup> group;
    int groups = 0;
    for (const Spec& s : all()) {
        const auto name = groupOf(s.id);
        if (group == nullptr || group->getName() != name) {
            if (group != nullptr) layout.add(std::move(group));
            // A module may come back later in the table (MOTION's globals, its HARD): unique ids.
            group = std::make_unique<juce::AudioProcessorParameterGroup>(name.toLowerCase().removeCharacters(" ") + juce::String(++groups), name, " | ");
        }
        const juce::ParameterID id{s.id.c_str(), 1};
        switch (s.kind) {
            case Kind::Float: {
                auto text = s.text;
                auto attributes = juce::AudioParameterFloatAttributes().withStringFromValueFunction([text](float v, int) {
                    return text ? juce::String(text(v)) : juce::String(v, 2);
                });
                if (text) attributes = attributes.withValueFromStringFunction([text](const juce::String& typed) { return valueFromText(text, typed); });
                group->addChild(std::make_unique<juce::AudioParameterFloat>(id, juce::String(s.name), juce::NormalisableRange<float>(0.0f, 1.0f),
                                                                            s.defaultValue, attributes));
                break;
            }
            case Kind::Bool:
                group->addChild(std::make_unique<juce::AudioParameterBool>(id, juce::String(s.name), s.defaultValue > 0.5f));
                break;
            case Kind::Choice: {
                juce::StringArray choices;
                for (const auto& c : s.choices) choices.add(c);
                group->addChild(std::make_unique<juce::AudioParameterChoice>(id, juce::String(s.name), choices, int(s.defaultValue)));
                break;
            }
        }
    }
    if (group != nullptr) layout.add(std::move(group));
    return layout;
}
// Saved before GRAIN had intervals (v0.1.0): twelve keys above the source plus 0-2
// octaves. Keep what still exists: unison, minor third, fourth, fifth, and an octave.
void migrateGrainNotes(juce::ValueTree& state) {
    const auto find = [&](const char* id) { return state.getChildWithProperty("id", id); };
    if (!find("grain_n0").isValid() || find("grain_0").isValid()) return;
    const auto on = [&](const char* id) { return float(find(id).getProperty("value", 0.0f)) > 0.5f; };
    const auto add = [&](const char* id, bool value) {
        juce::ValueTree param("PARAM");
        param.setProperty("id", id, nullptr).setProperty("value", value ? 1.0f : 0.0f, nullptr);
        state.appendChild(param, nullptr);
    };
    for (const char* id : {"grain_m12", "grain_m7", "grain_m5", "grain_m3"}) add(id, false);
    add("grain_0", on("grain_n0")), add("grain_p3", on("grain_n3")), add("grain_p5", on("grain_n5")), add("grain_p7", on("grain_n7"));
    add("grain_p12", float(find("grain_octaves").getProperty("value", 1.0f)) >= 0.5f);
}

constexpr int kStateVersion = 4;

// State version 2 (v0.1.3): DEREZ RATE and BITS knobs are reversed (up = clean) and the
// COLOR bands reach twice as far. Older sessions are moved so they sound the same.
void migrateToVersion2(juce::ValueTree& state) {
    const auto remap = [&](const char* id, auto&& f) {
        auto param = state.getChildWithProperty("id", id);
        if (param.isValid()) param.setProperty("value", f(float(param.getProperty("value", 0.0f))), nullptr);
    };
    for (const char* id : {"derez_crush", "derez_bits"}) remap(id, [](float v) { return 1.0f - v; });
    for (const char* id : {"color_bass", "color_body", "color_air"}) remap(id, [](float v) { return 0.5f + 0.5f * (v - 0.5f); });
    for (const char* id : {"color_mud", "color_harsh"}) remap(id, [](float v) { return 0.5f * v; });
}

// State version 3 (v0.1.4): every COLOR word is bipolar in dB (BASS / AIR ±24, BODY / MUD /
// HARSH ±18); MUD and HARSH used to be cut-only amounts. Same sound, new positions.
void migrateToVersion3(juce::ValueTree& state) {
    const auto remap = [&](const char* id, float fallback, auto&& f) {
        auto param = state.getChildWithProperty("id", id);
        if (param.isValid()) param.setProperty("value", f(float(param.getProperty("value", fallback))), nullptr);
    };
    remap("color_bass", 0.5f, [](float v) { return 0.5f + (v - 0.5f) * 18.0f / 24.0f; });
    remap("color_body", 0.5f, [](float v) { return 0.5f + (v - 0.5f) * 12.0f / 18.0f; });
    remap("color_air", 0.5f, [](float v) { return 0.5f + (v - 0.5f) * 16.0f / 24.0f; });
    for (const char* id : {"color_mud", "color_harsh"}) remap(id, 0.0f, [](float v) { return 0.5f - 0.25f * v; });
}

// State version 4 (v0.1.6): COLOR's LOW CUT reaches 5 kHz (was 1 kHz) and HIGH CUT goes
// down to 50 Hz (was 1 kHz). Same frequencies, new positions; a MOTION lane that moves
// only one of them swings by the same number of octaves.
void migrateToVersion4(juce::ValueTree& state) {
    const float lowCut = std::log(50.0f) / std::log(250.0f);  // 20·50^t = 20·250^(t·k)
    const auto remap = [&](const juce::String& id, float fallback, auto&& f) {
        auto param = state.getChildWithProperty("id", id);
        if (param.isValid()) param.setProperty("value", f(float(param.getProperty("value", fallback))), nullptr);
    };
    // GAIN STAGING is now on in a new instance; a session saved before it existed had none.
    if (!state.getChildWithProperty("id", "gain_staging").isValid()) {
        juce::ValueTree param("PARAM");
        param.setProperty("id", "gain_staging", nullptr).setProperty("value", 0.0f, nullptr);
        state.appendChild(param, nullptr);
    }
    // PLATE: DECAY is now a real RT60 (it was a raw tank gain, 0.3 + 0.69 t, applied 4 times
    // per trip of 0.725 s x SIZE scale). Same tail length, new position.
    const auto valueOf = [&](const char* id, float fallback) {
        return float(state.getChildWithProperty("id", id).getProperty("value", fallback));
    };
    if (int(std::lround(valueOf("space_type", 2.0f))) == 1 && valueOf("space_decay", 0.5f) <= 0.98f) {
        const float trip = 0.7254f * (0.5f + 1.5f * valueOf("space_size", 0.6f));
        const float gain = 0.3f + 0.69f * valueOf("space_decay", 0.5f);
        const float rt60 = -0.75f * trip / std::log10(gain);
        remap("space_decay", 0.5f, [&](float) { return std::clamp(std::log(rt60 / 0.5f) / std::log(120.0f), 0.0f, 0.98f); });
    }
    // SHAPE now starts last in the chain: a session saved without an order kept the old one.
    if (!state.hasProperty("order")) state.setProperty("order", "shape,derez,pitch,stretch,grain,echo,space,focus,air,color", nullptr);
    remap("color_hpf", 0.0f, [&](float v) { return v * lowCut; });
    remap("color_lpf", 1.0f, [](float v) { return 0.5f + 0.5f * v; });  // 1000·20^t = 50·400^((1+t)/2)
    const auto lanes = juce::StringArray::fromTokens(state.getProperty("targets").toString(), ",", "");
    for (int lane = 0; lane < lanes.size() && lane < 12; ++lane) {
        const auto targets = juce::StringArray::fromTokens(lanes[lane], "|", "");
        const auto only = [&](const char* id) { return !targets.isEmpty() && std::all_of(targets.begin(), targets.end(), [&](const juce::String& t) { return t == id; }); };
        const juce::String depth(stasis::params::laneId(lane, "depth"));
        if (only("color_hpf")) remap(depth, 0.0f, [&](float v) { return v * lowCut; });
        if (only("color_lpf")) remap(depth, 0.0f, [](float v) { return 0.5f * v; });
    }
}

}  // namespace

StasisProcessor::StasisProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "STASIS", createLayout()) {
    for (const auto& s : stasis::params::all()) raw_.push_back(apvts.getRawParameterValue(s.id.c_str()));
    values_.resize(raw_.size());
    // A new instance starts on Glacier.
    const auto& list = stasis::presets::all();
    for (size_t i = 0; i < list.size(); ++i)
        if (juce::String(list[i].name) == "Glacier") applyPreset(int(i), false);
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

// Per lane: its knobs joined by '|' ("" = empty).
juce::StringArray StasisProcessor::getLaneTargets() const {
    juce::StringArray lanes;
    const auto& specs = stasis::params::all();
    for (int lane = 0; lane < 12; ++lane) {
        juce::StringArray ids;
        for (int slot = 0; slot < int(stasis::Engine::kLaneTargets); ++slot) {
            const int index = engine.laneTargetIndex(lane, slot);
            if (index >= 0) ids.add(juce::String(specs[size_t(index)].id));
        }
        lanes.add(ids.joinIntoString("|"));
    }
    return lanes;
}

// ids: one knob, or several joined by '|' (up to 4), or "" to empty the lane.
void StasisProcessor::setLaneTarget(int lane, const juce::String& ids) {
    if (lane < 0 || lane >= 12) return;
    std::vector<int> indices;
    for (const auto& id : juce::StringArray::fromTokens(ids, "|", "")) {
        const int index = stasis::params::indexOf(id.trim().toStdString());
        if (index < 0) continue;
        const auto& spec = stasis::params::all()[size_t(index)];
        if (spec.kind != stasis::params::Kind::Float || stasis::params::isMotionId(spec.id) || stasis::params::laneOf(spec.id) == lane) continue;
        if (std::find(indices.begin(), indices.end(), index) == indices.end() && indices.size() < stasis::Engine::kLaneTargets) indices.push_back(index);
    }
    // One LFO per knob: a lane already on one of them lets go of it.
    for (int other = 0; other < 12; ++other)
        for (int slot = 0; slot < int(stasis::Engine::kLaneTargets); ++slot)
            if (other != lane && std::find(indices.begin(), indices.end(), engine.laneTargetIndex(other, slot)) != indices.end())
                engine.setLaneTarget(other, slot, -1);
    for (int slot = 0; slot < int(stasis::Engine::kLaneTargets); ++slot)
        engine.setLaneTarget(lane, slot, slot < int(indices.size()) ? indices[size_t(slot)] : -1);
}

// INIT: every knob at its starting point, MOTION back to lane 1 on DRY / WET, the default
// order, the tails cleared. Not a factory preset: the preset name shows INIT.
void StasisProcessor::resetAll() {
    for (auto* param : getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(param)) p->setValueNotifyingHost(p->getDefaultValue());
    for (int lane = 0; lane < 12; ++lane) setLaneTarget(lane, stasis::params::kDefaultTargets[size_t(lane)]);
    resetEngine();
    setModuleOrder(juce::StringArray::fromTokens(stasis::params::kDefaultOrderNames, ",", ""));
    currentPreset_ = -1;
    userPresetName = {};
    juce::MessageManager::callAsync([safe = juce::WeakReference<StasisProcessor>(this)] {
        if (safe == nullptr) return;
        if (safe->onOrderChanged) safe->onOrderChanged();
        if (safe->onPresetChanged) safe->onPresetChanged();
    });
}

int StasisProcessor::getNumPrograms() { return int(stasis::presets::all().size()); }

const juce::String StasisProcessor::getProgramName(int index) {
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return {};
    return juce::String(list[size_t(index)].category) + " / " + list[size_t(index)].name;
}

// Every parameter goes back to its default, then the preset's own values apply. MOTION
// lanes 1, 2, 3… take the preset's modulations; the others go back to their defaults.
void StasisProcessor::applyPreset(int index, bool flush) {
    using stasis::params::laneId;
    const auto& list = stasis::presets::all();
    if (index < 0 || index >= int(list.size())) return;
    const auto& preset = list[size_t(index)];
    // The base (the knobs' former starting points), then the preset's own values: later wins.
    std::vector<std::pair<std::string, float>> values(stasis::presets::kBase.begin(), stasis::presets::kBase.end());
    values.insert(values.end(), preset.values.begin(), preset.values.end());
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
    // A clean slate: the previous preset's tails (reverb, echo, stretch…) are flushed,
    // like PANIC, so the new preset is heard on its own.
    if (flush) resetEngine();
    const juce::String order(preset.order);
    setModuleOrder(juce::StringArray::fromTokens(order.isEmpty() ? juce::String(stasis::params::kDefaultOrderNames) : order, ",", ""));
    currentPreset_ = index;
    userPresetName = {};
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

// The whole state as a tree: parameters, order, MOTION targets, version. A project also
// keeps the preset shown, the layout and the window size; a preset file does not.
juce::ValueTree StasisProcessor::stateTree(bool forPreset) {
    auto state = apvts.copyState();
    state.setProperty("order", getModuleOrder().joinIntoString(","), nullptr);
    state.setProperty("targets", getLaneTargets().joinIntoString(","), nullptr);
    state.setProperty("version", kStateVersion, nullptr);
    if (!forPreset) {
        state.setProperty("preset", currentPreset_, nullptr);
        state.setProperty("userPreset", userPresetName, nullptr);
        state.setProperty("layout", layout, nullptr);
        state.setProperty("motionOpen", motionOpen, nullptr);
        state.setProperty("editorScale", editorScale, nullptr);
    }
    return state;
}

void StasisProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = stateTree(false).createXml()) copyXmlToBinary(*xml, destData);
}

void StasisProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType())) applyStateTree(juce::ValueTree::fromXml(*xml), false);
}

void StasisProcessor::applyStateTree(juce::ValueTree state, bool fromPreset) {
    migrateGrainNotes(state);
    const int version = int(state.getProperty("version", 1));
    if (version < 2) migrateToVersion2(state);
    if (version < 3) migrateToVersion3(state);
    if (version < 4) migrateToVersion4(state);
    apvts.replaceState(state);
    setModuleOrder(juce::StringArray::fromTokens(state.getProperty("order").toString(), ",", ""));
    if (!fromPreset) {
        currentPreset_ = int(state.getProperty("preset", 0));
        userPresetName = state.getProperty("userPreset", "").toString();
        layout = state.getProperty("layout", "top").toString() == "side" ? "side" : "top";
        motionOpen = bool(state.getProperty("motionOpen", true));
        editorScale = double(state.getProperty("editorScale", editorScale));
    }
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

// ---- User presets: one .stasis file each (the state as XML), in Documents/STASIS/Presets.
juce::File StasisProcessor::userPresetFolder() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("STASIS").getChildFile("Presets");
}

juce::StringArray StasisProcessor::userPresets() {
    juce::StringArray names;
    for (const auto& file : userPresetFolder().findChildFiles(juce::File::findFiles, false, "*.stasis"))
        names.add(file.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
}

bool StasisProcessor::exportPreset(const juce::File& file) {
    auto xml = stateTree(true).createXml();
    return xml != nullptr && file.getParentDirectory().createDirectory() && xml->writeTo(file);
}

bool StasisProcessor::saveUserPreset(const juce::String& name) {
    const auto clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty() || !exportPreset(userPresetFolder().getChildFile(clean + ".stasis"))) return false;
    userPresetName = clean;
    currentPreset_ = kUserPreset;
    if (onPresetChanged) onPresetChanged();
    return true;
}

bool StasisProcessor::loadPresetFile(const juce::File& file) {
    auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType())) return false;
    // Like a factory preset: every knob from the file, the old tails flushed.
    applyStateTree(juce::ValueTree::fromXml(*xml), true);
    resetEngine();
    userPresetName = file.getFileNameWithoutExtension();
    currentPreset_ = kUserPreset;
    return true;
}

bool StasisProcessor::importPreset(const juce::File& file) {
    if (!loadPresetFile(file)) return false;
    // Imported presets join the user list.
    const auto copy = userPresetFolder().getChildFile(file.getFileName());
    if (copy != file && userPresetFolder().createDirectory()) file.copyFileTo(copy);
    return true;
}

bool StasisProcessor::deleteUserPreset(const juce::String& name) {
    return userPresetFolder().getChildFile(juce::File::createLegalFileName(name) + ".stasis").deleteFile();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StasisProcessor(); }
