// BLOOM — moteur audio (portage C++ du moteur WebAudio de la preview)
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include "SoundBank.h"

namespace bloom
{

//==============================================================================
// Réglages lus à chaque bloc (copie des paramètres du plugin)
struct EngineParams
{
    float attack = 0, release = .3f, tone = .5f, space = .5f, shimmer = .25f, width = .35f,
          lowcut = .35f, glide = 0, blend = .5f, bloom = .82f, bpm = 140;
    int sound = 3, layer = -1, root = 11, scale = 0, delayDiv = 3, quant = 4, arp = 0, rate = 3;
    bool snap = false, midiOut = true;
};

// Événements envoyés par l'interface (thread message → thread audio)
struct UiEvent
{
    enum Type { Note, KeyDown, KeyUp, Sustain } type = Note;
    int midi = 60, id = 0;
    float vel = .8f, pan = 0, delaySec = 0;
    bool on = false;
};

// Sample utilisateur, partagé en lecture seule avec le thread audio
struct SampleData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100;
    juce::String name;
};

//==============================================================================
// Filtre à variables d'état (TPT), modulable sans artefacts
struct Svf
{
    float ic1 = 0, ic2 = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    void set (float fc, float q, float sr) noexcept
    {
        fc = juce::jlimit (10.0f, sr * 0.45f, fc);
        const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);
        k = 1.0f / juce::jmax (0.05f, q);
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = 0; }
    inline void tick (float v0, float& lp, float& bp, float& hp) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        lp = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
    inline float lp (float x) noexcept { float l, b, h; tick (x, l, b, h); return l; }
    inline float bp (float x) noexcept { float l, b, h; tick (x, l, b, h); return b * k; }   // gain 0 dB au pic
    inline float hp (float x) noexcept { float l, b, h; tick (x, l, b, h); return h; }
};

//==============================================================================
struct Osc
{
    double ph = 0;
    float r = 1, g0 = 0, gain = 0, decMul = 1, det = 0;
    int decLeft = 0, wave = 0;
    bool fm = false;
};

struct Part
{
    const SoundDef* P = nullptr;
    std::shared_ptr<const SampleData> smp;
    bool active = false, hold = false;
    float f = 440, fTarget = 440, glideCoef = 1, vel = .8f, level = 1, gL = .7f, gR = .7f;
    std::array<Osc, 8> osc; int nOsc = 0;
    std::array<Osc, 2> shim; std::array<float, 2> shimGain {}, shimPk {}, shimDecMul {}; int nShim = 0;
    int shimRise = 1, shimDecStart = 0;
    // FM
    bool hasFm = false; double fmPh = 0; float fmR = 1, fmIdx0 = 0, fmIdx1 = 0; int fmDecS = 1;
    // vibrato propre au son
    bool hasVib = false; double vibPh = 0; float vibRate = 5, vibDepth = 0;
    // filtres
    Svf lp; float fc0 = 1000, fc1 = 1000, q = .76f; int ftS = 1; bool fixedCut = false;
    std::array<Svf, 3> form; std::array<float, 3> formG {}; int nForm = 0;
    bool hasNoise = false; Svf nbp; float noiseG = 0, noiseR = 1, noiseQ = 10;
    int clickLeft = 0, clickLen = 1; float clickAmp = 0; Svf clickHp;
    // sample
    double smpPos = 0; float smpRate = 1, smpRateTarget = 1;
    // enveloppe d'amplitude
    enum Stage { Atk, Sus, Dec, Rel, Done } st = Done;
    float env = 0, peak = 0, atkInc = 0, susLevel = 0, susCoef = 0, decMul = 1, relCoef = 0, shimRelCoef = 0;
    int t = 0, endAt = 0;
};

struct Voice
{
    std::array<Part, 2> parts; int nParts = 0;
    int midi = 60, delay = 0;
    bool active = false;
    uint32_t serial = 0;
    uint64_t age = 0;
};

//==============================================================================
class Engine
{
public:
    Engine();
    void prepare (double sampleRate, int maxBlock);
    void reset();

    // thread audio
    void render (juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, const EngineParams& p,
                 double hostBpm, bool hostPlaying, double hostPpq, bool hostHasPpq);

    // thread message
    void pushUiEvent (const UiEvent& e);
    void setSample (std::shared_ptr<const SampleData> s) { std::atomic_store (&userSample, std::move (s)); }
    std::shared_ptr<const SampleData> getSample() const { return std::atomic_load (&userSample); }

    // notes à afficher sur la fleur (thread audio → interface)
    struct Vis { int midi; float vel; };
    int popVisuals (Vis* dest, int max);
    int getActiveVoices() const noexcept { return activeVoices.load(); }

    // notes jouées dans le plugin (souris / clavier / arp) renvoyées à l'hôte en MIDI
    const juce::MidiBuffer& getOutMidi() const noexcept { return outMidi; }

    static constexpr int kSoundCount = 19;     // + 1 emplacement « sample »
    const std::vector<SoundDef>& sounds() const { return bank; }

private:
    static constexpr int kMaxVoices = 40, kSub = 32;
    std::vector<SoundDef> bank;
    SoundDef sampleDef;
    std::shared_ptr<const SampleData> userSample;

    double sr = 48000;
    std::array<Voice, kMaxVoices> voices;
    uint32_t nextSerial = 1; uint64_t ageCounter = 0;
    std::atomic<int> activeVoices { 0 };
    EngineParams P;

    // modulation globale
    double modPh = 0; float bendCents = 0, modWheel = 0;
    juce::Random rng;

    // horloge musicale
    double beatPos = 0, lastUiSlot = -1;

    // notes tenues
    struct Held { int id = 0, midi = 0; uint32_t serial = 0; bool used = false; };
    std::array<Held, 64> held {};
    struct ArpNote { int id = 0, midi = 0; float vel = .8f; bool up = false; };
    std::array<ArpNote, 32> arpNotes {}; int nArp = 0, arpI = 0;
    struct Mono { int id = 0, midi = 0; };
    std::array<Mono, 32> monoStack {}; int nMono = 0; uint32_t monoSerial = 0;
    bool sustain = false;
    std::array<uint32_t, 128> sustained {}; int nSustained = 0;

    // file d'événements interface
    juce::AbstractFifo uiFifo { 512 };
    std::array<UiEvent, 512> uiBuf;
    juce::AbstractFifo visFifo { 256 };
    std::array<Vis, 256> visBuf;

    // sortie MIDI + filtre anti-écho (quand l'hôte renvoie nos propres notes)
    juce::MidiBuffer outMidi;
    struct PendingOut { int samples = 0, midi = 60; float vel = 0; bool on = false; };
    std::array<PendingOut, 512> pending {}; int nPending = 0;
    struct UiHeld { int id = 0, midi = 0; bool used = false; };
    std::array<UiHeld, 32> uiHeld {};
    std::array<int, 128> echoOn {}, echoOff {};
    std::array<double, 128> echoTime {};
    double sampleClock = 0;
    void queueOut (int delaySamples, int midi, float vel, bool on);
    void flushOut (int n);

    // effets globaux
    juce::AudioBuffer<float> busL, shimBus, work;
    std::array<Svf, 2> hpf; Svf shimHp;
    juce::dsp::Convolution reverb { juce::dsp::Convolution::NonUniform { 512 } };
    juce::dsp::Convolution shimVerb { juce::dsp::Convolution::NonUniform { 512 } };
    juce::AudioBuffer<float> verbIn, shimIn;
    std::vector<float> dlyL, dlyR; int dlyW = 0; float dlyTime = .375f; std::array<Svf, 2> dlyLp;
    std::vector<float> chBuf; int chW = 0; double chPh1 = 0, chPh2 = 0;
    juce::dsp::Compressor<float> comp; juce::dsp::Limiter<float> lim;
    juce::SmoothedValue<float> wetS, dlyWetS, widthS;

    // internes
    const SoundDef* soundAt (int idx) const;
    void handleUi (const UiEvent& e, int offset);
    void keyDown (int id, int midi, float vel, int offset);
    void keyUp (int id);
    void setSustain (bool on);
    Voice* startNote (int midi, float vel, float pan, bool hold, int delaySamples, bool visual);
    void initPart (Part& pt, const SoundDef* S, int midi, float vel, float pan, bool hold, float level);
    void releaseVoice (Voice& v, bool fast);
    void retune (Voice& v, int midi, float timeSec);
    Voice* findVoice (uint32_t serial);
    int snapMidi (int m) const;
    void arpTick (int offset, double stepSamples);
    void renderVoices (int start, int num);
    void pushVisual (int midi, float vel);
    void buildIRs();
};

} // namespace bloom
