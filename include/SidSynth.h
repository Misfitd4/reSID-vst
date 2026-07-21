#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sid.h"

namespace resid_vst {

enum class ChipModel { mos6581 = 0, mos8580 = 1 };
enum class Waveform { triangle = 0, saw = 1, pulse = 2, noise = 3, off = 4 };
enum class FilterMode { off = 0, lowpass = 1, bandpass = 2, highpass = 3, notch = 4 };
enum class WavetableMode { off = 0, pulseSaw = 1, pulseNoise = 2, octavePulse = 3, fifthSaw = 4 };
enum class VoiceMode { mono = 0, poly = 1, unison = 2, code = 3, codePoly = 4 };
enum class ModWheelTarget { off = 0, vibrato = 1, filter = 2, pulseWidth = 3 };

struct SidCodeInstrument {
    static constexpr int rowCount = 32;

    struct WfRow {
        uint8_t waveform = 0x41;
        uint8_t arp = 0x00;
        uint8_t detune = 0xff;
    };

    struct TripleRow {
        uint8_t command = 0x00;
        uint8_t value = 0x00;
        uint8_t keyTrack = 0x00;
    };

    struct ChordRow {
        uint8_t offset0 = 0x00;
        uint8_t offset1 = 0x00;
        uint8_t offset2 = 0x00;
        uint8_t next = 0xff;
    };

    uint16_t adsr = 0x00f0;
    uint8_t speed = 0x00;
    uint16_t vibrato = 0x0000;
    uint8_t vibratoType = 0x10;
    uint8_t wfLength = 0;
    uint8_t pulseLength = 0;
    uint8_t filterLength = 0;
    uint8_t chordLength = 0;
    uint16_t defaultPulseWidth = 0x0800;
    uint16_t defaultFilterCutoff = 1535;
    uint8_t defaultFilterResonance = 0;
    uint8_t defaultFilterMode = 0x00;
    bool absolutePitch = false;
    uint8_t fixedMidiNote = 48;
    bool velocityRelease = false;
    uint8_t gateOffWfRow = 0xff;
    uint8_t gateOffPulseRow = 0xff;
    uint8_t gateOffFilterRow = 0xff;
    uint8_t playbackRate = 1;
    std::array<WfRow, rowCount> wf {};
    std::array<TripleRow, rowCount> pulse {};
    std::array<TripleRow, rowCount> filter {};
    std::array<ChordRow, rowCount> chord {};
};

struct SidVoiceSettings {
    Waveform waveform = Waveform::pulse;
    WavetableMode wavetable = WavetableMode::off;
    float wavetableRate = 8.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
    float pulseWidth = 0.5f;
    float detuneCents = 0.0f;
    int octave = 0;
    bool glideEnabled = false;
    float glideTimeMs = 120.0f;
};

struct SidSynthSettings {
    ChipModel chipModel = ChipModel::mos6581;
    FilterMode filterMode = FilterMode::lowpass;
    VoiceMode voiceMode = VoiceMode::mono;
    std::array<SidVoiceSettings, 3> voice {};
    SidCodeInstrument codeInstrument {};
    float cutoff = 0.75f;
    float resonance = 0.2f;
    float outputGain = 0.8f;
    float pitchBendRangeSemitones = 2.0f;
    ModWheelTarget modWheelTarget = ModWheelTarget::vibrato;
    float modWheelDepth = 0.5f;
    bool codeLegato = false;
};

class SidSynth {
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const SidSynthSettings& newSettings);

    void noteOn(int midiNote, float velocity);
    void noteOff(int midiNote);
    void allNotesOff();
    void setPitchBend(float normalizedBend);
    void setModWheel(float normalizedValue);

    float nextSample();
    double getSampleRate() const { return sampleRate; }
    uint16_t debugVoiceFrequency(int voiceIndex) const;
    uint8_t debugVoiceControl(int voiceIndex) const;
    uint16_t debugVoicePulseWidth(int voiceIndex) const;
    float debugVoiceSample(int voiceIndex) const;
    uint16_t debugFilterCutoff() const { return appliedFilterCutoff; }
    uint8_t debugFilterMode() const { return appliedFilterMode; }
    bool isVoiceActive(int voiceIndex) const;
    bool hasActiveVoice() const;

private:
    struct VoiceState {
        int midiNote = -1;
        float velocity = 0.0f;
        uint64_t age = 0;
        int wavetableStep = 0;
        double wavetableSamplesUntilStep = 0.0;
        double codeSamplesUntilStep = 0.0;
        int codeWfFramesUntilStep = 0;
        int codeMultispeedPhase = 0;
        int codeWfRow = 0;
        int codePulseRow = 0;
        int codeFilterRow = 0;
        int codeChordRow = 0;
        int codeChordNote = 0;
        int codePulseRepeat = 0;
        int codePulseDelta = 0;
        int codeFilterRepeat = 0;
        int codeFilterDelta = 0;
        int codeSemitoneOffset = 0;
        double codePitchCents = 0.0;
        double codeTargetPitchCents = 0.0;
        double codeVibratoPhase = 0.0;
        double codeVibratoSamplesUntilStep = 0.0;
        double codeSamplesSinceTrigger = 0.0;
        bool codePitchOverride = false;
        uint8_t codeDetune = 0;
        float codeVibratoCents = 0.0f;
        uint8_t codeControl = 0x41;
        uint8_t lastControl = 0x00;
        uint16_t codePulseWidth = 2048;
        uint16_t codeFilterCutoff = 1535;
        uint8_t codeFilterResonance = 0;
        uint8_t codeFilterMode = 0x10;
        double glideNote = 60.0;
        double glideTargetNote = 60.0;
        double glideStepPerSample = 0.0;
        int glideSamplesRemaining = 0;
        bool allocated = false;
        bool gate = false;
    };

    struct HeldNote {
        int midiNote = -1;
        float velocity = 0.0f;
        uint64_t age = 0;
    };

    static constexpr double c64PalClock = 985248.0;
    static constexpr int sidVoiceCount = 3;

    void configureSid();
    void applyGlobalRegisters();
    void updateVoice(int voiceIndex);
    void advanceWavetables();
    void advanceCodeTables();
    void resetCodeState(int voiceIndex);
    void stepCodeVoice(int voiceIndex, bool mainFrame);
    void stepCodePulse(VoiceState& voice);
    void stepCodeFilter(VoiceState& voice);
    void advanceCodeVibrato();
    void advancePerformanceModulation();
    void advanceGlide();
    void writeVoiceGate(int voiceIndex, bool gate);
    void triggerVoice(int voiceIndex, int midiNote, float velocity);
    void triggerHeldNote(const HeldNote& heldNote);
    void retuneCodeLegato(int midiNote, float velocity);
    void retuneLegatoVoice(int voiceIndex, int midiNote, float velocity, bool preserveCodeTables);
    void applyCodeGateOffPointers(int voiceIndex);
    void rememberHeldNote(int midiNote, float velocity);
    bool releaseHeldNote(int midiNote);
    const HeldNote* newestHeldNote() const;
    void releaseStackedVoices();
    int chooseVoice();

    static uint16_t noteToSidFrequency(int midiNote, int octave, float detuneCents, int semitoneOffset);
    static uint16_t noteValueToSidFrequency(double note);
    static int signedByte(uint8_t value);
    static int codeArpOffset(uint8_t arp, const VoiceState& voice, const SidCodeInstrument& instrument);
    static bool codeArpIsAbsolute(uint8_t arp);
    static int codeArpAbsoluteNote(uint8_t arp);
    static uint8_t codeFilterModeBits(uint8_t command);
    static uint8_t velocityReleaseNibble(float velocity);
    static int wavetableSemitoneOffset(WavetableMode mode, int step);
    static bool voiceProducesWaveform(const SidVoiceSettings& voiceSettings);
    static Waveform wavetableWaveform(const SidVoiceSettings& voiceSettings, int step);
    static uint8_t waveformBits(Waveform waveform);
    static uint8_t adsrByte(float highNibble, float lowNibble);
    static uint8_t sidNibble(float value);
    static uint8_t cleanSustainByte(float value);
    static uint16_t sid12Bit(float value);
    static uint16_t sid11Bit(float value);

    reSID::SID sid;
    SidSynthSettings settings;
    std::array<VoiceState, sidVoiceCount> voices {};
    double sampleRate = 44100.0;
    double configuredSampleRate = 0.0;
    double cycleRemainder = 0.0;
    ChipModel configuredChipModel = ChipModel::mos6581;
    bool sidConfigured = false;
    uint64_t noteCounter = 0;
    int nextPolyVoice = 0;
    std::vector<HeldNote> heldNotes;
    float pitchBend = 0.0f;
    float modWheel = 0.0f;
    double modWheelPhase = 0.0;
    uint16_t appliedFilterCutoff = 0;
    uint8_t appliedFilterMode = 0;
    std::array<uint16_t, 3> appliedPulseWidths {};
    std::array<float, 3> lastVoiceSamples {};
};

} // namespace resid_vst
