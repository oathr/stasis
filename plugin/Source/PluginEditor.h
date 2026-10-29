#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

// The UI is the HTML page of design/mockup.html, shown in the system web view
// (WebKit on macOS, WebView2 on Windows). Every parameter has a relay: the page and
// the host stay in sync both ways (automation, undo, presets).
class StasisEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit StasisEditor(StasisProcessor&);
    ~StasisEditor() override;

    void resized() override;

private:
    struct Relays {
        std::vector<std::unique_ptr<juce::WebSliderRelay>> sliders;
        std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggles;
        std::vector<std::unique_ptr<juce::WebComboBoxRelay>> combos;
    };
    static Relays createRelays();
    static juce::WebBrowserComponent::Options createOptions(Relays&);

    void timerCallback() override;
    void sendSpectrum();

    StasisProcessor& processor_;
    Relays relays_;                        // must exist before the web view
    juce::WebBrowserComponent webView_;
    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>> sliderAttachments_;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAttachments_;
    std::vector<std::unique_ptr<juce::WebComboBoxParameterAttachment>> comboAttachments_;

    std::vector<float> samples_, window_;
    std::vector<stasis::Fft::Complex> spectrum_;
    stasis::Fft fft_;
    int statusCounter_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StasisEditor)
};
