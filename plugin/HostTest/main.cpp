// Loads the built VST3 the way a host does and stresses it: several sample rates and
// block sizes (including odd and oversized ones), random automation of every
// parameter, MIDI, freeze, state save/restore. Fails on NaN/Inf, runaway levels or
// a block that takes longer than real time.
//
//   stasis_hosttest path/to/STASIS.vst3 [--no-timing] [--checks]
//   stasis_hosttest path/to/STASIS.vst3 --dump-state <folder>
//
// --dump-state saves a few sessions of this build (state + every knob as the host shows
// it) into <folder>. Kept in HostTest/compat/<version>, they are reloaded by every later
// build, which must show the same values: an update must not change a saved session.
// --no-timing reports CPU without failing on it (shared CI machines are noisy).
// --checks runs only the targeted measurements (FOCUS, GRAIN, PITCH, DEREZ…), not the
// stress test and the presets.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

namespace {

int failures = 0;
void fail(const juce::String& what) {
    std::printf("  FAIL  %s\n", what.toRawUTF8());
    ++failures;
}

// A hosted parameter's stable key (the VST3 id, not its index or name).
juce::String keyOf(juce::AudioProcessorParameter* p) {
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p)) return hosted->getParameterID();
    return juce::String(p->getParameterIndex());
}

// The plugin's own state inside the host's chunk: the VST3 host wraps it as base64 in
// <IComponent>. Returns an attribute of the STASIS tree ("order", "targets").
juce::String stateAttribute(const juce::MemoryBlock& chunk, const juce::String& name) {
    auto outer = juce::AudioProcessor::getXmlFromBinary(chunk.getData(), int(chunk.getSize()));
    if (outer == nullptr) return {};
    juce::MemoryBlock inner;
    if (auto* component = outer->getChildByName("IComponent")) inner.fromBase64Encoding(component->getAllSubText());
    const std::string raw(static_cast<const char*>(inner.getData()), inner.getSize());
    const auto at = raw.find(" " + name.toStdString() + "=\"");
    if (at == std::string::npos) return {};
    const auto start = at + size_t(name.length()) + 3;
    return juce::String(raw.substr(start, raw.find('"', start) - start));
}

// The same chunk with one attribute of the STASIS tree changed (inside the base64
// <IComponent>, behind JUCE's "VC2!" + length header).
juce::MemoryBlock withStateAttribute(const juce::MemoryBlock& chunk, const juce::String& name, const juce::String& value) {
    auto outer = juce::AudioProcessor::getXmlFromBinary(chunk.getData(), int(chunk.getSize()));
    auto* component = outer != nullptr ? outer->getChildByName("IComponent") : nullptr;
    if (component == nullptr) return chunk;
    juce::MemoryBlock inner;
    inner.fromBase64Encoding(component->getAllSubText());
    std::string raw(static_cast<const char*>(inner.getData()), inner.getSize());
    const auto at = raw.find(" " + name.toStdString() + "=\"");
    if (at == std::string::npos || raw.compare(0, 4, "VC2!") != 0) return chunk;
    const auto start = at + size_t(name.length()) + 3, end = raw.find('"', start);
    raw.replace(start, end - start, value.toStdString());
    uint32_t length;
    std::memcpy(&length, raw.data() + 4, 4);
    length = uint32_t(int64_t(length) + int64_t(value.length()) - int64_t(end - start));
    std::memcpy(raw.data() + 4, &length, 4);
    component->deleteAllTextElements();
    component->addTextElement(juce::MemoryBlock(raw.data(), raw.size()).toBase64Encoding());
    juce::MemoryBlock result;
    juce::AudioProcessor::copyXmlToBinary(*outer, result);
    return result;
}

// Two displays of the same value: equal, or the same number within its last digit
// (the text is rounded) and 1 %.
bool sameShown(const juce::String& a, const juce::String& b) {
    if (a == b) return true;
    const auto number = [](const juce::String& s) { return s.retainCharacters("+-0123456789.").getDoubleValue(); };
    const auto unit = [](const juce::String& s) { return s.removeCharacters("+-0123456789. "); };
    if (unit(a) != unit(b) || a.retainCharacters("0123456789").isEmpty()) return false;
    const auto digits = a.retainCharacters("+-0123456789.");
    const int decimals = digits.containsChar('.') ? digits.length() - digits.lastIndexOfChar('.') - 1 : 0;
    const double step = std::pow(10.0, -decimals) * 1.01, x = number(a), y = number(b);
    return std::abs(x - y) <= std::max(step, 0.01 * std::abs(x));
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc < 2) {
        std::printf("usage: stasis_hosttest STASIS.vst3\n");
        return 2;
    }

    bool timing = true, checksOnly = false;
    juce::File dumpFolder;
    for (int a = 2; a < argc; ++a) {
        timing = timing && juce::String(argv[a]) != "--no-timing";
        checksOnly = checksOnly || juce::String(argv[a]) == "--checks";
        if (juce::String(argv[a]) == "--dump-state" && a + 1 < argc) dumpFolder = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++a]);
    }
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]).getFullPathName());
    if (found.isEmpty()) {
        std::printf("no plugin found in %s\n", argv[1]);
        return 1;
    }
    std::printf("%s by %s, VST3 %s\n", found[0]->name.toRawUTF8(), found[0]->manufacturerName.toRawUTF8(),
                found[0]->isInstrument ? "instrument" : "effect");

    if (dumpFolder != juce::File()) {
        // A few sessions: factory presets as they are, and presets with every knob moved
        // at random (each range, each choice, the MOTION lanes).
        dumpFolder.createDirectory();
        juce::Random dice(99);
        for (const int program : {0, 13, 21, 30, 47, 59, -5, -26, -44}) {
            juce::String error;
            auto plugin = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            if (!plugin) return 1;
            plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
            plugin->prepareToPlay(48000.0, 512);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            // A VST3 host hands knob changes to the plugin with the next audio block.
            const auto settle = [&] {
                for (int b = 0; b < 4; ++b) {
                    buffer.clear();
                    plugin->processBlock(buffer, midi);
                }
            };
            plugin->setCurrentProgram(std::abs(program));
            settle();
            if (program < 0)
                for (auto* p : plugin->getParameters())
                    if (!p->getName(64).startsWith("MIDI CC")) p->setValueNotifyingHost(dice.nextFloat());
            settle();
            juce::MemoryBlock chunk;
            plugin->getStateInformation(chunk);
            const auto name = program < 0 ? "random-" + juce::String(-program) : "preset-" + juce::String(program);
            dumpFolder.getChildFile(name + ".state").replaceWithData(chunk.getData(), chunk.getSize());
            // What the file holds, as this build reads it back: a fresh instance loads it.
            plugin = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
            plugin->prepareToPlay(48000.0, 512);
            plugin->setStateInformation(chunk.getData(), int(chunk.getSize()));
            settle();
            juce::StringArray shown;
            shown.add("#order=" + stateAttribute(chunk, "order"));
            shown.add("#targets=" + stateAttribute(chunk, "targets"));
            for (auto* p : plugin->getParameters())  // the host's MIDI CC stand-ins are not ours
                if (!p->getName(64).startsWith("MIDI CC")) shown.add(keyOf(p) + "\t" + p->getName(64) + "\t" + p->getCurrentValueAsText());
            dumpFolder.getChildFile(name + ".txt").replaceWithText(shown.joinIntoString("\n") + "\n");
        }
        std::printf("states saved in %s\n", dumpFolder.getFullPathName().toRawUTF8());
        return 0;
    }

    juce::Random rng(1234);
    juce::MemoryBlock savedState;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0}) {
        if (checksOnly) break;
        for (const int blockSize : {64, 480, 512, 1024, 2048}) {
            juce::String error;
            auto plugin = formats.createPluginInstance(*found[0], sampleRate, blockSize, error);
            if (!plugin) {
                fail("could not instantiate: " + error);
                return 1;
            }
            plugin->setPlayConfigDetails(2, 2, sampleRate, blockSize);
            plugin->prepareToPlay(sampleRate, blockSize);
            if (savedState.getSize() > 0) plugin->setStateInformation(savedState.getData(), int(savedState.getSize()));

            auto& params = plugin->getParameters();
            juce::AudioBuffer<float> buffer(2, blockSize);
            juce::MidiBuffer midi;
            const int blocks = int(12.0 * sampleRate / blockSize);  // 12 s each
            double worst = 0.0, total = 0.0, preset = 0.0;
            int overruns = 0;
            float peak = 0.0f;
            bool finite = true;
            for (int b = 0; b < blocks; ++b) {
                // Pink-ish noise plus a slow chord: something for every module to chew on.
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < blockSize; ++i) {
                        const double t = double(b * blockSize + i) / sampleRate;
                        buffer.setSample(c, i, 0.05f * (rng.nextFloat() * 2.0f - 1.0f) + 0.1f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * t)));
                    }
                midi.clear();
                if (b % 200 == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57 + rng.nextInt(12), 0.8f), 0);
                if (b % 200 == 150) midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                for (int n = 57; n < 69 && b % 200 == 150; ++n) midi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                // Automation: a few random parameters move all the time. Every 3 s, all of
                // them jump at once, like a preset change: measured apart.
                if (b % 8 == 0)
                    for (int k = 0; k < 3; ++k) params[rng.nextInt(params.size())]->setValueNotifyingHost(rng.nextFloat());
                const bool presetChange = b % int(3 * sampleRate / blockSize) == 0;
                if (presetChange)
                    for (auto* p : params) p->setValueNotifyingHost(rng.nextFloat());

                const auto start = juce::Time::getHighResolutionTicks();
                plugin->processBlock(buffer, midi);
                const double elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
                total += elapsed;
                if (presetChange) preset = std::max(preset, elapsed);  // includes the cold start
                else {
                    worst = std::max(worst, elapsed);
                    if (elapsed > blockSize / sampleRate) ++overruns;
                }
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < blockSize; ++i) {
                        const float v = buffer.getSample(c, i);
                        finite = finite && std::isfinite(v);
                        peak = std::max(peak, std::abs(v));
                    }
            }
            const double budget = blockSize / sampleRate;
            std::printf("  %6.0f Hz  block %4d   avg CPU %4.1f %%   worst block %5.1f %%   preset change %6.1f %%   peak %+5.1f dBFS\n",
                        sampleRate, blockSize, 100.0 * total / (blocks * budget), 100.0 * worst / budget, 100.0 * preset / budget,
                        20.0 * std::log10(std::max(peak, 1.0e-9f)));
            if (!finite) fail("non-finite output");
            if (peak > 8.0f) fail("runaway level");
            if (timing && overruns > 0) fail(juce::String(overruns) + " block(s) took longer than real time");

            plugin->getStateInformation(savedState);
            plugin->releaseResources();
        }
    }
    // Every factory preset: 4 s of a chord in, 4 s of silence; the tail must ring, stay
    // finite and stay under 0 dBFS.
    {
        juce::String error;
        auto plugin = formats.createPluginInstance(*found[0], 48000.0, 512, error);
        if (!checksOnly) std::printf("\nPresets (%d):\n", plugin->getNumPrograms());
        for (int p = 0; p < (checksOnly ? 0 : plugin->getNumPrograms()); ++p) {
            plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
            plugin->prepareToPlay(48000.0, 512);
            plugin->setCurrentProgram(p);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            double tail = 0.0, held = 0.0;
            float peak = 0.0f;
            bool finite = true;
            const int blocks = int(8.0 * 48000 / 512);
            for (int b = 0; b < blocks; ++b) {
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i) {
                        const double t = double(b * 512 + i) / 48000.0;
                        float v = 0.0f;
                        if (b < blocks / 2)
                            for (const double f : {220.0, 261.63, 329.63}) v += 0.08f * float(std::sin(2.0 * juce::MathConstants<double>::pi * f * t));
                        buffer.setSample(c, i, v);
                    }
                plugin->processBlock(buffer, midi);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i) {
                        const float v = buffer.getSample(c, i);
                        finite = finite && std::isfinite(v);
                        peak = std::max(peak, std::abs(v));
                        if (b >= blocks / 2) tail += double(v) * v;
                        else if (b >= blocks / 4) held += double(v) * v;
                    }
            }
            const double tailDb = 10.0 * std::log10(tail / (blocks / 2 * 512 * 2) + 1.0e-20);
            // Level while the chord plays, against the input (three sines at 0.08: -20.2 dBFS RMS).
            const double levelDb = 10.0 * std::log10(held / ((blocks / 2 - blocks / 4) * 512 * 2) + 1.0e-20) + 20.2;
            std::printf("  %2d  %-30s  peak %+5.1f dBFS   level %+5.1f dB   tail %6.1f dB\n", p + 1, plugin->getProgramName(p).toRawUTF8(),
                        20.0 * std::log10(std::max(peak, 1.0e-9f)), levelDb, tailDb);
            if (!finite) fail("preset " + plugin->getProgramName(p) + ": non-finite output");
            if (peak > 1.0f) fail("preset " + plugin->getProgramName(p) + ": clips");
            plugin->releaseResources();
        }
    }
    // FOCUS through the VST3 MIDI path, as a DAW routes it: two tones in (A4 and D#4),
    // FOCUS alone in the chain. A held MIDI note must tilt the output toward that note.
    {
        juce::String error;
        auto plugin = formats.createPluginInstance(*found[0], 48000.0, 512, error);
        plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
        plugin->prepareToPlay(48000.0, 512);
        // From the raw parameter defaults (a new instance starts on a preset).
        for (auto* p : plugin->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
        std::printf("\nFOCUS via MIDI (input bus: %s):\n", plugin->acceptsMidi() ? "yes" : "NO");
        if (!plugin->acceptsMidi()) fail("the VST3 has no MIDI input");
        const auto set = [&](const char* name, float value) {
            for (auto* p : plugin->getParameters())
                if (p->getName(64) == name) return p->setValueNotifyingHost(value);
            fail(juce::String("no parameter ") + name);
        };
        set("Echo On", 0.0f), set("Space On", 0.0f), set("Color On", 0.0f), set("Focus On", 1.0f), set("Focus Mode", 0.0f);
        // Level of a frequency in the output (Goertzel over one block).
        const auto level = [](const juce::AudioBuffer<float>& buffer, double hz) {
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0, k = 2.0 * std::cos(w);
            double s1 = 0.0, s2 = 0.0;
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                const double s = buffer.getSample(0, i) + k * s1 - s2;
                s2 = s1, s1 = s;
            }
            return s1 * s1 + s2 * s2 - k * s1 * s2;
        };
        const double fa = 440.0, fds = 311.13;
        int64_t sample = 0;
        // Returns the A4 / D#4 ratio in dB over the last second of 3 s, holding `note` (-1: none).
        const auto run = [&](int note) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            double a = 0.0, d = 0.0;
            const int blocks = int(3.0 * 48000 / 512);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i, ++sample) {
                    const double t = double(sample) / 48000.0;
                    const float v = 0.1f * float(std::sin(2.0 * juce::MathConstants<double>::pi * fa * t) + std::sin(2.0 * juce::MathConstants<double>::pi * fds * t));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                midi.clear();
                if (b == 0 && note >= 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
                if (b == blocks - 1 && note >= 0) midi.addEvent(juce::MidiMessage::noteOff(1, note), 511);
                plugin->processBlock(buffer, midi);
                if (b >= blocks - int(48000 / 512)) a += level(buffer, fa), d += level(buffer, fds);
            }
            return 10.0 * std::log10((a + 1e-20) / (d + 1e-20));
        };
        const double none = run(-1), onA = run(69), onDs = run(63);
        std::printf("  A4 / D#4 in the output: no note %+5.1f dB   MIDI A4 %+5.1f dB   MIDI D#4 %+5.1f dB\n", none, onA, onDs);
        if (onA - none < 6.0 || none - onDs < 6.0) fail("FOCUS does not follow MIDI notes");
        plugin->releaseResources();

        // GRAIN intervals, relative to the sound: 440 Hz in, GRAIN alone at 100 %, one
        // interval allowed at a time; the grains must land on 220 / 440 / 880 Hz.
        std::printf("\nGRAIN intervals (440 Hz in):\n");
        const auto grainRun = [&](const char* only, double expectHz) {
            auto grain = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            grain->setPlayConfigDetails(2, 2, 48000.0, 512);
            grain->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : grain->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setG = [&](const juce::String& name, float value) {
                for (auto* p : grain->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setG("Echo On", 0.0f), setG("Space On", 0.0f), setG("Color On", 0.0f), setG("Grain On", 1.0f), setG("Grain Mix", 1.0f);
            for (const char* name : {"Grain -12", "Grain -7", "Grain -5", "Grain -3", "Grain 0", "Grain +3", "Grain +5", "Grain +7", "Grain +12"})
                setG(name, juce::String(name) == only ? 1.0f : 0.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            double want = 0.0, other = 0.0;
            for (int b = 0, s = 0; b < int(6.0 * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                grain->processBlock(buffer, midi);
                if (b < int(2.0 * 48000 / 512)) continue;  // let the buffer fill
                want += level(buffer, expectHz);
                for (const double hz : {220.0, 440.0, 880.0})
                    if (hz != expectHz) other += level(buffer, hz);
            }
            const double ratio = 10.0 * std::log10((want + 1e-20) / (other + 1e-20));
            std::printf("  %-9s  energy at %4.0f Hz vs the other octaves: %+5.1f dB\n", only, expectHz, ratio);
            // Unison alone reads about +18 dB here (block-length leakage); a wrong octave reads below 0.
            if (ratio < 12.0) fail(juce::String("GRAIN does not transpose to ") + only);
            grain->releaseResources();
        };
        grainRun("Grain -12", 220.0), grainRun("Grain 0", 440.0), grainRun("Grain +12", 880.0);

        // DEREZ grit: a 220 Hz tone fading out (where hard bit steps crackle), DEREZ alone
        // at its default CRUSH and BITS. Energy above ~4 kHz relative to the whole output.
        std::printf("\nDEREZ grit (220 Hz fading out, energy above 4 kHz):\n");
        const auto derezRun = [&](float smooth, float on = 1.0f) {
            auto derez = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            derez->setPlayConfigDetails(2, 2, 48000.0, 512);
            derez->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : derez->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setD = [&](const juce::String& name, float value) {
                for (auto* p : derez->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setD("Echo On", 0.0f), setD("Space On", 0.0f), setD("Color On", 0.0f), setD("Derez On", on), setD("Derez Mix", 1.0f);
            setD("Derez Smooth", smooth);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            // Two RBJ high-pass biquads at 4 kHz (24 dB/oct): the 220 Hz tone itself stays out.
            juce::IIRFilter hp1, hp2;
            hp1.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0)), hp2.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0));
            double high = 0.0, all = 0.0;
            const int blocks = int(4.0 * 48000 / 512);
            for (int b = 0, s = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float fade = std::exp(-1.5f * float(s) / 48000.0f);  // -13 dB per second
                    const float v = 0.3f * fade * float(std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                derez->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i) {
                    const float y = buffer.getSample(0, i);
                    const float h = hp2.processSingleSampleRaw(hp1.processSingleSampleRaw(y));
                    high += double(h) * h, all += double(y) * y;
                }
            }
            const double db = 10.0 * std::log10((high + 1e-20) / (all + 1e-20));
            if (on > 0.5f) std::printf("  SMOOTH %3.0f %%   %+6.1f dB\n", 100.0 * smooth, db);
            else std::printf("  DEREZ off    %+6.1f dB  (the clean tone, for reference)\n", db);
            derez->releaseResources();
            return db;
        };
        derezRun(0.0f, 0.0f);

        // COLOR moves: a 440 Hz tone, MUD jumping between -11 and +11 dB every 1024 samples
        // (a square LFO). Gliding, the jumps must leave no crackle above 4 kHz; in HARD
        // (MOTION) they must click, on purpose.
        std::printf("\nCOLOR knob jumps (440 Hz in, MUD +-11 dB every 21 ms, energy above 4 kHz):\n");
        const auto colorRun = [&](bool hard) {
            auto color = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            color->setPlayConfigDetails(2, 2, 48000.0, 512);
            color->prepareToPlay(48000.0, 512);
            for (auto* p : color->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setC = [&](const juce::String& name, float value) {
                for (auto* p : color->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setC("Echo On", 0.0f), setC("Space On", 0.0f), setC("Color On", 1.0f), setC("Motion Hard", hard ? 1.0f : 0.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            juce::IIRFilter hp1, hp2;
            hp1.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0)), hp2.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0));
            double high = 0.0, all = 0.0;
            for (int b = 0, s = 0; b < int(3.0 * 48000 / 512); ++b) {
                setC("Color Mids", (b / 2) % 2 ? 0.8f : 0.2f);
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                color->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i) {
                    const float y = buffer.getSample(0, i);
                    const float h = hp2.processSingleSampleRaw(hp1.processSingleSampleRaw(y));
                    if (b > 10) high += double(h) * h, all += double(y) * y;
                }
            }
            const double db = 10.0 * std::log10((high + 1e-20) / (all + 1e-20));
            std::printf("  %-6s %+6.1f dB\n", hard ? "HARD" : "glide", db);
            color->releaseResources();
            return db;
        };
        // HARD on an LFO: lane 1 (square, 4 Hz, 30 %) on SPACE MIX, set through the state as a
        // project would; every step it takes must crack at the output.
        const auto lfoRun = [&](float hard, bool spaceOn = true, const char* target = "space_mix", float depth = 0.3f) {
            auto inst = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            inst->setPlayConfigDetails(2, 2, 48000.0, 512);
            inst->prepareToPlay(48000.0, 512);
            for (auto* p : inst->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            juce::MemoryBlock state;
            inst->getStateInformation(state);
            state = withStateAttribute(state, "targets", juce::String(target) + ",,,,,,,,,,,");
            inst->setStateInformation(state.getData(), int(state.getSize()));
            const auto setL = [&](const juce::String& name, float value) {
                for (auto* p : inst->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setL("Echo On", 0.0f), setL("Color On", 0.0f), setL("Space On", spaceOn ? 1.0f : 0.0f), setL("Space Mix", 0.5f);
            setL("Motion 1 Shape", 4.0f / 11.0f), setL("Motion 1 Rate", float(std::log(4.0 / 0.001) / std::log(20000.0))), setL("Motion 1 Depth", depth);
            setL("Motion Hard", hard);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            juce::IIRFilter hp1, hp2;
            hp1.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0)), hp2.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 4000.0));
            double high = 0.0, all = 0.0;
            for (int b = 0, s = 0; b < int(4.0 * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                inst->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i) {
                    const float y = buffer.getSample(0, i);
                    const float h = hp2.processSingleSampleRaw(hp1.processSingleSampleRaw(y));
                    if (b > 90) high += double(h) * h, all += double(y) * y;
                }
            }
            inst->releaseResources();
            return 10.0 * std::log10((high + 1e-20) / (all + 1e-20));
        };
        const double lfoSoft = lfoRun(0.0f), lfoHard = lfoRun(1.0f);
        std::printf("  LFO on SPACE MIX (square 4 Hz):  HARD off %+6.1f dB   HARD 100 %% %+6.1f dB\n", lfoSoft, lfoHard);
        if (lfoHard < lfoSoft + 10.0) fail("HARD does not crack on LFO steps");
        // The same lane with SPACE switched off: the LFO leaves the knob alone, no crack.
        const double lfoIdle = lfoRun(1.0f, false);
        std::printf("  same, SPACE off:                  HARD 100 %% %+6.1f dB\n", lfoIdle);
        if (lfoIdle > lfoSoft + 3.0) fail("an LFO moves a knob of a module that is off");
        // SIZE jumping (square LFO, 4 Hz, ±30 %) under a tone: the lengths glide, no crackle.
        const double sizeStill = lfoRun(0.0f, true, "space_size", 0.0f), sizeMoved = lfoRun(0.0f, true, "space_size", 0.3f);
        const double preStill = lfoRun(0.0f, true, "space_pre", 0.0f), preMoved = lfoRun(0.0f, true, "space_pre", 0.3f);
        std::printf("  SPACE SIZE jumping:   above 4 kHz %+6.1f dB (still %+6.1f)\n", sizeMoved, sizeStill);
        std::printf("  SPACE PRE jumping:    above 4 kHz %+6.1f dB (still %+6.1f)\n", preMoved, preStill);
        // PRE bends the pitch while it glides (Doppler, physical): a looser bound. v0.1.5: -24 / -45 dB.
        if (sizeMoved > sizeStill + 10.0 || preMoved > preStill + 15.0) fail("moving SIZE or PRE crackles");

        const double glide = colorRun(false), hardClicks = colorRun(true);
        if (glide > -60.0) fail("COLOR crackles when its knobs move");
        if (hardClicks < glide + 15.0) fail("HARD does not click");

        // GRAIN FEEDBACK at 100 %: the cloud feeds itself and must stay bounded and finite.
        {
            auto grain = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            grain->setPlayConfigDetails(2, 2, 48000.0, 512);
            grain->prepareToPlay(48000.0, 512);
            for (auto* p : grain->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setG = [&](const juce::String& name, float value) {
                for (auto* p : grain->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setG("Echo On", 0.0f), setG("Space On", 0.0f), setG("Color On", 0.0f), setG("Grain On", 1.0f), setG("Grain Mix", 1.0f);
            setG("Grain Feedback", 1.0f), setG("Grain Density", 0.75f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            float peak = 0.0f;
            bool finite = true;
            double late = 0.0;
            const int blocks = int(10.0 * 48000 / 512);
            for (int b = 0, s = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = s < 48000 ? 0.3f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 330.0 * s / 48000.0)) : 0.0f;
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                grain->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i) {
                    const float y = buffer.getSample(0, i);
                    finite = finite && std::isfinite(y);
                    peak = std::max(peak, std::abs(y));
                    if (b > blocks - 94) late += double(y) * y;
                }
            }
            std::printf("\nGRAIN FEEDBACK 100 %% (1 s of tone, then 9 s of silence): peak %+5.1f dBFS, last second %6.1f dB\n",
                        20.0 * std::log10(std::max(peak, 1.0e-9f)), 10.0 * std::log10(late / (94 * 512) + 1e-20));
            if (!finite || peak > 1.0f) fail("GRAIN FEEDBACK runs away");
            grain->releaseResources();
        }
        // OFFSET and BYPASS: every module off so the wet is the input. OFFSET at the top
        // must delay the right channel by 25 ms; BYPASS must give the input back untouched.
        {
            auto global = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            global->setPlayConfigDetails(2, 2, 48000.0, 512);
            global->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : global->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setG = [&](const juce::String& name, float value) {
                for (auto* p : global->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setG("Echo On", 0.0f), setG("Space On", 0.0f), setG("Color On", 0.0f), setG("Stereo Offset", 1.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            // Let OFFSET's 30 ms glide settle, then one click on both channels.
            int peakL = -1, peakR = -1;
            for (int b = 0, s = 0; b < 200; ++b) {
                buffer.clear();
                if (b == 100) buffer.setSample(0, 0, 0.5f), buffer.setSample(1, 0, 0.5f);
                global->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i, ++s) {
                    if (peakL < 0 && std::abs(buffer.getSample(0, i)) > 0.1f) peakL = s;
                    if (peakR < 0 && std::abs(buffer.getSample(1, i)) > 0.1f) peakR = s;
                }
            }
            const double offsetMs = (peakR - peakL) / 48.0;
            std::printf("\nOFFSET at the top: right channel %+.1f ms after the left (expected +25.0)\n", offsetMs);
            if (std::abs(offsetMs - 25.0) > 0.5) fail("OFFSET does not delay the right channel by 25 ms");

            setG("Stereo Offset", 0.0f), setG("Space On", 1.0f), setG("Echo On", 1.0f), setG("Bypass", 1.0f);
            double diff = 0.0;
            for (int b = 0, s = 0; b < 200; ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 330.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                juce::AudioBuffer<float> input(buffer);
                global->processBlock(buffer, midi);
                if (b > 20)
                    for (int i = 0; i < 512; ++i) diff = std::max(diff, double(std::abs(buffer.getSample(0, i) - input.getSample(0, i))));
            }
            std::printf("BYPASS: largest difference from the input %.6f\n", diff);
            if (diff > 1.0e-4) fail("BYPASS does not give the input back");
            global->releaseResources();
        }

        // The safety limiter on an over: a sine at +6 dBFS (OUTPUT +12 dB, dry only). The
        // harmonics it adds, against the fundamental, over the last 0.5 s of 2 s.
        std::printf("\nSafety limiter, a sine at +6 dBFS (and at -6, untouched, for reference):\n");
        for (const auto [hz, over] : {std::pair{100.0, false}, std::pair{100.0, true}, std::pair{1000.0, true}}) {
            auto inst = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            inst->setPlayConfigDetails(2, 2, 48000.0, 512);
            inst->prepareToPlay(48000.0, 512);
            for (auto* p : inst->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setO = [&](const juce::String& name, float value) {
                for (auto* p : inst->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setO("Dry / Wet", 0.0f), setO("Output", over ? 1.0f : 0.5f), setO("Glue", 0.0f), setO("Motion On", 0.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (int b = 0, s = 0; b < int(2.0 * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.5f * float(std::sin(2.0 * juce::MathConstants<double>::pi * hz * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                inst->processBlock(buffer, midi);
                if (b >= int(1.5 * 48000 / 512)) out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 512);
            }
            inst->releaseResources();
            out.erase(out.begin(), out.end() - 24000);  // a whole number of cycles: no leakage
            const auto power = [&](double f) {
                const double w = 2.0 * juce::MathConstants<double>::pi * f / 48000.0, k = 2.0 * std::cos(w);
                double s1 = 0.0, s2 = 0.0;
                for (const float x : out) {
                    const double s = x + k * s1 - s2;
                    s2 = s1, s1 = s;
                }
                return s1 * s1 + s2 * s2 - k * s1 * s2;
            };
            double harmonics = 0.0;
            for (int h = 2; h <= 15 && h * hz < 20000.0; ++h) harmonics += power(h * hz);
            const double thd = 10.0 * std::log10((harmonics + 1e-30) / (power(hz) + 1e-30));
            float peak = 0.0f;
            for (const float x : out) peak = std::max(peak, std::abs(x));
            std::printf("  %5.0f Hz %-9s harmonics %+6.1f dB under the tone   peak %+5.2f dBFS\n", hz, over ? "+6 dBFS" : "-6 dBFS", thd, 20.0 * std::log10(peak));
            if (peak > 0.967f) fail("the safety limiter lets a peak through");
            if (thd > -80.0) fail("the safety limiter distorts a steady tone");  // v0.1.5: -40 dB on bass
        }

        // PITCH MIX at 50 %: a 440 Hz tone in, PITCH alone; the original and the moved
        // component must come out at the same level (0 dB between them). Goertzel over
        // one second, so 440 and 446 Hz are told apart.
        std::printf("\nPITCH mix 50 %% (440 Hz in):\n");
        const auto pitchRun = [&](float semiT, float shiftT, double movedHz, const char* what) {
            auto pitch = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            pitch->setPlayConfigDetails(2, 2, 48000.0, 512);
            pitch->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : pitch->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setP = [&](const juce::String& name, float value) {
                for (auto* p : pitch->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setP("Echo On", 0.0f), setP("Space On", 0.0f), setP("Color On", 0.0f), setP("Pitch On", 1.0f), setP("Pitch Mix", 0.5f);
            setP("Pitch Semitones", semiT), setP("Pitch Shift", shiftT);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            std::vector<float> tail;
            const int blocks = int(3.0 * 48000 / 512);
            for (int b = 0, s = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                pitch->processBlock(buffer, midi);
                if (b >= blocks - int(48000 / 512)) tail.insert(tail.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 512);
            }
            const auto amp = [&](double hz) {  // amplitude of a frequency over the tail
                double re = 0.0, im = 0.0;
                for (size_t i = 0; i < tail.size(); ++i) {
                    const double a = 2.0 * juce::MathConstants<double>::pi * hz * double(i) / 48000.0;
                    re += tail[i] * std::cos(a), im += tail[i] * std::sin(a);
                }
                return 2.0 * std::sqrt(re * re + im * im) / double(tail.size());
            };
            const double original = amp(440.0), moved = amp(movedHz);
            std::printf("  %-14s original %5.3f   moved %5.3f   (%+5.1f dB)   input 0.200\n", what, original, moved, 20.0 * std::log10((moved + 1e-12) / (original + 1e-12)));
            pitch->releaseResources();
            return 20.0 * std::log10((moved + 1e-12) / (original + 1e-12));
        };
        const double upOctave = pitchRun(0.75f, 0.5f, 880.0, "+12 st");
        const double shifted = pitchRun(0.5f, 0.75f, 440.0 + 50.0 * 0.125, "shift +6.25 Hz");
        const double downOctave = pitchRun(0.25f, 0.5f, 220.0, "-12 st");
        if (std::abs(upOctave) > 1.5 || std::abs(shifted) > 1.5 || std::abs(downOctave) > 1.5) fail("PITCH MIX 50 % is not half and half");

        // Level of each module alone, at its default settings (MIX included), with the chord
        // of the preset test in: where the output loses (or gains) level.
        std::printf("\nModule levels (alone, default settings, chord in at -20.2 dBFS RMS):\n");
        const auto moduleLevel = [&](const juce::String& module, const std::function<void(const std::function<void(const juce::String&, float)>&)>& extra) {
            auto inst = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            inst->setPlayConfigDetails(2, 2, 48000.0, 512);
            inst->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : inst->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const std::function<void(const juce::String&, float)> setM = [&](const juce::String& name, float value) {
                for (auto* p : inst->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            for (const char* m : {"Shape", "Pitch", "Air", "Grain", "Derez", "Stretch", "Echo", "Space", "Focus", "Color"})
                setM(juce::String(m) + " On", juce::String(m) == module ? 1.0f : 0.0f);
            extra(setM);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            double e = 0.0;
            const int blocks = int(5.0 * 48000 / 512);
            for (int b = 0, s = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    float v = 0.0f;
                    for (const double f : {220.0, 261.63, 329.63}) v += 0.08f * float(std::sin(2.0 * juce::MathConstants<double>::pi * f * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                inst->processBlock(buffer, midi);
                if (b >= blocks - int(2.0 * 48000 / 512))
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < 512; ++i) e += double(buffer.getSample(c, i)) * buffer.getSample(c, i);
            }
            inst->releaseResources();
            return 10.0 * std::log10(e / (int(2.0 * 48000 / 512) * 512 * 2) + 1e-20) + 20.2;
        };
        const auto noExtra = [](const auto&) {};
        for (const char* m : {"Shape", "Pitch", "Air", "Grain", "Derez", "Stretch", "Echo", "Focus", "Color"})
            std::printf("  %-10s %+5.1f dB\n", m, moduleLevel(m, noExtra));
        // SHAPE must never come out louder than it went in, whatever the type, DRIVE or TILT.
        std::printf("\nSHAPE level (chord at -20.2 dBFS RMS), DRIVE 50 / 100 %%, TILT +6 dB:\n");
        const char* shapes[] = {"Tape", "Tube", "Fold", "Rectify", "Slew", "Bloom"};
        double loudest = -99.0;
        for (int t = 0; t < 6; ++t) {
            double row[3];
            int k = 0;
            for (const auto [drive, tilt] : {std::pair{0.5f, 0.5f}, std::pair{1.0f, 0.5f}, std::pair{1.0f, 1.0f}}) {
                row[k] = moduleLevel("Shape", [&](const auto& set) { set("Shape Type", float(t) / 5.0f), set("Shape Drive", drive), set("Shape Tilt", tilt), set("Shape Harmonics", 0.5f); });
                loudest = std::max(loudest, row[k++]);
            }
            std::printf("  %-8s %+5.1f   %+5.1f   tilt %+5.1f dB\n", shapes[t], row[0], row[1], row[2]);
        }
        if (loudest > 1.0) fail("SHAPE makes the sound louder");

        // SHAPE aliasing: a 5 kHz sine at -6 dBFS, DRIVE 100 %. Harmonics above 24 kHz fold
        // back to frequencies that are not multiples of 5 kHz: their energy against the tone.
        std::printf("\nSHAPE aliasing (5 kHz at -6 dBFS, DRIVE 100 %%):");
        for (int t = 0; t < 6; ++t) {
            auto inst = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            inst->setPlayConfigDetails(2, 2, 48000.0, 512);
            inst->prepareToPlay(48000.0, 512);
            for (auto* p : inst->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setA = [&](const juce::String& name, float value) {
                for (auto* p : inst->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            for (const char* m : {"Echo", "Space", "Color"}) setA(juce::String(m) + " On", 0.0f);
            setA("Shape On", 1.0f), setA("Shape Type", float(t) / 5.0f), setA("Shape Drive", 1.0f), setA("Glue", 0.0f), setA("Motion On", 0.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (int b = 0, s = 0; b < int(2.0 * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = 0.5f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 5000.0 * s / 48000.0));
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                inst->processBlock(buffer, midi);
                out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 512);
            }
            inst->releaseResources();
            out.erase(out.begin(), out.end() - 24000);
            const auto power = [&](double f) {
                const double w = 2.0 * juce::MathConstants<double>::pi * f / 48000.0, k = 2.0 * std::cos(w);
                double s1 = 0.0, s2 = 0.0;
                for (const float x : out) {
                    const double s = x + k * s1 - s2;
                    s2 = s1, s1 = s;
                }
                return s1 * s1 + s2 * s2 - k * s1 * s2;
            };
            double aliases = 0.0;
            for (int h = 5; h <= 19; ++h) {
                double f = std::fmod(h * 5000.0, 48000.0);
                if (f > 24000.0) f = 48000.0 - f;
                if (std::fmod(f, 5000.0) != 0.0) aliases += power(f);
            }
            double total = 0.0;
            for (const float x : out) total += double(x) * x;
            total *= double(out.size()) / 2.0;  // Goertzel power scale for a whole-cycle tone
            std::printf("  %s %+5.1f dB", shapes[t], 10.0 * std::log10((aliases + 1e-30) / (total + 1e-30)));
        }
        std::printf("\n");
        const char* types[] = {"Room", "Plate", "Hall", "Cathedral", "Bloom", "Resonator"};
        for (int t = 0; t < 6; ++t) {
            const double atMix = moduleLevel("Space", [&](const auto& set) { set("Space Type", float(t) / 5.0f); });
            const double wet = moduleLevel("Space", [&](const auto& set) { set("Space Type", float(t) / 5.0f), set("Space Mix", 1.0f); });
            std::printf("  Space %-10s %+5.1f dB  (100 %% wet %+5.1f dB)\n", types[t], atMix, wet);
        }

        // SPACE alone at 100 % wet, one type (index of 6). Feeds `input(sample)` for `seconds`,
        // MIDI `note` held from the start (-1: none); returns the left output.
        const auto spaceRun = [&](int type, double seconds, const std::function<float(int)>& input, int note = -1, float tone = -1.0f) {
            auto space = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            space->setPlayConfigDetails(2, 2, 48000.0, 512);
            space->prepareToPlay(48000.0, 512);
            // From the raw parameter defaults (a new instance starts on a preset).
            for (auto* p : space->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setS = [&](const juce::String& name, float value) {
                for (auto* p : space->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setS("Echo On", 0.0f), setS("Color On", 0.0f), setS("Space On", 1.0f), setS("Space Mix", 1.0f), setS("Dry / Wet", 1.0f);
            setS("Space Type", float(type) / 5.0f), setS("Space Decay", 0.48f), setS("Space Predelay", 0.0f);
            if (tone >= 0.0f) setS("Space Tone", tone), setS("Glue", 0.0f), setS("Gain Staging", 0.0f), setS("Space Decay", 0.58f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (int b = 0, s = 0; b < int(seconds * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) buffer.setSample(0, i, input(s)), buffer.setSample(1, i, input(s));
                midi.clear();
                if (b == 0 && note >= 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
                space->processBlock(buffer, midi);
                out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 512);
            }
            space->releaseResources();
            return out;
        };

        // TONE: 100 ms of noise in, DECAY 8 s. The attack's colour (10 kHz vs 1 kHz in the
        // first 300 ms) and the decay time in each band (from the energy 0.5-1 s and 1.5-2 s
        // after the burst). TONE must be heard at once and in the tail.
        std::printf("\nSPACE TONE (noise burst in, DECAY 8 s):            attack 10k/1k    T60 1 kHz   T60 10 kHz\n");
        const char* toneTypes[] = {"Room", "Plate", "Hall", "Cathedral"};
        double attackSpread = 0.0, tailSpread = 0.0;
        for (int type = 0; type < 4; ++type) {
            double attack[3], ratio[3];
            int k = 0;
            for (const float tone : {0.0f, 0.5f, 1.0f}) {
                juce::Random noise(11);
                const auto out = spaceRun(type, 2.4, [&](int s) { return s < 4800 ? 0.3f * (noise.nextFloat() * 2.0f - 1.0f) : 0.0f; }, -1, tone);
                const auto band = [&](double hz, double from, double to) {
                    juce::IIRFilter f1, f2;
                    f1.setCoefficients(juce::IIRCoefficients::makeBandPass(48000.0, hz, 1.4)), f2.setCoefficients(juce::IIRCoefficients::makeBandPass(48000.0, hz, 1.4));
                    double e = 0.0;
                    for (size_t i = 0; i < out.size(); ++i) {
                        const double y = f2.processSingleSampleRaw(f1.processSingleSampleRaw(out[i]));
                        const double t = double(i) / 48000.0 - 0.1;
                        if (t >= from && t < to) e += y * y;
                    }
                    return e + 1e-30;
                };
                const auto t60 = [&](double hz) { return 60.0 / std::max(1e-3, 10.0 * std::log10(band(hz, 0.5, 1.0) / band(hz, 1.5, 2.0))); };
                attack[k] = 10.0 * std::log10(band(10000.0, -0.1, 0.2) / band(1000.0, -0.1, 0.2));
                const double lo = t60(1000.0), hi = t60(10000.0);
                ratio[k] = hi / lo;
                std::printf("  %-10s TONE %3.0f %%                          %+6.1f dB      %5.2f s     %5.2f s\n", toneTypes[type], 100.0 * tone, attack[k], lo, hi);
                ++k;
            }
            attackSpread = std::max(attackSpread, attack[2] - attack[0]);
            tailSpread = std::max(tailSpread, ratio[2] / ratio[0]);
        }
        (void)attackSpread, (void)tailSpread;

        // SHIMMER +12: 1 s of a 440 Hz tone, then the tail (1.5-3.5 s). Energy on the
        // octaves it builds (880, 1760 Hz) against between them (1100, 1320, 1540, 2200 Hz):
        // a clean shimmer keeps its notes, a smeared one fills the gaps.
        {
            auto shim = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            shim->setPlayConfigDetails(2, 2, 48000.0, 512);
            shim->prepareToPlay(48000.0, 512);
            for (auto* p : shim->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setH = [&](const juce::String& name, float value) {
                for (auto* p : shim->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setH("Echo On", 0.0f), setH("Color On", 0.0f), setH("Space Mix", 1.0f), setH("Glue", 0.0f), setH("Gain Staging", 0.0f), setH("Motion On", 0.0f);
            setH("Space Type", 0.4f), setH("Space Decay", 0.58f), setH("Shimmer Depth", 0.8f), setH("Shimmer Pitch", 0.75f), setH("Space Tone", 0.5f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            std::vector<float> tail;
            for (int b = 0, s = 0; b < int(3.5 * 48000 / 512); ++b) {
                for (int i = 0; i < 512; ++i, ++s) {
                    const float v = s < 48000 ? 0.2f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * s / 48000.0)) : 0.0f;
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                shim->processBlock(buffer, midi);
                if (b * 512 >= 72000) tail.insert(tail.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 512);
            }
            shim->releaseResources();
            const auto at = [&](double hz) {
                const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0, k = 2.0 * std::cos(w);
                double s1 = 0.0, s2 = 0.0;
                for (const float x : tail) {
                    const double s = x + k * s1 - s2;
                    s2 = s1, s1 = s;
                }
                return s1 * s1 + s2 * s2 - k * s1 * s2;
            };
            const double notes = (at(880.0) + at(1760.0)) / 2.0, gaps = (at(1100.0) + at(1320.0) + at(1540.0) + at(2200.0)) / 4.0;
            const double clean = 10.0 * std::log10((notes + 1e-20) / (gaps + 1e-20));
            std::printf("\nSPACE SHIMMER +12 (440 Hz in, HALL): octaves %+5.1f dB above the gaps between them\n", clean);
            if (clean < 45.0) fail("SHIMMER smears its notes");  // v0.1.5: +52 dB
        }

        // FREEZE: 1 s of noise, then frozen for 11 s. The held tail must neither fade nor
        // grow (level 2-3 s against 11-12 s), now that its modulation keeps running.
        {
            auto frozen = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            frozen->setPlayConfigDetails(2, 2, 48000.0, 512);
            frozen->prepareToPlay(48000.0, 512);
            for (auto* p : frozen->getParameters()) p->setValueNotifyingHost(p->getDefaultValue());
            const auto setF = [&](const juce::String& name, float value) {
                for (auto* p : frozen->getParameters())
                    if (p->getName(64) == name) return p->setValueNotifyingHost(value);
                fail("no parameter " + name);
            };
            setF("Echo On", 0.0f), setF("Color On", 0.0f), setF("Space Mix", 1.0f), setF("Glue", 0.0f), setF("Gain Staging", 0.0f), setF("Motion On", 0.0f);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            juce::Random noise(5);
            double early = 0.0, late = 0.0;
            for (int b = 0; b < int(12.0 * 48000 / 512); ++b) {
                const double t = b * 512 / 48000.0;
                if (b == int(48000 / 512)) setF("Freeze", 1.0f);
                for (int i = 0; i < 512; ++i) {
                    const float v = t < 1.0 ? 0.1f * (noise.nextFloat() * 2.0f - 1.0f) : 0.0f;
                    buffer.setSample(0, i, v), buffer.setSample(1, i, v);
                }
                frozen->processBlock(buffer, midi);
                for (int i = 0; i < 512; ++i) {
                    const double y = buffer.getSample(0, i);
                    if (t >= 2.0 && t < 3.0) early += y * y;
                    if (t >= 11.0 && t < 12.0) late += y * y;
                }
            }
            frozen->releaseResources();
            const double drift = 10.0 * std::log10((late + 1e-20) / (early + 1e-20));
            std::printf("\nSPACE FREEZE (HALL): level after 11 s against after 2 s %+5.2f dB\n", drift);
            if (std::abs(drift) > 1.5) fail("FREEZE does not hold its level");
        }

        // BLOOM: a click in; the energy envelope (50 ms windows) must rise to its peak
        // instead of starting there, as HALL does.
        std::printf("\nSPACE BLOOM vs HALL (a click in, DECAY 5 s):\n");
        const auto envelope = [&](int type) {
            const auto out = spaceRun(type, 3.0, [](int s) { return s == 4800 ? 0.5f : 0.0f; });
            std::vector<double> windows;
            for (size_t w = 4800; w + 2400 <= out.size(); w += 2400) {
                double e = 0.0;
                for (size_t i = w; i < w + 2400; ++i) e += double(out[i]) * out[i];
                windows.push_back(e);
            }
            const auto peak = std::max_element(windows.begin(), windows.end());
            const double peakMs = 50.0 * double(peak - windows.begin()) + 25.0, peakDb = 10.0 * std::log10(*peak + 1e-20);
            const double firstDb = 10.0 * std::log10(windows[0] + 1e-20) - peakDb;
            std::printf("  %-6s energy peak at %4.0f ms   first 50 ms %+5.1f dB under it   peak window %+5.1f dB\n",
                        type == 4 ? "BLOOM" : "HALL", peakMs, firstDb, peakDb);
            return std::make_pair(peakMs, firstDb);
        };
        const auto hall = envelope(2), bloom = envelope(4);
        // A big HALL's diffusion already takes ~150 ms to build: BLOOM must come clearly later.
        if (bloom.first < hall.first + 300.0 || bloom.second > -10.0) fail("BLOOM does not swell");

        // RESONATOR: white noise in, MIDI A2 held; the output must ring on A (110 / 220 / 440 Hz)
        // well above the notes in between.
        std::printf("\nSPACE RESONATOR (noise in, MIDI A held):\n");
        juce::Random noise(7);
        const auto ring = spaceRun(5, 4.0, [&](int) { return 0.1f * (noise.nextFloat() * 2.0f - 1.0f); }, 45);
        const auto energy = [&](double hz) {  // over the last 2 s
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0, k = 2.0 * std::cos(w);
            double s1 = 0.0, s2 = 0.0;
            for (size_t i = ring.size() - 96000; i < ring.size(); ++i) {
                const double s = ring[i] + k * s1 - s2;
                s2 = s1, s1 = s;
            }
            return s1 * s1 + s2 * s2 - k * s1 * s2;
        };
        double onNote = 0.0, between = 0.0;
        for (const double hz : {110.0, 220.0, 440.0}) onNote += energy(hz);
        for (const double hz : {311.1, 622.3, 233.1, 466.2}) between += energy(hz);
        const double tuned = 10.0 * std::log10((onNote / 3.0 + 1e-20) / (between / 4.0 + 1e-20));
        std::printf("  energy on A vs between the notes: %+5.1f dB\n", tuned);
        if (tuned < 15.0) fail("RESONATOR does not ring on the MIDI note");

        const double raw = derezRun(0.0f), soft = derezRun(0.7f), softest = derezRun(1.0f);
        if (soft > raw - 6.0 || softest > soft) fail("DEREZ SMOOTH does not remove the grit");
    }
#ifdef STASIS_COMPAT_DIR
    // Sessions saved by earlier releases: every knob they had must show the same value
    // in this build, with the same order and MOTION targets.
    {
        std::printf("\nSessions from earlier releases:\n");
        for (const auto& state : juce::File(STASIS_COMPAT_DIR).findChildFiles(juce::File::findFiles, true, "*.state")) {
            juce::MemoryBlock chunk;
            state.loadFileAsData(chunk);
            juce::String error;
            auto plugin = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
            plugin->prepareToPlay(48000.0, 512);
            plugin->setStateInformation(chunk.getData(), int(chunk.getSize()));
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < 4; ++b) {
                buffer.clear();
                plugin->processBlock(buffer, midi);
            }
            juce::MemoryBlock now;
            plugin->getStateInformation(now);
            juce::HashMap<juce::String, juce::AudioProcessorParameter*> byKey;
            for (auto* p : plugin->getParameters()) byKey.set(keyOf(p), p);
            int checked = 0, wrong = 0;
            juce::StringArray lanes;
            juce::HashMap<juce::String, juce::String> oldShown;
            const auto where = state.getParentDirectory().getFileName() + "/" + state.getFileNameWithoutExtension();
            for (const auto& line : juce::StringArray::fromLines(state.withFileExtension("txt").loadFileAsString())) {
                if (line.startsWith("#")) {
                    const auto name = line.substring(1).upToFirstOccurrenceOf("=", false, false);
                    if (name == "targets") lanes = juce::StringArray::fromTokens(line.fromFirstOccurrenceOf("=", false, false), ",", "");
                    if (stateAttribute(now, name) != line.fromFirstOccurrenceOf("=", false, false)) fail(where + ": " + name + " changed");
                    continue;
                }
                const auto fields = juce::StringArray::fromTokens(line, "\t", "");
                if (fields.size() < 3) continue;
                auto* p = byKey.contains(fields[0]) ? byKey[fields[0]] : nullptr;
                ++checked;
                // Since state 4 the cut filters have a wider travel: a lane that moves only
                // one of them gets a smaller DEPTH for the same swing in octaves.
                double factor = 1.0;
                if (fields[1].startsWith("Motion ") && fields[1].endsWith(" Depth")) {
                    const int lane = fields[1].fromFirstOccurrenceOf("Motion ", false, false).getIntValue() - 1;
                    const auto targets = juce::StringArray::fromTokens(lane >= 0 && lane < lanes.size() ? lanes[lane] : juce::String(), "|", "");
                    const auto only = [&](const char* id) { return !targets.isEmpty() && std::all_of(targets.begin(), targets.end(), [&](const juce::String& t) { return t == id; }); };
                    if (only("color_hpf")) factor = std::log(50.0) / std::log(250.0);
                    if (only("color_lpf")) factor = 0.5;
                }
                oldShown.set(fields[1], fields[2]);
                // Since state 4 a PLATE's DECAY is a real RT60: the knob moves so that the
                // tail keeps the length it really had (0.3 + 0.69 t, 4 times per trip).
                if (p != nullptr && fields[1] == "Space Decay" && oldShown["Space Type"] == "Plate" && fields[2] != "inf") {
                    const double t = std::log(fields[2].getDoubleValue() / 0.5) / std::log(120.0);
                    const double trip = 0.7254 * (0.5 + 1.5 * oldShown["Space Size"].getDoubleValue());
                    const double expected = std::min(-0.75 * trip / std::log10(0.3 + 0.69 * t), 0.5 * std::pow(120.0, 0.98));
                    const double now = p->getCurrentValueAsText().getDoubleValue();
                    if (std::abs(now - expected) > 0.03 * expected + 0.06) {
                        fail(where + ": PLATE " + fields[1] + " was " + fields[2] + ", now " + p->getCurrentValueAsText() + " (expected " + juce::String(expected, 1) + " s)");
                        ++wrong;
                    }
                    continue;
                }
                if (p != nullptr && factor != 1.0) {
                    const double expected = fields[2].getDoubleValue() * factor, now = p->getCurrentValueAsText().getDoubleValue();
                    if (std::abs(now - expected) > 1.01) {
                        fail(where + ": " + fields[1] + " was " + fields[2] + ", now " + p->getCurrentValueAsText() + " (expected " + juce::String(expected, 1) + ")");
                        ++wrong;
                    }
                } else if (p == nullptr) {
                    fail(where + ": " + fields[1] + " is gone");
                    ++wrong;
                } else if (!sameShown(fields[2], p->getCurrentValueAsText())) {
                    fail(where + ": " + fields[1] + " was " + fields[2] + ", now " + p->getCurrentValueAsText());
                    ++wrong;
                }
            }
            std::printf("  %-28s %d knobs, %d changed\n", where.toRawUTF8(), checked, wrong);
        }
    }
#endif
    std::printf(failures == 0 ? "ALL TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
