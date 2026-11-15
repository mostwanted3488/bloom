#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine.h"

namespace bloom
{
// Identifiants des paramètres (les mêmes clés que l'interface web)
inline const char* const kFloatIds[] = { "attack", "release", "tone", "space", "shimmer", "width", "lowcut", "glide", "blend", "bloom", "bpm" };
inline const char* const kChoiceIds[] = { "sound", "layer", "key", "scale", "delay", "quant", "arp", "rate", "flower", "color", "preset" };
inline const char* const kBoolIds[] = { "snap", "hover", "midiout" };
}

class BloomProcessor : public juce::AudioProcessor
{
public:
    BloomProcessor();
    ~BloomProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // samples utilisateur
    bool loadSampleFile (const juce::File& file);
    bool loadSampleData (const juce::MemoryBlock& data, const juce::String& fileName);
    juce::String getSampleName() const;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    bloom::Engine engine;
    std::atomic<double> hostBpm { 0.0 };
    std::atomic<bool> hostPlaying { false }, hostRecording { false };

private:
    bool loadFromReader (std::unique_ptr<juce::AudioFormatReader> reader, const juce::String& fileName);
    float raw (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    juce::AudioFormatManager formats;
    juce::String samplePath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BloomProcessor)
};
