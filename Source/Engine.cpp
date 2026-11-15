#include "Engine.h"
#include <algorithm>

namespace bloom
{

//==============================================================================
namespace
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    const int kScales[10][8] = {               // dernier élément = nombre de notes
        { 0, 2, 3, 5, 7, 8, 10, 7 },   // MINOR
        { 0, 2, 4, 5, 7, 9, 11, 7 },   // MAJOR
        { 0, 2, 3, 5, 7, 9, 10, 7 },   // DORIAN
        { 0, 1, 3, 5, 7, 8, 10, 7 },   // PHRYGIAN
        { 0, 2, 3, 5, 7, 8, 11, 7 },   // HARM MINOR
        { 0, 2, 4, 6, 7, 9, 11, 7 },   // LYDIAN
        { 0, 3, 5, 7, 10, 0, 0, 5 },   // PENTA MIN
        { 0, 2, 4, 7, 9, 0, 0, 5 },    // PENTA MAJ
        { 0, 2, 3, 7, 8, 0, 0, 5 },    // HIRAJOSHI
        { 0, 1, 5, 7, 10, 0, 0, 5 },   // IN-SEN
    };
    const double kDelayBeats[] = { 0, 1, 1.5, .75, .5, 1.0 / 3, .25 };      // OFF 1/4 1/4D 1/8D 1/8 1/8T 1/16
    const double kQuantBeats[] = { 0, 1, .5, 1.0 / 3, .25, .125 };          // OFF 1/4 1/8 1/8T 1/16 1/32
    const double kRateBeats[]  = { 1, .5, 1.0 / 3, .25, 1.0 / 6, .125 };    // 1/4 1/8 1/8T 1/16 1/16T 1/32

    inline float mtof (float m) noexcept { return 440.0f * std::exp2 ((m - 69.0f) / 12.0f); }
    inline float coefFor (double seconds, double sr) noexcept { return 1.0f - (float) std::exp (-1.0 / juce::jmax (1.0, seconds * sr)); }

    inline float polyBlep (double t, double dt) noexcept
    {
        if (dt <= 0) return 0;
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0;
    }
    inline float wave (int w, double ph, double dt) noexcept
    {
        switch (w)
        {
            case 0:  return std::sin (twoPi * (float) ph);
            case 1:  return 1.0f - 4.0f * std::abs ((float) ph - 0.5f) ;
            case 2:  return (float) (2.0 * ph - 1.0) - polyBlep (ph, dt);
            default:
            {
                double p2 = ph + 0.5; if (p2 >= 1.0) p2 -= 1.0;
                return (ph < 0.5 ? 1.0f : -1.0f) + polyBlep (ph, dt) - polyBlep (p2, dt);
            }
        }
    }

    // Réponse impulsionnelle façon preview + normalisation identique à WebAudio (ConvolverNode)
    juce::AudioBuffer<float> makeIR (double sr, double sec, bool bright, juce::Random& r)
    {
        const int len = (int) (sr * sec);
        juce::AudioBuffer<float> b (2, len);
        double power = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            float lp = 0;
            for (int i = 0; i < len; ++i)
            {
                const float tt = (float) i / (float) len;
                const float kk = bright ? .05f + .4f * tt : .25f + .7f * tt;
                lp += (r.nextFloat() * 2 - 1 - lp) * (1 - kk);
                d[i] = (i < sr * .012 ? 0.0f : lp) * std::pow (1 - tt, bright ? 1.6f : 2.4f) * 1.6f;
                power += (double) d[i] * d[i];
            }
        }
        power = std::sqrt (power / (2.0 * len));
        power = juce::jmax (power, 0.000125);
        const float scale = (float) (1.0 / power * 0.00125 * 44100.0 / sr);
        b.applyGain (scale);
        return b;
    }
}

//==============================================================================
Engine::Engine() : bank (makeSoundBank())
{
    sampleDef = SoundDef { "SAMPLE", "SAMPLE", 1.4f, 0, {}, false, 0, 0, 0, false, 0, 0, {}, 0,
                           false, 1, 0, 0, .003f, 6.0f, .4f, 1.0f, .35f, 0, 0, 0, 0, .1f, .6f, 1.0f, 1.0f };
    rng.setSeed (0x5eed);
}

void Engine::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    busL.setSize (2, maxBlock); shimBus.setSize (1, maxBlock);
    verbIn.setSize (2, maxBlock); shimIn.setSize (2, maxBlock); work.setSize (2, maxBlock);
    dlyL.assign ((size_t) (sr * 4.1), 0); dlyR.assign ((size_t) (sr * 4.1), 0); dlyW = 0;
    chBuf.assign ((size_t) (sr * 0.05), 0); chW = 0;
    for (auto& f : dlyLp) { f.set (3200, .707f, (float) sr); f.reset(); }
    shimHp.set (700, .707f, (float) sr);

    buildIRs();
    const juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, 2 };
    reverb.prepare (spec); shimVerb.prepare (spec);
    comp.setThreshold (-16); comp.setRatio (3); comp.setAttack (4); comp.setRelease (200); comp.prepare (spec);
    lim.setThreshold (-2); lim.setRelease (80); lim.prepare (spec);
    wetS.reset (sr, .08); dlyWetS.reset (sr, .08); widthS.reset (sr, .05);
    reset();
}

void Engine::buildIRs()
{
    juce::Random r (1234);
    reverb.loadImpulseResponse (makeIR (sr, 4.4, false, r), sr, juce::dsp::Convolution::Stereo::yes,
                                juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    shimVerb.loadImpulseResponse (makeIR (sr, 7.0, true, r), sr, juce::dsp::Convolution::Stereo::yes,
                                  juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
}

void Engine::reset()
{
    for (auto& v : voices) v.active = false;
    for (auto& f : hpf) f.reset();
    nArp = nMono = nSustained = 0; sustain = false;
    for (auto& h : held) h.used = false;
    reverb.reset(); shimVerb.reset(); comp.reset(); lim.reset();
    std::fill (dlyL.begin(), dlyL.end(), 0.0f); std::fill (dlyR.begin(), dlyR.end(), 0.0f);
    std::fill (chBuf.begin(), chBuf.end(), 0.0f);
}

//==============================================================================
void Engine::pushUiEvent (const UiEvent& e)
{
    const auto scope = uiFifo.write (1);
    if (scope.blockSize1 > 0) uiBuf[(size_t) scope.startIndex1] = e;
    else if (scope.blockSize2 > 0) uiBuf[(size_t) scope.startIndex2] = e;
}

void Engine::pushVisual (int midi, float vel)
{
    const auto scope = visFifo.write (1);
    if (scope.blockSize1 > 0) visBuf[(size_t) scope.startIndex1] = { midi, vel };
    else if (scope.blockSize2 > 0) visBuf[(size_t) scope.startIndex2] = { midi, vel };
}

int Engine::popVisuals (Vis* dest, int max)
{
    const auto scope = visFifo.read (juce::jmin (max, visFifo.getNumReady()));
    int n = 0;
    for (int i = 0; i < scope.blockSize1; ++i) dest[n++] = visBuf[(size_t) (scope.startIndex1 + i)];
    for (int i = 0; i < scope.blockSize2; ++i) dest[n++] = visBuf[(size_t) (scope.startIndex2 + i)];
    return n;
}

//==============================================================================
const SoundDef* Engine::soundAt (int idx) const
{
    if (idx >= 0 && idx < (int) bank.size()) return &bank[(size_t) idx];
    if (idx == kSoundCount && getSample() != nullptr) return &sampleDef;
    return nullptr;
}

int Engine::snapMidi (int m) const
{
    const auto* s = kScales[juce::jlimit (0, 9, P.scale)];
    int best = m, bd = 99;
    for (int d = -6; d <= 6; ++d)
    {
        const int pc = (((m + d - P.root) % 12) + 12) % 12;
        for (int k = 0; k < s[7]; ++k)
            if (s[k] == pc && std::abs (d) < bd) { bd = std::abs (d); best = m + d; }
    }
    return best;
}

Voice* Engine::findVoice (uint32_t serial)
{
    if (serial == 0) return nullptr;
    for (auto& v : voices) if (v.active && v.serial == serial) return &v;
    return nullptr;
}

void Engine::initPart (Part& pt, const SoundDef* S, int midi, float vel, float pan, bool hold, float level)
{
    const float fsr = (float) sr;
    pt = Part {};
    pt.P = S; pt.active = true; pt.hold = hold; pt.vel = vel; pt.level = level;
    if (S == &sampleDef)
    {
        pt.smp = getSample();
        if (pt.smp == nullptr) { pt.active = false; return; }
    }
    pt.f = pt.fTarget = mtof ((float) (midi + S->oct));
    const float a = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    pt.gL = std::cos (a); pt.gR = std::sin (a);

    const float atk = S->a + P.attack * P.attack * 2.5f, mul = .3f + P.release * 2.33f;
    const float baseLen = pt.smp != nullptr ? juce::jmin (6.0f, (float) (pt.smp->buffer.getNumSamples() / pt.smp->sampleRate)) : S->len;
    const float len = baseLen * mul, rel = S->rel * mul;
    const float tm = std::exp2 ((P.tone - .5f) * 3.0f), br = .5f + P.bloom * .8f;

    pt.peak = .3f * vel * S->gain * level;
    pt.atkInc = pt.peak / juce::jmax (1.0f, atk * fsr);
    pt.susLevel = pt.peak * S->hs;
    pt.susCoef = coefFor (S->hd / 3.0, sr);
    pt.decMul = std::pow (0.0008f, 1.0f / juce::jmax (1.0f, len * fsr));
    pt.relCoef = coefFor (rel / 2.5, sr);
    pt.shimRelCoef = coefFor (rel * 1.6 / 2.5, sr);
    pt.endAt = (int) ((atk + len * 1.3f + .2f) * fsr);
    pt.st = Part::Atk; pt.env = 0;

    pt.q = std::pow (10.0f, S->q / 20.0f);
    if (pt.smp != nullptr) { pt.fixedCut = true; pt.lp.set (juce::jmin (18000.0f, 14000.0f * tm), pt.q, fsr); }
    else
    {
        pt.fc0 = juce::jmin (18000.0f, pt.f * (S->f0a + S->f0b * br) * tm);
        pt.fc1 = juce::jlimit (60.0f, 16000.0f, pt.f * (S->f1a + S->f1b * br) * tm);
        pt.ftS = juce::jmax (1, (int) (S->ft * fsr));
        pt.lp.set (pt.fc0, pt.q, fsr);
    }

    pt.nForm = (int) juce::jmin ((size_t) 3, S->formants.size());
    for (int k = 0; k < pt.nForm; ++k) { pt.form[(size_t) k].set (S->formants[(size_t) k].first, 7.0f, fsr); pt.formG[(size_t) k] = S->formants[(size_t) k].second * 2.4f; }

    pt.nOsc = 0;
    bool anyFm = false;
    for (const auto& pp : S->p)
    {
        if (pt.nOsc >= (int) pt.osc.size()) break;
        auto& o = pt.osc[(size_t) pt.nOsc++];
        o.wave = pp.wave; o.r = pp.r;
        o.g0 = o.gain = pp.g * (pp.bl ? (.5f + P.bloom) : 1.0f);
        o.decLeft = pp.dec > 0 ? (int) (pp.dec * fsr) : 0;
        o.decMul = pp.dec > 0 ? std::pow (0.001f, 1.0f / juce::jmax (1.0f, pp.dec * fsr)) : 1.0f;
        o.det = std::exp2 (pp.det / 1200.0f);
        o.fm = pp.fm; anyFm |= pp.fm;
        o.ph = 0;
    }
    pt.hasFm = S->hasFm && anyFm;
    if (pt.hasFm)
    {
        pt.fmR = S->fmR; pt.fmIdx0 = pt.f * S->fmI * vel; pt.fmIdx1 = pt.f * .02f + .01f;
        pt.fmDecS = juce::jmax (1, (int) (S->fmDec * fsr));
    }
    pt.hasVib = S->hasVib; pt.vibRate = S->vibRate; pt.vibDepth = S->vibDepth;
    pt.hasNoise = S->hasNoise;
    if (pt.hasNoise) { pt.noiseR = S->noiseR; pt.noiseQ = S->noiseQ; pt.noiseG = S->noiseG; pt.nbp.set (pt.f * pt.noiseR, pt.noiseQ, fsr); }
    if (S->click > 0)
    {
        pt.clickLen = pt.clickLeft = juce::jmax (1, (int) (.05f * fsr));
        pt.clickAmp = S->click * vel;
        pt.clickHp.set (2500, .707f, fsr);
    }

    // shimmer : octaves fantômes vers la réverbe shimmer
    pt.nShim = 0;
    if (P.shimmer > .01f && pt.smp == nullptr)
    {
        const float rs[2] = { 2.0f, 4.0f }, gs[2] = { 1.0f, .45f };
        for (int k = 0; k < 2; ++k)
        {
            auto& o = pt.shim[(size_t) k];
            o = Osc {}; o.r = rs[k]; o.wave = 0;
            pt.shimPk[(size_t) k] = .06f * vel * P.shimmer * gs[k] * level;
            pt.shimGain[(size_t) k] = 0;
            const float decDur = (atk + len * 1.3f) - (.25f + atk);
            pt.shimDecMul[(size_t) k] = (! hold && decDur > .05f) ? std::pow (0.001f, 1.0f / (decDur * fsr)) : 1.0f;
            ++pt.nShim;
        }
        pt.shimRise = juce::jmax (1, (int) ((.25f + atk) * fsr));
        pt.shimDecStart = pt.shimRise;
    }
    if (pt.smp != nullptr) pt.smpRate = pt.smpRateTarget = std::exp2 ((float) (midi - 60) / 12.0f);
}

Voice* Engine::startNote (int midi, float vel, float pan, bool hold, int delaySamples, bool visual)
{
    const SoundDef* S = soundAt (P.sound);
    if (S == nullptr) S = &bank[3];
    Voice* v = nullptr;
    for (auto& cand : voices) if (! cand.active) { v = &cand; break; }
    if (v == nullptr)
    {
        v = &voices[0];
        for (auto& cand : voices) if (cand.age < v->age) v = &cand;
    }
    v->active = true; v->midi = midi; v->delay = juce::jmax (0, delaySamples);
    v->serial = nextSerial++; if (nextSerial == 0) nextSerial = 1;
    v->age = ageCounter++;
    const SoundDef* L = soundAt (P.layer);
    const bool lay = L != nullptr && P.layer != P.sound;
    v->nParts = lay ? 2 : 1;
    initPart (v->parts[0], S, midi, vel, pan, hold, lay ? 1.0f - P.blend * .35f : 1.0f);
    if (lay) initPart (v->parts[1], L, midi, vel, -pan * .5f, hold, P.blend * .8f);
    if (visual) pushVisual (midi, vel);
    return v;
}

void Engine::releaseVoice (Voice& v, bool fast)
{
    for (int i = 0; i < v.nParts; ++i)
    {
        auto& pt = v.parts[(size_t) i];
        if (! pt.active) continue;
        if (fast) { pt.relCoef = coefFor (.02 / 2.5, sr); pt.shimRelCoef = pt.relCoef; }
        pt.st = Part::Rel; pt.hold = false;
    }
}

void Engine::retune (Voice& v, int midi, float timeSec)
{
    v.midi = midi;
    const float c = coefFor (juce::jmax (.005f, timeSec / 3.0f), sr);
    for (int i = 0; i < v.nParts; ++i)
    {
        auto& pt = v.parts[(size_t) i];
        pt.fTarget = mtof ((float) (midi + pt.P->oct));
        pt.smpRateTarget = std::exp2 ((float) (midi - 60) / 12.0f);
        pt.glideCoef = c;
    }
}

//==============================================================================
void Engine::keyDown (int id, int midi, float vel, int)
{
    if (P.snap) midi = snapMidi (midi);
    const float pan = (rng.nextFloat() - .5f) * .6f;
    if (P.arp > 0)
    {
        for (int i = 0; i < nArp; ++i) if (arpNotes[(size_t) i].id == id) { arpNotes[(size_t) i].up = false; return; }
        if (nArp < (int) arpNotes.size()) arpNotes[(size_t) nArp++] = { id, midi, vel, false };
        pushVisual (midi, vel * .5f);
        return;
    }
    if (P.glide > .02f)
    {
        if (nMono < (int) monoStack.size()) monoStack[(size_t) nMono++] = { id, midi };
        if (auto* v = findVoice (monoSerial)) { retune (*v, midi, P.glide * .4f); pushVisual (midi, vel); }
        else if (auto* nv = startNote (midi, vel, pan, true, 0, true)) monoSerial = nv->serial;
        return;
    }
    for (auto& h : held)
        if (h.used && h.id == id) { if (auto* v = findVoice (h.serial)) releaseVoice (*v, true); h.used = false; }
    if (auto* v = startNote (midi, vel, pan, true, 0, true))
        for (auto& h : held) if (! h.used) { h = { id, midi, v->serial, true }; break; }
}

void Engine::keyUp (int id)
{
    auto relOrSus = [this] (uint32_t serial)
    {
        if (serial == 0) return;
        if (sustain) { if (nSustained < (int) sustained.size()) sustained[(size_t) nSustained++] = serial; }
        else if (auto* v = findVoice (serial)) releaseVoice (*v, false);
    };
    for (int i = 0; i < nArp; ++i)
        if (arpNotes[(size_t) i].id == id)
        {
            if (sustain) arpNotes[(size_t) i].up = true;
            else { for (int k = i; k < nArp - 1; ++k) arpNotes[(size_t) k] = arpNotes[(size_t) k + 1]; --nArp; }
            break;
        }
    for (int i = 0; i < nMono; ++i)
        if (monoStack[(size_t) i].id == id)
        {
            const bool wasTop = i == nMono - 1;
            for (int k = i; k < nMono - 1; ++k) monoStack[(size_t) k] = monoStack[(size_t) k + 1];
            --nMono;
            if (nMono == 0) { relOrSus (monoSerial); monoSerial = 0; }
            else if (wasTop) if (auto* v = findVoice (monoSerial)) retune (*v, monoStack[(size_t) nMono - 1].midi, P.glide * .4f);
            break;
        }
    for (auto& h : held)
        if (h.used && h.id == id) { relOrSus (h.serial); h.used = false; }
}

void Engine::setSustain (bool on)
{
    sustain = on;
    if (on) return;
    for (int i = 0; i < nSustained; ++i) if (auto* v = findVoice (sustained[(size_t) i])) releaseVoice (*v, false);
    nSustained = 0;
    int w = 0;
    for (int i = 0; i < nArp; ++i) if (! arpNotes[(size_t) i].up) arpNotes[(size_t) w++] = arpNotes[(size_t) i];
    nArp = w;
}

void Engine::handleUi (const UiEvent& e, int)
{
    switch (e.type)
    {
        case UiEvent::Note:
        {
            const double spb = sr * 60.0 / juce::jmax (20.0f, P.bpm);
            double qd = 0;
            if (P.quant > 0)
            {
                const double step = kQuantBeats[juce::jlimit (0, 5, P.quant)];
                const double next = std::ceil (beatPos / step - 1e-9) * step;
                if (e.delaySec <= 0)
                {
                    if (std::abs (next - lastUiSlot) < 1e-9) return;   // une note par case de grille
                    lastUiSlot = next;
                }
                qd = (next - beatPos) * spb;
            }
            startNote (e.midi, e.vel, e.pan, false, (int) (qd + e.delaySec * sr), false);
            break;
        }
        case UiEvent::KeyDown: keyDown (e.id, e.midi, e.vel, 0); break;
        case UiEvent::KeyUp:   keyUp (e.id); break;
        case UiEvent::Sustain: setSustain (e.on); break;
    }
}

void Engine::arpTick (int offset)
{
    const int n = nArp;
    if (n <= 0) return;
    std::array<ArpNote, 32> asc;
    for (int i = 0; i < n; ++i) asc[(size_t) i] = arpNotes[(size_t) i];
    std::sort (asc.begin(), asc.begin() + n, [] (const ArpNote& a, const ArpNote& b) { return a.midi < b.midi; });
    auto play = [&] (const ArpNote& a) { startNote (a.midi, a.vel, (rng.nextFloat() - .5f) * .6f, false, offset, true); };
    switch (P.arp)
    {
        case 6: for (int i = 0; i < n; ++i) play (asc[(size_t) i]); break;            // CHORD
        case 1: play (asc[(size_t) (arpI % n)]); break;                                 // UP
        case 2: play (asc[(size_t) (n - 1 - arpI % n)]); break;                         // DOWN
        case 3: { const int cyc = juce::jmax (1, 2 * n - 2), k = arpI % cyc; play (asc[(size_t) (k < n ? k : cyc - k)]); break; }
        case 4: play (asc[(size_t) rng.nextInt (n)]); break;                            // RANDOM
        default: play (arpNotes[(size_t) (arpI % n)]); break;                           // ORDER
    }
    ++arpI;
}

//==============================================================================
void Engine::renderVoices (int start, int num)
{
    auto* L = busL.getWritePointer (0); auto* R = busL.getWritePointer (1); auto* SH = shimBus.getWritePointer (0);
    const float fsr = (float) sr;
    for (int s = start; s < start + num; s += kSub)
    {
        const int m = juce::jmin (kSub, start + num - s);
        modPh += 5.2 * m / sr; if (modPh > 1) modPh -= 1;
        const float gc = std::sin (twoPi * (float) modPh) * modWheel * 35.0f + bendCents;

        for (auto& v : voices)
        {
            if (! v.active) continue;
            int off = s, cnt = m;
            if (v.delay > 0)
            {
                const int d = juce::jmin (v.delay, m);
                v.delay -= d; off += d; cnt -= d;
                if (cnt <= 0) continue;
            }
            bool alive = false;
            for (int pi = 0; pi < v.nParts; ++pi)
            {
                auto& pt = v.parts[(size_t) pi];
                if (! pt.active) continue;
                // --- mises à jour par sous-bloc
                if (pt.glideCoef < 1.0f)
                {
                    const float c = 1.0f - std::pow (1.0f - pt.glideCoef, (float) cnt);
                    pt.f += (pt.fTarget - pt.f) * c;
                    pt.smpRate += (pt.smpRateTarget - pt.smpRate) * c;
                }
                const float tSec = (float) pt.t / fsr;
                float cents = gc;
                if (pt.hasVib)
                {
                    cents += std::sin (twoPi * (float) pt.vibPh) * pt.vibDepth * juce::jmin (1.0f, tSec / .5f);
                    pt.vibPh += pt.vibRate * cnt / sr; if (pt.vibPh > 1) pt.vibPh -= 1;
                }
                const float detMul = std::exp2 (cents / 1200.0f);
                if (! pt.fixedCut)
                    pt.lp.set (pt.fc0 * std::pow (pt.fc1 / pt.fc0, juce::jmin (1.0f, (float) pt.t / (float) pt.ftS)), pt.q, fsr);
                const float fmIdx = pt.hasFm ? pt.fmIdx0 * std::pow (pt.fmIdx1 / juce::jmax (1e-3f, pt.fmIdx0), juce::jmin (1.0f, (float) pt.t / (float) pt.fmDecS)) : 0.0f;
                if (pt.hasNoise) pt.nbp.set (pt.f * pt.noiseR, pt.noiseQ, fsr);
                std::array<double, 8> dt {};
                for (int k = 0; k < pt.nOsc; ++k) dt[(size_t) k] = (double) (pt.f * pt.osc[(size_t) k].r * pt.osc[(size_t) k].det * detMul) / sr;
                std::array<double, 2> sdt {};
                for (int k = 0; k < pt.nShim; ++k) sdt[(size_t) k] = (double) (pt.f * pt.shim[(size_t) k].r * detMul) / sr;
                const double fmDt = pt.f * pt.fmR / sr;
                const SampleData* smp = pt.smp.get();
                const double smpStep = smp != nullptr ? (double) pt.smpRate * detMul * smp->sampleRate / sr : 0;

                for (int i = 0; i < cnt; ++i)
                {
                    float fmOut = 0;
                    if (pt.hasFm)
                    {
                        pt.fmPh += fmDt; if (pt.fmPh >= 1) pt.fmPh -= 1;
                        fmOut = std::sin (twoPi * (float) pt.fmPh) * fmIdx;
                    }
                    float mix = 0;
                    for (int k = 0; k < pt.nOsc; ++k)
                    {
                        auto& o = pt.osc[(size_t) k];
                        const double d = dt[(size_t) k] + (o.fm ? fmOut / sr : 0.0);
                        o.ph += d; o.ph -= std::floor (o.ph);
                        mix += wave (o.wave, o.ph, std::abs (d)) * o.gain;
                        if (o.decLeft > 0) { o.gain *= o.decMul; --o.decLeft; }
                    }
                    if (smp != nullptr)
                    {
                        const int n0 = (int) pt.smpPos, len = smp->buffer.getNumSamples();
                        if (n0 + 1 < len)
                        {
                            const float fr = (float) (pt.smpPos - n0);
                            float sv = 0;
                            for (int ch = 0; ch < smp->buffer.getNumChannels(); ++ch)
                            {
                                const auto* d = smp->buffer.getReadPointer (ch);
                                sv += d[n0] + (d[n0 + 1] - d[n0]) * fr;
                            }
                            mix += sv / (float) smp->buffer.getNumChannels();
                            pt.smpPos += smpStep;
                        }
                    }
                    if (pt.hasNoise) mix += pt.nbp.bp (rng.nextFloat() * 2 - 1) * pt.noiseG;
                    float x = mix;
                    if (pt.nForm > 0)
                    {
                        x = 0;
                        for (int k = 0; k < pt.nForm; ++k) x += pt.form[(size_t) k].bp (mix) * pt.formG[(size_t) k];
                    }
                    float y = pt.lp.lp (x);
                    if (pt.clickLeft > 0)
                    {
                        y += pt.clickHp.hp ((rng.nextFloat() * 2 - 1) * (float) pt.clickLeft / (float) pt.clickLen) * pt.clickAmp;
                        --pt.clickLeft;
                    }
                    switch (pt.st)
                    {
                        case Part::Atk: pt.env += pt.atkInc; if (pt.env >= pt.peak) { pt.env = pt.peak; pt.st = pt.hold ? Part::Sus : Part::Dec; } break;
                        case Part::Sus: pt.env += (pt.susLevel - pt.env) * pt.susCoef; break;
                        case Part::Dec: pt.env *= pt.decMul; if (pt.t > pt.endAt) pt.st = Part::Done; break;
                        case Part::Rel: pt.env -= pt.env * pt.relCoef; break;
                        case Part::Done: break;
                    }
                    y *= pt.env;
                    L[off + i] += y * pt.gL; R[off + i] += y * pt.gR;

                    float maxShim = 0;
                    for (int k = 0; k < pt.nShim; ++k)
                    {
                        auto& o = pt.shim[(size_t) k];
                        o.ph += sdt[(size_t) k]; o.ph -= std::floor (o.ph);
                        float& g = pt.shimGain[(size_t) k];
                        if (pt.st == Part::Rel)              g -= g * pt.shimRelCoef;
                        else if (pt.t < pt.shimRise)         g = pt.shimPk[(size_t) k] * (float) pt.t / (float) pt.shimRise;
                        else if (pt.t >= pt.shimDecStart)    g *= pt.shimDecMul[(size_t) k];
                        SH[off + i] += std::sin (twoPi * (float) o.ph) * g;
                        maxShim = juce::jmax (maxShim, g);
                    }
                    ++pt.t;
                    if (pt.st == Part::Rel && pt.env < 1e-6f && maxShim < 1e-6f) pt.st = Part::Done;
                    if (! pt.hold && pt.t > pt.endAt + (int) (2 * sr)) pt.st = Part::Done;   // garde-fou
                    if (pt.st == Part::Done) { pt.active = false; break; }
                }
                alive |= pt.active;
            }
            if (! alive) v.active = false;
        }
    }
}

//==============================================================================
void Engine::render (juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, const EngineParams& p,
                     double hostBpm, bool hostPlaying, double hostPpq, bool hostHasPpq)
{
    juce::ScopedNoDenormals noDenormals;
    P = p;
    if (hostBpm > 0) P.bpm = (float) hostBpm;
    const int n = out.getNumSamples();
    if (n > busL.getNumSamples()) prepare (sr, n);
    const double spb = sr * 60.0 / juce::jmax (20.0f, P.bpm);
    if (hostPlaying && hostHasPpq) beatPos = hostPpq;

    busL.clear (0, n); shimBus.clear (0, n);

    // événements de l'interface
    {
        const auto scope = uiFifo.read (uiFifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) handleUi (uiBuf[(size_t) (scope.startIndex1 + i)], 0);
        for (int i = 0; i < scope.blockSize2; ++i) handleUi (uiBuf[(size_t) (scope.startIndex2 + i)], 0);
    }

    // arpégiateur calé sur la grille
    if (P.arp > 0 && nArp > 0)
    {
        const double step = kRateBeats[juce::jlimit (0, 5, P.rate)];
        const double endBeat = beatPos + n / spb;
        for (double k = std::ceil (beatPos / step - 1e-9); k * step < endBeat; k += 1)
            arpTick ((int) ((k * step - beatPos) * spb));
    }
    else arpI = 0;

    // MIDI de l'hôte, au sample près
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (at > pos) { renderVoices (pos, at - pos); pos = at; }
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())                  keyDown (1000 + msg.getNoteNumber(), msg.getNoteNumber(), msg.getFloatVelocity(), at);
        else if (msg.isNoteOff())            keyUp (1000 + msg.getNoteNumber());
        else if (msg.isSustainPedalOn())     setSustain (true);
        else if (msg.isSustainPedalOff())    setSustain (false);
        else if (msg.isControllerOfType (1)) modWheel = (float) msg.getControllerValue() / 127.0f;
        else if (msg.isPitchWheel())         bendCents = (float) (msg.getPitchWheelValue() - 8192) / 8192.0f * 200.0f;
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            for (auto& v : voices) if (v.active) releaseVoice (v, msg.isAllSoundOff());
            nArp = nMono = 0; monoSerial = 0;
            for (auto& h : held) h.used = false;
        }
    }
    if (pos < n) renderVoices (pos, n - pos);
    beatPos += n / spb;

    // --- effets globaux
    const SoundDef* S = soundAt (P.sound);
    const float verb = S ? S->verb : 1.0f, dlyAmt = S ? S->dly : 1.0f, sp = P.space * 2.0f;
    wetS.setTargetValue ((.22f + .5f * P.bloom) * verb * sp);
    dlyWetS.setTargetValue (P.delayDiv > 0 ? (.08f + .22f * P.bloom) * dlyAmt * sp : 0.0f);
    widthS.setTargetValue (P.width * .8f);
    const float dTarget = (float) (kDelayBeats[juce::jlimit (0, 6, P.delayDiv)] * 60.0 / P.bpm);
    for (int ch = 0; ch < 2; ++ch) hpf[(size_t) ch].set (20.0f * std::pow (30.0f, P.lowcut), std::pow (10.0f, .7f / 20.0f), (float) sr);

    auto* L = busL.getWritePointer (0); auto* R = busL.getWritePointer (1); auto* SH = shimBus.getWritePointer (0);
    auto* oL = out.getWritePointer (0); auto* oR = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;
    auto* vL = verbIn.getWritePointer (0); auto* vR = verbIn.getWritePointer (1);
    auto* sL = shimIn.getWritePointer (0); auto* sR = shimIn.getWritePointer (1);
    const int dSize = (int) dlyL.size(), cSize = (int) chBuf.size();
    auto readFrac = [] (const std::vector<float>& buf, int w, float d, int size)
    {
        float rp = (float) w - d; while (rp < 0) rp += (float) size;
        const int i0 = (int) rp; const int i1 = (i0 + 1) % size; const float fr = rp - (float) i0;
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr;
    };
    for (int i = 0; i < n; ++i)
    {
        const float hl = hpf[0].hp (L[i]), hr = hpf[1].hp (R[i]);
        const float wet = wetS.getNextValue(), dw = dlyWetS.getNextValue(), wd = widthS.getNextValue();
        // chorus stéréo
        chBuf[(size_t) chW] = .5f * (hl + hr);
        chPh1 += .23 / sr; if (chPh1 > 1) chPh1 -= 1;
        chPh2 += .31 / sr; if (chPh2 > 1) chPh2 -= 1;
        const float d1 = (float) ((.012 + .0035 * std::sin (twoPi * chPh1)) * sr);
        const float d2 = (float) ((.019 + .0035 * std::sin (twoPi * chPh2)) * sr);
        const float cL = readFrac (chBuf, chW, d1, cSize) * wd, cR = readFrac (chBuf, chW, d2, cSize) * wd;
        chW = (chW + 1) % cSize;
        // delay synchronisé (avec filtre dans la boucle)
        if (dTarget > 0) dlyTime += (dTarget - dlyTime) * .0005f;
        const float dS = juce::jlimit (1.0f, (float) dSize - 2, dlyTime * (float) sr);
        const float pl = dlyLp[0].lp (readFrac (dlyL, dlyW, dS, dSize));
        const float pr = dlyLp[1].lp (readFrac (dlyR, dlyW, dS, dSize));
        dlyL[(size_t) dlyW] = hl + pl * .38f; dlyR[(size_t) dlyW] = hr + pr * .38f;
        dlyW = (dlyW + 1) % dSize;
        // envois réverbes
        vL[i] = hl + pl * dw; vR[i] = hr + pr * dw;
        const float sh = shimHp.hp (SH[i]);
        sL[i] = sh; sR[i] = sh;
        L[i] = .8f * hl + cL + pl * dw;
        R[i] = .8f * hr + cR + pr * dw;
        work.setSample (0, i, wet);
    }
    {
        juce::dsp::AudioBlock<float> vb (verbIn); auto vbn = vb.getSubBlock (0, (size_t) n);
        reverb.process (juce::dsp::ProcessContextReplacing<float> (vbn));
        juce::dsp::AudioBlock<float> sb (shimIn); auto sbn = sb.getSubBlock (0, (size_t) n);
        shimVerb.process (juce::dsp::ProcessContextReplacing<float> (sbn));
    }
    const auto* wv = work.getReadPointer (0);
    for (int i = 0; i < n; ++i)
    {
        oL[i] = (L[i] + vL[i] * wv[i] + sL[i] * 1.1f) * .85f;
        if (oR != nullptr) oR[i] = (R[i] + vR[i] * wv[i] + sR[i] * 1.1f) * .85f;
    }
    {
        juce::dsp::AudioBlock<float> ob (out); auto obn = ob.getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (obn);
        comp.process (ctx); lim.process (ctx);
    }
    int cnt = 0; for (auto& v : voices) cnt += v.active ? 1 : 0;
    activeVoices.store (cnt);
}

} // namespace bloom
