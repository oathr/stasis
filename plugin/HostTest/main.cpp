// Loads the built VST3 the way a host does and stresses it: several sample rates and
// block sizes (including odd and oversized ones), random automation of every
// parameter, MIDI, freeze, state save/restore. Fails on NaN/Inf, runaway levels or
// a block that takes longer than real time.
//
//   stasis_hosttest path/to/STASIS.vst3 [--no-timing] [--checks]
//
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

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc < 2) {
        std::printf("usage: stasis_hosttest STASIS.vst3\n");
        return 2;
    }

    bool timing = true, checksOnly = false;
    for (int a = 2; a < argc; ++a) {
        timing = timing && juce::String(argv[a]) != "--no-timing";
        checksOnly = checksOnly || juce::String(argv[a]) == "--checks";
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
            double tail = 0.0;
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
                    }
            }
            const double tailDb = 10.0 * std::log10(tail / (blocks / 2 * 512 * 2) + 1.0e-20);
            std::printf("  %2d  %-30s  peak %+5.1f dBFS   tail %6.1f dB\n", p + 1, plugin->getProgramName(p).toRawUTF8(),
                        20.0 * std::log10(std::max(peak, 1.0e-9f)), tailDb);
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
        // OFFSET and BYPASS: every module off so the wet is the input. OFFSET at the top
        // must delay the right channel by 25 ms; BYPASS must give the input back untouched.
        {
            auto global = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            global->setPlayConfigDetails(2, 2, 48000.0, 512);
            global->prepareToPlay(48000.0, 512);
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

        // PITCH MIX at 50 %: a 440 Hz tone in, PITCH alone; the original and the moved
        // component must come out at the same level (0 dB between them). Goertzel over
        // one second, so 440 and 446 Hz are told apart.
        std::printf("\nPITCH mix 50 %% (440 Hz in):\n");
        const auto pitchRun = [&](float semiT, float shiftT, double movedHz, const char* what) {
            auto pitch = formats.createPluginInstance(*found[0], 48000.0, 512, error);
            pitch->setPlayConfigDetails(2, 2, 48000.0, 512);
            pitch->prepareToPlay(48000.0, 512);
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

        const double raw = derezRun(0.0f), soft = derezRun(0.7f), softest = derezRun(1.0f);
        if (soft > raw - 6.0 || softest > soft) fail("DEREZ SMOOTH does not remove the grit");
    }
    std::printf(failures == 0 ? "ALL TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
