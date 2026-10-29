include(FetchContent)

# JUCE 8 (AGPLv3 / commercial). Set FETCHCONTENT_SOURCE_DIR_JUCE to reuse a local copy.
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Stasis
  PRODUCT_NAME "STASIS"
  COMPANY_NAME "Pando Studio"
  BUNDLE_ID "com.pandostudio.stasis"
  PLUGIN_MANUFACTURER_CODE Pndo
  PLUGIN_CODE Stsi
  FORMATS VST3 Standalone
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  NEEDS_WEBVIEW2 TRUE
  VST3_CATEGORIES Fx Reverb Delay
  COPY_PLUGIN_AFTER_BUILD FALSE)

# The UI: the mockup page and JUCE's JavaScript bridge, embedded in the binary.
juce_add_binary_data(StasisUI SOURCES
  ${PROJECT_SOURCE_DIR}/design/mockup.html
  ${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/index.js
  ${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/check_native_interop.js)

target_sources(Stasis PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_definitions(Stasis PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Stasis
  PRIVATE
    stasis_dsp
    StasisUI
    juce::juce_audio_utils
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Host-side stress test: loads the built VST3 like a DAW does.
juce_add_console_app(stasis_hosttest PRODUCT_NAME "stasis_hosttest")
target_sources(stasis_hosttest PRIVATE HostTest/main.cpp)
target_compile_definitions(stasis_hosttest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(stasis_hosttest PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
