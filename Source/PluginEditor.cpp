#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

namespace
{
    var obj (std::initializer_list<std::pair<const char*, var>> props)
    {
        auto* o = new DynamicObject();
        for (const auto& p : props) o->setProperty (p.first, p.second);
        return var (o);
    }
}

WebBrowserComponent::Options BloomEditor::makeOptions()
{
    return WebBrowserComponent::Options {}
        .withBackend (WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (WebBrowserComponent::Options::WinWebView2 {}
                                     .withUserDataFolder (File::getSpecialLocation (File::tempDirectory).getChildFile ("BLOOM_WebView2")))
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withEventListener ("params", [this] (var v) { handleParams (v); })
        .withEventListener ("note", [this] (var v)
        {
            bloom::UiEvent e;
            e.type = bloom::UiEvent::Note;
            e.midi = (int) v.getProperty ("midi", 60);
            e.vel = (float) (double) v.getProperty ("vel", .8);
            e.pan = (float) (double) v.getProperty ("pan", 0.0);
            e.delaySec = (float) (double) v.getProperty ("delay", 0.0);
            proc.engine.pushUiEvent (e);
        })
        .withEventListener ("key", [this] (var v)
        {
            bloom::UiEvent e;
            const bool down = (bool) v.getProperty ("down", false);
            e.type = down ? bloom::UiEvent::KeyDown : bloom::UiEvent::KeyUp;
            e.id = v.getProperty ("id", "").toString().hashCode();
            e.midi = (int) v.getProperty ("midi", 60);
            e.vel = (float) (double) v.getProperty ("vel", .8);
            proc.engine.pushUiEvent (e);
        })
        .withEventListener ("sustain", [this] (var v)
        {
            bloom::UiEvent e; e.type = bloom::UiEvent::Sustain; e.on = (bool) v.getProperty ("on", false);
            proc.engine.pushUiEvent (e);
        })
        .withEventListener ("load_sample", [this] (var)
        {
            chooser = std::make_unique<FileChooser> ("Load a sample", File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this] (const FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      if (f.existsAsFile() && proc.loadSampleFile (f)) sendSampleName();
                                  });
        })
        .withEventListener ("sample_data", [this] (var v)
        {
            MemoryBlock mb;
            if (mb.fromBase64Encoding (v.getProperty ("b64", "").toString())
                && proc.loadSampleData (mb, v.getProperty ("name", "sample").toString()))
                sendSampleName();
        })
        .withEventListener ("ready", [this] (var)
        {
            ready = true;
            sendSampleName();
            sendParams (true);
        })
        .withResourceProvider ([this] (const String& url) { return getResource (url); });
}

BloomEditor::BloomEditor (BloomProcessor& p)
    : AudioProcessorEditor (&p), proc (p), web (makeOptions())
{
    addAndMakeVisible (web);
    setResizable (true, true);
    setResizeLimits (432, 540, 1080, 1350);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio (540.0 / 675.0);
    setSize (540, 675);
    web.goToURL (WebBrowserComponent::getResourceProviderRoot());
    startTimerHz (30);
}

BloomEditor::~BloomEditor() { stopTimer(); }

std::optional<WebBrowserComponent::Resource> BloomEditor::getResource (const String& url)
{
    const auto path = url.fromFirstOccurrenceOf (WebBrowserComponent::getResourceProviderRoot(), false, false);
    if (path.isEmpty() || path == "/" || path.endsWithIgnoreCase ("index.html"))
    {
        std::vector<std::byte> data ((size_t) BinaryData::index_htmlSize);
        std::memcpy (data.data(), BinaryData::index_html, data.size());
        return WebBrowserComponent::Resource { std::move (data), "text/html" };
    }
    return std::nullopt;
}

//==============================================================================
void BloomEditor::handleParams (const var& v)
{
    if (auto* o = v.getDynamicObject())
        for (const auto& prop : o->getProperties())
        {
            const auto id = prop.name.toString();
            auto* param = proc.apvts.getParameter (id);
            if (param == nullptr) continue;
            const float plain = (float) (double) prop.value;
            const float norm = param->convertTo0to1 (plain);
            if (std::abs (norm - param->getValue()) > 1e-5f)
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (norm);
                param->endChangeGesture();
            }
            lastSent[id] = plain;
        }
}

void BloomEditor::sendParams (bool force)
{
    if (! ready) return;
    auto* o = new DynamicObject();
    bool changed = force;
    auto add = [&] (const char* id)
    {
        const float v = proc.apvts.getRawParameterValue (id)->load();
        o->setProperty (id, v);
        const auto it = lastSent.find (id);
        if (it == lastSent.end() || std::abs (it->second - v) > 1e-4f) changed = true;
        lastSent[id] = v;
    };
    for (auto* id : bloom::kFloatIds) add (id);
    for (auto* id : bloom::kChoiceIds) add (id);
    for (auto* id : bloom::kBoolIds) add (id);
    const var payload (o);
    if (changed) emit ("params", payload);
}

void BloomEditor::sendSampleName()
{
    if (! ready) return;
    const auto name = proc.getSampleName();
    if (name.isNotEmpty()) emit ("sample_loaded", obj ({ { "name", name } }));
}

void BloomEditor::timerCallback()
{
    if (! ready) return;
    bloom::Engine::Vis vis[64];
    const int n = proc.engine.popVisuals (vis, 64);
    for (int i = 0; i < n; ++i) emit ("note_vis", obj ({ { "midi", vis[i].midi }, { "vel", vis[i].vel } }));
    if (++tick % 6 == 0)
        emit ("host", obj ({ { "bpm", proc.hostBpm.load() }, { "playing", proc.hostPlaying.load() }, { "recording", proc.hostRecording.load() },
                             { "voices", proc.engine.getActiveVoices() } }));
    sendParams (false);   // automation de l'hôte → interface
}
