#include "PluginEditor.h"

#include <BinaryData.h>

namespace {

constexpr int kWidth = 1340, kHeight = 934;  // the page's own layout size, scaled to fit
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
            case Kind::Float: r.sliders.push_back(std::make_unique<juce::WebSliderRelay>(s.id)); break;
            case Kind::Bool: r.toggles.push_back(std::make_unique<juce::WebToggleButtonRelay>(s.id)); break;
            case Kind::Choice: r.combos.push_back(std::make_unique<juce::WebComboBoxRelay>(s.id)); break;
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
                       .withResourceProvider(getResource);
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
        auto& param = *p.apvts.getParameter(s.id);
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

    addAndMakeVisible(webView_);
    webView_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable(true, true);
    setResizeLimits(kWidth / 2, kHeight / 2, kWidth * 3 / 2, kHeight * 3 / 2);
    getConstrainer()->setFixedAspectRatio(double(kWidth) / kHeight);
    setSize(kWidth * 4 / 5, kHeight * 4 / 5);
    startTimerHz(30);
}

StasisEditor::~StasisEditor() { stopTimer(); }

void StasisEditor::resized() { webView_.setBounds(getLocalBounds()); }

void StasisEditor::timerCallback() {
    sendSpectrum();
    if (++statusCounter_ % 10 == 0) {
        auto* status = new juce::DynamicObject();
        status->setProperty("cpu", processor_.cpuLoad.load() * 100.0f);
        status->setProperty("classes", int(processor_.engine.focusClasses()));
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
