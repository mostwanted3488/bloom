#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

namespace
{
    const char* const kDelays[] = { "OFF", "1/4", "1/4D", "1/8D", "1/8", "1/8T", "1/16" };
    const char* const kQuants[] = { "OFF", "1/4", "1/8", "1/8T", "1/16", "1/32" };
    const char* const kArps[]   = { "OFF", "UP", "DOWN", "UP-DOWN", "RANDOM", "ORDER", "CHORD" };
    const char* const kRates[]  = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    const char* const kRoots[]  = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char* const kScales[] = { "MINOR", "MAJOR", "DORIAN", "PHRYGIAN", "HARM MINOR", "LYDIAN", "PENTA MIN", "PENTA MAJ", "HIRAJOSHI", "IN-SEN" };

    template <size_t N> StringArray arr (const char* const (&a)[N]) { StringArray s; for (auto* x : a) s.add (x); return s; }
}

AudioProcessorValueTreeState::ParameterLayout BloomProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto addF = [&] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def));
    };
    addF ("attack", "Attack", 0.0f);   addF ("release", "Release", .3f);  addF ("tone", "Tone", .5f);
    addF ("space", "Space", .5f);      addF ("shimmer", "Shimmer", .25f); addF ("width", "Width", .35f);
    addF ("lowcut", "Low Cut", .35f);  addF ("glide", "Glide", 0.0f);     addF ("blend", "Blend", .5f);
    addF ("bloom", "Bloom", .82f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "bpm", 1 }, "BPM (no host tempo)",
                                                       NormalisableRange<float> (40.0f, 240.0f, 1.0f), 140.0f));

    StringArray sounds;
    for (const auto& s : bloom::makeSoundBank()) sounds.add (s.name);
    sounds.add ("SAMPLE");
    StringArray layers { "OFF" }; layers.addArray (sounds);

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "sound", 1 }, "Sound", sounds, 3));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "layer", 1 }, "Layer", layers, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", arr (kRoots), 11));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", arr (kScales), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "delay", 1 }, "Delay", arr (kDelays), 3));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "quant", 1 }, "Quantize", arr (kQuants), 4));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "arp", 1 }, "Arp", arr (kArps), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "rate", 1 }, "Arp Rate", arr (kRates), 3));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "flower", 1 }, "Flower", arr (bloom::kFlowerNames), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "color", 1 }, "Color", arr (bloom::kColorNames), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "preset", 1 }, "Preset", arr (bloom::kPresetNames), 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "snap", 1 }, "Snap to Scale", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "hover", 1 }, "Hover Play", true));
    return layout;
}

BloomProcessor::BloomProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BLOOM", createLayout())
{
    formats.registerBasicFormats();
}

bool BloomProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == AudioChannelSet::stereo() || out == AudioChannelSet::mono();
}

void BloomProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, jmax (32, samplesPerBlock));
}

void BloomProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    double bpm = 0, ppq = 0; bool playing = false, hasPpq = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            if (const auto b = pos->getBpm()) bpm = *b;
            playing = pos->getIsPlaying();
            if (const auto q = pos->getPpqPosition()) { ppq = *q; hasPpq = true; }
        }
    hostBpm.store (bpm); hostPlaying.store (playing);

    bloom::EngineParams p;
    p.attack = raw ("attack"); p.release = raw ("release"); p.tone = raw ("tone"); p.space = raw ("space");
    p.shimmer = raw ("shimmer"); p.width = raw ("width"); p.lowcut = raw ("lowcut"); p.glide = raw ("glide");
    p.blend = raw ("blend"); p.bloom = raw ("bloom"); p.bpm = raw ("bpm");
    p.sound = (int) raw ("sound"); p.layer = (int) raw ("layer") - 1; p.root = (int) raw ("key");
    p.scale = (int) raw ("scale"); p.delayDiv = (int) raw ("delay"); p.quant = (int) raw ("quant");
    p.arp = (int) raw ("arp"); p.rate = (int) raw ("rate"); p.snap = raw ("snap") > .5f;

    buffer.clear();
    if (buffer.getNumChannels() >= 2)
        engine.render (buffer, midi, p, bpm, playing, ppq, hasPpq);
    else
    {
        AudioBuffer<float> st (2, buffer.getNumSamples()); st.clear();
        engine.render (st, midi, p, bpm, playing, ppq, hasPpq);
        buffer.copyFrom (0, 0, st, 0, 0, buffer.getNumSamples());
        buffer.addFrom (0, 0, st, 1, 0, buffer.getNumSamples());
        buffer.applyGain (.5f);
    }
}

AudioProcessorEditor* BloomProcessor::createEditor() { return new BloomEditor (*this); }

//==============================================================================
void BloomProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("samplePath", samplePath, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void BloomProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = ValueTree::fromXml (*xml);
            const String path = tree.getProperty ("samplePath").toString();
            apvts.replaceState (tree);
            if (path.isNotEmpty() && File (path).existsAsFile()) loadSampleFile (File (path));
        }
}

//==============================================================================
bool BloomProcessor::loadFromReader (std::unique_ptr<AudioFormatReader> reader, const String& fileName)
{
    if (reader == nullptr) return false;
    const auto maxLen = (int64) (reader->sampleRate * 30.0);
    const int len = (int) jmin (reader->lengthInSamples, maxLen);
    if (len <= 0) return false;
    auto data = std::make_shared<bloom::SampleData>();
    data->buffer.setSize ((int) jlimit (1u, 2u, reader->numChannels), len);
    reader->read (&data->buffer, 0, len, 0, true, data->buffer.getNumChannels() > 1);
    data->sampleRate = reader->sampleRate;
    data->name = "SMP " + File::createFileWithoutCheckingPath (fileName).getFileNameWithoutExtension().toUpperCase().substring (0, 9);
    engine.setSample (data);
    return true;
}

bool BloomProcessor::loadSampleFile (const File& file)
{
    if (! loadFromReader (std::unique_ptr<AudioFormatReader> (formats.createReaderFor (file)), file.getFileName())) return false;
    samplePath = file.getFullPathName();
    return true;
}

bool BloomProcessor::loadSampleData (const MemoryBlock& data, const String& fileName)
{
    auto stream = std::make_unique<MemoryInputStream> (data, true);
    if (! loadFromReader (std::unique_ptr<AudioFormatReader> (formats.createReaderFor (std::move (stream))), fileName)) return false;
    samplePath = {};
    return true;
}

String BloomProcessor::getSampleName() const
{
    const auto s = engine.getSample();
    return s != nullptr ? s->name : String();
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BloomProcessor(); }
