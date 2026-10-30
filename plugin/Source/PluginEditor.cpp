#include "PluginEditor.h"

#include <BinaryData.h>

#include "Presets.h"

namespace {

constexpr int kWidth = 1251, kHeight = 934;  // the page (1500 x 1120) scaled: same proportions, no side bands
constexpr int kSideWidth = 1455, kSideHeight = 814;  // the same, visualizer on the right (page 1744 x 976)
constexpr int kBands = 96;                     // spectrum rows sent to the visual (MIDI 30 .. 118)

std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url) {
    const auto make = [](const char* data, int size, const char* mime) {
        const auto* bytes = reinterpret_cast<const std::byte*>(data);
        return juce::WebBrowserComponent::Resource{std::vector<std::byte>(bytes, bytes + size), mime};
    };
    const auto path = url.upToFirstOccurrenceOf("?", false, false);
    if (path == "/" || path == "/index.html") return make(BinaryData::mockup_html, BinaryData::mockup_htmlSize, "text/html");
    if (path == "/juce/index.js") return make(BinaryData::index_js, BinaryData::index_jsSize, "text/javascript");
    if (path == "/juce/check_native_interop.js")
        return make(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize, "text/javascript");
    return std::nullopt;
}

}  // namespace

StasisEditor::Relays StasisEditor::createRelays() {
    using namespace stasis::params;
    Relays r;
    for (const Spec& s : all()) {
        switch (s.kind) {
            case Kind::Float: r.sliders.push_back(std::make_unique<juce::WebSliderRelay>(s.id.c_str())); break;
            case Kind::Bool: r.toggles.push_back(std::make_unique<juce::WebToggleButtonRelay>(s.id.c_str())); break;
            case Kind::Choice: r.combos.push_back(std::make_unique<juce::WebComboBoxRelay>(s.id.c_str())); break;
        }
    }
    return r;
}

juce::WebBrowserComponent::Options StasisEditor::createOptions(Relays& relays) {
    auto options = juce::WebBrowserComponent::Options{}
                       .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
                       .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}.withUserDataFolder(
                           juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("STASIS")))
                       .withNativeIntegrationEnabled()
                       .withResourceProvider(getResource)
                       // Module order: the page asks for it on load and sends every change.
                       .withNativeFunction("getOrder", [this](const juce::Array<juce::var>&, auto complete) {
                           juce::Array<juce::var> names;
                           for (const auto& n : processor_.getModuleOrder()) names.add(n);
                           complete(names);
                       })
                       // Presets: the list (category, name), and loading one by index.
                       .withNativeFunction("getPresets", [this](const juce::Array<juce::var>&, auto complete) {
                           juce::Array<juce::var> list;
                           for (const auto& p : stasis::presets::all()) {
                               auto* item = new juce::DynamicObject();
                               item->setProperty("category", p.category);
                               item->setProperty("name", p.name);
                               list.add(juce::var(item));
                           }
                           auto* result = new juce::DynamicObject();
                           result->setProperty("list", list);
                           result->setProperty("current", processor_.currentPresetOrInit());
                           complete(juce::var(result));
                       })
                       .withNativeFunction("loadPreset", [this](const juce::Array<juce::var>& args, auto complete) {
                           if (!args.isEmpty()) processor_.applyPreset(int(args[0]));
                           complete(juce::var());
                       })
                       // MOTION mapping.
                       .withNativeFunction("getLaneTargets", [this](const juce::Array<juce::var>&, auto complete) {
                           juce::Array<juce::var> ids;
                           for (const auto& id : processor_.getLaneTargets()) ids.add(id);
                           complete(ids);
                       })
                       .withNativeFunction("setLaneTarget", [this](const juce::Array<juce::var>& args, auto complete) {
                           if (args.size() >= 2) processor_.setLaneTarget(int(args[0]), args[1].toString());
                           // Another lane may have let go of that knob: show every lane's target.
                           juce::Array<juce::var> ids;
                           for (const auto& id : processor_.getLaneTargets()) ids.add(id);
                           complete(ids);
                       })
                       // Visualizer on top or on the side: the window takes the page's proportions.
                       .withNativeFunction("getLayout", [this](const juce::Array<juce::var>&, auto complete) { complete(processor_.layout); })
                       .withNativeFunction("setLayout", [this](const juce::Array<juce::var>& args, auto complete) {
                           processor_.layout = !args.isEmpty() && args[0].toString() == "side" ? "side" : "top";
                           applyLayout(true);
                           complete(juce::var());
                       })
                       .withNativeFunction("resetAll", [this](const juce::Array<juce::var>&, auto complete) {
                           processor_.resetAll();
                           complete(juce::var());
                       })
                       .withNativeFunction("panic", [this](const juce::Array<juce::var>&, auto complete) {
                           processor_.panic();
                           complete(juce::var());
                       })
                       .withNativeFunction("setOrder", [this](const juce::Array<juce::var>& args, auto complete) {
                           juce::StringArray names;
                           if (!args.isEmpty())
                               if (const auto* list = args[0].getArray())
                                   for (const auto& n : *list) names.add(n.toString());
                           processor_.setModuleOrder(names);
                           complete(juce::var());
                       });
    for (auto& s : relays.sliders) options = options.withOptionsFrom(*s);
    for (auto& t : relays.toggles) options = options.withOptionsFrom(*t);
    for (auto& c : relays.combos) options = options.withOptionsFrom(*c);
    return options;
}

StasisEditor::StasisEditor(StasisProcessor& p)
    : AudioProcessorEditor(p), processor_(p), relays_(createRelays()), webView_(createOptions(relays_)) {
    using namespace stasis::params;
    size_t si = 0, ti = 0, ci = 0;
    for (const Spec& s : all()) {
        auto& param = *p.apvts.getParameter(s.id.c_str());
        switch (s.kind) {
            case Kind::Float:
                sliderAttachments_.push_back(std::make_unique<juce::WebSliderParameterAttachment>(param, *relays_.sliders[si++], nullptr));
                break;
            case Kind::Bool:
                toggleAttachments_.push_back(std::make_unique<juce::WebToggleButtonParameterAttachment>(param, *relays_.toggles[ti++], nullptr));
                break;
            case Kind::Choice:
                comboAttachments_.push_back(std::make_unique<juce::WebComboBoxParameterAttachment>(param, *relays_.combos[ci++], nullptr));
                break;
        }
    }

    const int size = stasis::Engine::kVisualSize;
    samples_.resize(size_t(size));
    spectrum_.resize(size_t(size));
    window_.resize(size_t(size));
    for (int i = 0; i < size; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(stasis::kTwoPi * float(i) / float(size));
    fft_.prepare(size);

    // A preset or project load changes the order: show it.
    processor_.onOrderChanged = [this] {
        sendOrder();
        juce::Array<juce::var> ids;
        for (const auto& id : processor_.getLaneTargets()) ids.add(id);
        webView_.emitEventIfBrowserIsVisible("stasis_targets", ids);
    };
    processor_.onPresetChanged = [this] { webView_.emitEventIfBrowserIsVisible("stasis_preset", processor_.currentPresetOrInit()); };

    addAndMakeVisible(webView_);
    webView_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable(true, true);
    applyLayout(false);
    startTimerHz(30);
}

StasisEditor::~StasisEditor() {
    stopTimer();
    processor_.onOrderChanged = nullptr;
    processor_.onPresetChanged = nullptr;
}

// Window limits and proportions for the current layout. keepScale: same zoom as now.
void StasisEditor::applyLayout(bool keepScale) {
    const bool side = processor_.layout == "side";
    const int w = side ? kSideWidth : kWidth, h = side ? kSideHeight : kHeight;
    const double scale = keepScale && layoutWidth_ > 0 ? double(getWidth()) / layoutWidth_ : 0.8;
    layoutWidth_ = w;
    setResizeLimits(w / 2, h / 2, w * 3 / 2, h * 3 / 2);
    getConstrainer()->setFixedAspectRatio(double(w) / h);
    setSize(int(w * scale), int(h * scale));
}

void StasisEditor::sendOrder() {
    juce::Array<juce::var> names;
    for (const auto& n : processor_.getModuleOrder()) names.add(n);
    webView_.emitEventIfBrowserIsVisible("stasis_order", names);
}

void StasisEditor::resized() { webView_.setBounds(getLocalBounds()); }

void StasisEditor::timerCallback() {
    sendSpectrum();
    // MOTION: each lane's LFO value and where it has moved its knob.
    juce::Array<juce::var> lanes, targets;
    for (int i = 0; i < 12; ++i) {
        lanes.add(processor_.engine.laneValue(i));
        juce::Array<juce::var> slots;  // where each of the lane's knobs is now
        for (int s = 0; s < int(stasis::Engine::kLaneTargets); ++s) slots.add(processor_.engine.laneTarget(i, s));
        targets.add(slots);
    }
    auto* motion = new juce::DynamicObject();
    motion->setProperty("lanes", lanes);
    motion->setProperty("targets", targets);
    webView_.emitEventIfBrowserIsVisible("stasis_motion", juce::var(motion));

    // GRAIN: the grains that played since the last tick.
    stasis::GrainCloud::Event events[64];
    const int count = processor_.engine.grainEvents(events, 64, grainCursor_);
    if (count > 0) {
        juce::Array<juce::var> grains;
        for (int i = 0; i < count; ++i) {
            auto* g = new juce::DynamicObject();
            g->setProperty("st", events[i].semitones);
            g->setProperty("len", events[i].seconds);
            g->setProperty("pan", events[i].pan);
            g->setProperty("env", events[i].envelope);
            g->setProperty("level", events[i].level);
            grains.add(juce::var(g));
        }
        webView_.emitEventIfBrowserIsVisible("stasis_grains", grains);
    }
    if (++statusCounter_ % 10 == 0) {
        auto* status = new juce::DynamicObject();
        status->setProperty("cpu", processor_.cpuLoad.load() * 100.0f);
        status->setProperty("classes", int(processor_.engine.focusClasses()));
        status->setProperty("midi", int(processor_.engine.midiClasses()));
        status->setProperty("res", int(processor_.engine.resonatorClasses()));
        status->setProperty("sampleRate", processor_.getSampleRate());
        webView_.emitEventIfBrowserIsVisible("stasis_status", juce::var(status));
    }
}

// Spectrum of the wet output on a log-frequency axis, 0 (silence) .. ~1.2 (loud).
void StasisEditor::sendSpectrum() {
    const int size = stasis::Engine::kVisualSize;
    processor_.engine.copyVisual(samples_.data());
    for (int i = 0; i < size; ++i) spectrum_[size_t(i)] = samples_[size_t(i)] * window_[size_t(i)];
    fft_.forward(spectrum_);

    const double fs = std::max(1.0, processor_.getSampleRate());
    const auto binOf = [&](double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0) * size / fs; };
    juce::Array<juce::var> bands;
    for (int b = 0; b < kBands; ++b) {
        const double midi = 30.0 + 88.0 * b / (kBands - 1);
        const int lo = std::max(1, int(binOf(midi - 0.5))), hi = std::min(size / 2, std::max(lo, int(binOf(midi + 0.5))));
        float peak = 0.0f;
        for (int k = lo; k <= hi; ++k) peak = std::max(peak, std::abs(spectrum_[size_t(k)]));
        const float db = 20.0f * std::log10(peak * 4.0f / float(size) + 1.0e-9f);
        bands.add(juce::jlimit(0.0f, 1.25f, (db + 72.0f) / 56.0f));
    }
    webView_.emitEventIfBrowserIsVisible("stasis_spectrum", bands);
}
