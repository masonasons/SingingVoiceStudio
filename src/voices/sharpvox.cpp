// sharpvox.cpp -- SharpVox TTS's voices, singing.
//
// SharpVox (third_party/SharpVoxTTS, MIT) is a formant synthesiser in the
// manner of Klatt's, with DECtalk's phonemes and voices modelled on DECtalk's.
// It is compiled into the program (cmake/engines/sharpvox.cmake) and needs no
// data: its dictionary is for reading text, which nothing here does.
//
// It is driven one level below text, as its own singing notation drives it
// ("[l<80>AA<520,C4>]", DOCUMENTATION.md): the phonemes go to
// AudioProcessor::ProcessSinging as tokens, which lays them out in a clause
// plan with no rules of its own applied; SpeechRenderer turns the plan into
// frames of formant targets and KlattSynthesizerFP each frame into samples.
// Two things are put in between:
//
//   The lengths. A plan counts in frames of 112 samples at 22050 Hz (5.08
//   ms; the engine's own arithmetic says 5), and a phoneme is a whole number
//   of them. Each phoneme is given the frames that bring its end nearest to
//   where syllable_lengths puts it, counted from the start of the phrase, so
//   the rounding never adds up and every note starts on its frame.
//
//   The pitch. Each frame's F0 is replaced with the studio's pitch curve
//   (pitch.h), so the notes, bends, detune, glides and vibrato are the
//   studio's, as they are for DECtalk's and Microsoft's voices, rather than
//   SharpVox's own portamento and vibrato. The engine's F0 is a log code, 256
//   to the octave above 25 Hz, in whole units (4.7 cents); the fraction left
//   over is carried into the next frame, and the synthesiser glides from
//   frame to frame, so a held note averages out exactly on its pitch.
//
// The synthesiser's output stage is lowered by 12 dB where it is compiled
// (sharpvox.cmake): sung vowels held at full voice go past its 16-bit full
// scale, which it clips. The 12 dB are given back here, in floats.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

#include "AudioProcessor.h"
#include "KlattSynthesizerFP.h"
#include "SpeechRenderer.h"
#include "VoicePresets.h"

namespace svs {

namespace {

//: the rate the engine is tuned at; resampled to the program's own after
constexpr int kEngineRate = 22050;
//: what sharpvox.cmake takes off the synthesiser's output stage
constexpr double kOutputCut = 4.0;

// The voices: SharpVox's own presets, named as its [:voice] command names
// them, Paul being its baseline voice. The gains even them out: each was sung
// through ten vowels on notes across its range and scaled so that its median
// peak is 0.5, as DECtalk's and the SSI-263's are (sharpvox_sing_test
// --levels). Pirate is rough and Whisper whispers, as they do in SharpVox.
struct Voice {
    const char *name;
    const char *preset;
    double gain;
};
const Voice kVoices[] = {
    {"Paul", "paul", 1.14},       {"Beth", "beth", 1.33},   {"Chris", "chris", 1.27},
    {"Deborah", "deborah", 1.23}, {"Jack", "jack", 1.28},   {"Jess", "jess", 1.35},
    {"John", "john", 1.15},       {"Matt", "matt", 0.96},   {"Pirate", "pirate", 0.46},
    {"Tommy", "tommy", 1.10},     {"Whisper", "whisper", 1.74},
};
constexpr int kVoiceCount = int(sizeof kVoices / sizeof kVoices[0]);

// VocalWriter's fifty-seven phonemes as SharpVox's, which are DECtalk's and
// so very nearly the same set. VocalWriter's UX (bUd) is SharpVox's AH; its
// glottal stops Q and QX are SharpVox's QX, and its flap DD SharpVox's DX
// (SharpVox's own DD is the unreleased d of "and"). O, the first part of OY,
// is sung as AO.
struct PhoneMap {
    const char *vw;
    int16_t phon;
};
const PhoneMap kPhoneMap[] = {
    {"IY", SharpVox::_IY_}, {"IH", SharpVox::_IH_}, {"EH", SharpVox::_EH_}, {"AE", SharpVox::_AE_},
    {"AA", SharpVox::_AA_}, {"UX", SharpVox::_AH_}, {"AO", SharpVox::_AO_}, {"UH", SharpVox::_UH_},
    {"AX", SharpVox::_AX_}, {"ER", SharpVox::_ER_}, {"EY", SharpVox::_EY_}, {"AY", SharpVox::_AY_},
    {"OY", SharpVox::_OY_}, {"AW", SharpVox::_AW_}, {"OW", SharpVox::_OW_}, {"UW", SharpVox::_UW_},
    {"YU", SharpVox::_YU_}, {"IR", SharpVox::_IR_}, {"XR", SharpVox::_XR_}, {"AR", SharpVox::_AR_},
    {"OR", SharpVox::_OR_}, {"UR", SharpVox::_UR_}, {"IX", SharpVox::_IX_}, {"%", SharpVox::_SIL_},
    {"RX", SharpVox::_RX_}, {"LX", SharpVox::_LX_}, {"EL", SharpVox::_EL_}, {"EN", SharpVox::_EN_},
    {"w", SharpVox::_W_},   {"y", SharpVox::_Y_},   {"r", SharpVox::_R_},   {"l", SharpVox::_L_},
    {"h", SharpVox::_HH_},  {"m", SharpVox::_M_},   {"n", SharpVox::_N_},   {"NG", SharpVox::_NG_},
    {"f", SharpVox::_F_},   {"v", SharpVox::_V_},   {"TH", SharpVox::_TH_}, {"DH", SharpVox::_DH_},
    {"s", SharpVox::_S_},   {"z", SharpVox::_Z_},   {"SH", SharpVox::_SH_}, {"ZH", SharpVox::_ZH_},
    {"p", SharpVox::_P_},   {"b", SharpVox::_B_},   {"t", SharpVox::_T_},   {"d", SharpVox::_D_},
    {"k", SharpVox::_K_},   {"g", SharpVox::_G_},   {"CH", SharpVox::_CH_}, {"JH", SharpVox::_JH_},
    {"TX", SharpVox::_TX_}, {"Q", SharpVox::_QX_},  {"QX", SharpVox::_QX_}, {"DD", SharpVox::_DX_},
    {"O", SharpVox::_AO_},
};

const PhoneMap *find_map(const std::string &symbol) {
    std::string s = palette_alias(symbol);   // OH and DX, as VocalWriter's MIDI files spell them
    for (const PhoneMap &m : kPhoneMap)
        if (s == m.vw) return &m;
    return nullptr;
}

SharpVox::VoiceData voice_data(int v) {
    SharpVox::VoiceData data = SharpVox::VoiceData::baseline_voice();
    SharpVox::VoicePresets::TryGet(kVoices[v].preset, data);
    return data;
}

class SharpVoxSinger : public Singer {
public:
    explicit SharpVoxSinger(int v) : v_(v), voice_(voice_data(v)) {}

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        SharpVox::KlattSynthesizerFP synth(kEngineRate);
        set_up(&synth);
        const double frame = double(synth.SampFrameLen) / kEngineRate;

        // One token for each phoneme, and where each is to end, in frames from
        // the first note's start. A note with nothing in it is a silence.
        std::vector<SharpVox::PhonemeToken> tokens;
        std::vector<long> ends;
        uint8_t breath = uint8_t(std::lround(std::clamp(phrase.style.breath, 0, 127) * 100.0 / 127.0));
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            const SungNote &n = phrase.notes[i];
            double at = curve.note_start(i) * 1000.0;
            bool any = false;
            for (size_t k = 0; k < n.phonemes.size(); ++k) {
                double ms = k < n.durations_ms.size() ? n.durations_ms[k] : 0.0;
                at += ms;
                const PhoneMap *m = find_map(n.phonemes[k]);
                if (!m) continue;
                tokens.push_back(token(m->phon, tokens.empty(), breath));
                ends.push_back(std::lround(at / 1000.0 / frame));
                any = true;
            }
            if (!any) {
                tokens.push_back(token(SharpVox::_SIL_, tokens.empty(), breath));
                ends.push_back(std::lround(curve.note_end(i) / frame));
            }
        }

        // The plan: a frame of silence, the tokens, and SharpVox's own 150 ms
        // of silence after them for the last sound to die away in.
        SharpVox::AudioProcessor processor(voice_);
        SharpVox::ClausePlan plan = processor.ProcessSinging(tokens);
        long done = 0;
        for (size_t t = 0; t < tokens.size(); ++t) {
            long frames = std::max(1L, ends[t] - done);
            plan.DurBuf[t + 1] = int32_t(frames);
            done += frames;
        }
        const int lead = plan.DurBuf[0];

        SharpVox::SpeechRenderer renderer(voice_);
        std::vector<int16_t> pcm;
        long index = 0;
        double carry = 0.0;
        renderer.RenderStreaming(plan, [&](const SharpVox::Frame &f) {
            SharpVox::Frame out = f;
            // the synthesiser reaches a frame's F0 at the frame's end
            double t = double(index + 1 - lead) * frame;
            double hz = curve.hz_at(std::clamp(t, 0.0, curve.total())) - voice_.PitchOffsetHz;
            double code = 256.0 * std::log2(std::max(hz, 20.0) / 25.0) + carry;
            long whole = std::lround(code);
            carry = code - double(whole);
            out.F0 = int16_t(std::clamp(whole, 1L, 0x7FFL));
            size_t at = pcm.size();
            pcm.resize(at + size_t(synth.SampFrameLen));
            synth.SynthesizeFrame(out, pcm.data(), int32_t(at));
            ++index;
        });

        std::vector<float> wide(pcm.begin(), pcm.end());
        std::vector<float> samples = resample_to_output(wide, kEngineRate);
        // to +-1 at the part's level; a phrase that would still clip is
        // brought down whole rather than cut off
        double gain = phrase.level * kVoices[v_].gain * kOutputCut / 32768.0, peak = 0.0;
        for (float &s : samples) {
            s = float(s * gain);
            peak = std::max(peak, double(std::fabs(s)));
        }
        if (peak > 0.999) {
            float k = float(0.999 / peak);
            for (float &s : samples) s *= k;
        }
        sung.samples = std::move(samples);
        sung.starts_before = lead * frame;
        return sung;
    }

private:
    // A token as SharpVox's singing notation makes one: the first of the
    // clause begins its word; the length is set on the plan afterwards.
    static SharpVox::PhonemeToken token(int16_t phon, bool first, uint8_t breath) {
        SharpVox::PhonemeToken t{};
        t.Phon = phon;
        t.Ctrl = SharpVox::kSingingPhon | SharpVox::kSingingDuration;
        if (first) t.Ctrl |= SharpVox::kWord_Start | SharpVox::kContent_Word;
        t.Aspiration = breath;
        return t;
    }

    // The synthesiser set up for the voice, as SharpVox's TtsEngine sets it up
    // (ResetSynthVoice), with no offset to the pitch: the curve has it.
    void set_up(SharpVox::KlattSynthesizerFP *s) const {
        const SharpVox::VoiceData &v = voice_;
        auto hz = [](int f) { return int16_t(std::clamp(f, 100, 8000)); };
        int lo = v.LarynxOffset;
        float ts = v.TractScale > 0 ? v.TractScale : 1.0f;
        s->SetVoice(v.NGain, true, hz(v.F4Freq + lo), v.F4BW, hz(v.F5Freq + lo), v.F5BW,
                    hz(int(3650 * ts) + lo), 150, hz(int(4200 * ts) + lo), 100, hz(int(6000 * ts) + lo), 700,
                    v.NasalBase, v.NasalBW, v.AGain, v.ACycle);
        s->Jitter = v.Jitter;
        s->Shimmer = v.Shimmer;
        s->Diplophonia = v.Diplophonia;
        s->FryAmount = v.FryAmount;
        s->SubglottalAmt = v.SubglottalAmt;
        s->BreathAmt = v.BreathAmt;
        s->OpenQuotient_set(v.OpenQuotient);
        s->OQStressLink = v.OQStressLink;
        s->OQF0Link = v.OQF0Link;
        s->BasePitchHz = v.PitchHz;
        s->LarynxOffset = int16_t(lo);
        s->PharyngealAmt = v.PharyngealAmt;
        s->PitchOffsetHz = 0;
        s->LipRounding = v.LipRounding;
        s->ComputeGlotWave(v.VGain);
    }

    int v_;
    SharpVox::VoiceData voice_;
};

class SharpVoxFamily : public Family {
public:
    std::string name() const override { return "SharpVox"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int i = 0; i < kVoiceCount; ++i)
            out.push_back({kSharpVoxBase + i, std::string("SharpVox ") + kVoices[i].name});
        return out;
    }

    bool available(std::string *) override { return true; }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int v = voice_id - kSharpVoxBase;
        if (v < 0 || v >= kVoiceCount) {
            if (err) *err = "There is no SharpVox voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        return std::make_unique<SharpVoxSinger>(v);
    }
};

}  // namespace

std::unique_ptr<Family> make_sharpvox_family() { return std::make_unique<SharpVoxFamily>(); }

//: Not part of family.h: the self-check the engine's test program runs. Every
//: VocalWriter phoneme has a row in the table and every row is one of
//: VocalWriter's. Returns an empty string, or what is wrong.
std::string sharpvox_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes())
        if (!find_map(p.symbol)) bad += std::string("no SharpVox phoneme for ") + p.symbol + "; ";
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    return bad;
}

}  // namespace svs
