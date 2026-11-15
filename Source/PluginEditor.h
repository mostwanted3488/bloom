#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// L'interface est la page HTML de la preview, intégrée dans une WebView.
// Le son, lui, est entièrement produit par le moteur C++ (bloom::Engine).
class BloomEditor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    explicit BloomEditor (BloomProcessor&);
    ~BloomEditor() override;

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    void resized() override { web.setBounds (getLocalBounds()); }

private:
    void timerCallback() override;
    void handleParams (const juce::var& v);
    void sendParams (bool force);
    void sendSampleName();
    void emit (const juce::Identifier& id, const juce::var& v) { web.emitEventIfBrowserIsVisible (id, v); }
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::WebBrowserComponent::Options makeOptions();

    BloomProcessor& proc;
    juce::WebBrowserComponent web;
    std::map<juce::String, float> lastSent;
    std::unique_ptr<juce::FileChooser> chooser;
    bool ready = false;
    int tick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BloomEditor)
};
