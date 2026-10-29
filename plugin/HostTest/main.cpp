// Loads the built VST3 the way a host does and stresses it: several sample rates and
// block sizes (including odd and oversized ones), random automation of every
// parameter, MIDI, freeze, state save/restore. Fails on NaN/Inf, runaway levels or
// a block that takes longer than real time.
//
//   stasis_hosttest path/to/STASIS.vst3 [--no-timing]
//
// --no-timing reports CPU without failing on it (shared CI machines are noisy).

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

    const bool timing = !(argc > 2 && juce::String(argv[2]) == "--no-timing");
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
    std::printf(failures == 0 ? "ALL TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
