#include "SidSynth.h"

#include <algorithm>
#include <cmath>
#include <iostream>

int main()
{
    resid_vst::SidSynth synth;
    synth.prepare(48000.0);

    resid_vst::SidSynthSettings settings;
    settings.outputGain = 1.0f;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);

    const auto firstNoteFrequency = synth.debugVoiceFrequency(0);
    synth.noteOn(64, 1.0f);
    const auto secondNoteFrequency = synth.debugVoiceFrequency(0);
    synth.noteOff(64);
    const auto restoredNoteFrequency = synth.debugVoiceFrequency(0);
    if (firstNoteFrequency == 0 || secondNoteFrequency == firstNoteFrequency || restoredNoteFrequency != firstNoteFrequency) {
        std::cerr << "mono note stack failed: first=" << firstNoteFrequency
                  << " second=" << secondNoteFrequency
                  << " restored=" << restoredNoteFrequency << "\n";
        return 1;
    }

    settings.voice[0].octave = 1;
    synth.setSettings(settings);
    const auto octaveUpFrequency = synth.debugVoiceFrequency(0);
    settings.voice[0].octave = 0;
    synth.setSettings(settings);
    const auto octaveResetFrequency = synth.debugVoiceFrequency(0);
    if (octaveUpFrequency <= restoredNoteFrequency || octaveResetFrequency != restoredNoteFrequency) {
        std::cerr << "octave offset failed: base=" << restoredNoteFrequency
                  << " up=" << octaveUpFrequency
                  << " reset=" << octaveResetFrequency << "\n";
        return 1;
    }

    settings.pitchBendRangeSemitones = 2.0f;
    synth.setSettings(settings);
    synth.setPitchBend(1.0f);
    const auto bentFrequency = synth.debugVoiceFrequency(0);
    synth.setPitchBend(0.0f);
    if (bentFrequency <= octaveResetFrequency || synth.debugVoiceFrequency(0) != octaveResetFrequency) {
        std::cerr << "pitch bend failed: base=" << octaveResetFrequency
                  << " bent=" << bentFrequency << " reset=" << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }

    settings.modWheelTarget = resid_vst::ModWheelTarget::vibrato;
    settings.modWheelDepth = 1.0f;
    synth.setSettings(settings);
    synth.setModWheel(1.0f);
    for (int i = 0; i < 2400; ++i) {
        synth.nextSample();
    }
    const auto modulatedFrequency = synth.debugVoiceFrequency(0);
    synth.setModWheel(0.0f);
    if (modulatedFrequency == octaveResetFrequency || synth.debugVoiceFrequency(0) != octaveResetFrequency) {
        std::cerr << "mod wheel vibrato failed: base=" << octaveResetFrequency
                  << " modulated=" << modulatedFrequency << " reset=" << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }

    settings.voiceMode = resid_vst::VoiceMode::unison;
    settings.voice[0].waveform = resid_vst::Waveform::noise;
    settings.voice[1].waveform = resid_vst::Waveform::off;
    settings.voice[2].waveform = resid_vst::Waveform::off;
    synth.setSettings(settings);
    synth.noteOn(67, 1.0f);
    if (synth.debugVoiceFrequency(0) == 0 || synth.debugVoiceFrequency(1) != 0 || synth.debugVoiceFrequency(2) != 0) {
        std::cerr << "unison off voices failed: v1=" << synth.debugVoiceFrequency(0)
                  << " v2=" << synth.debugVoiceFrequency(1)
                  << " v3=" << synth.debugVoiceFrequency(2) << "\n";
        return 1;
    }
    synth.noteOff(67);
    synth.allNotesOff();

    settings.voiceMode = resid_vst::VoiceMode::mono;
    settings.voice[0].waveform = resid_vst::Waveform::pulse;
    settings.voice[1].waveform = resid_vst::Waveform::pulse;
    settings.voice[2].waveform = resid_vst::Waveform::pulse;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    const auto releaseScopeFrequency = synth.debugVoiceFrequency(0);
    synth.noteOff(60);
    if (releaseScopeFrequency == 0 || synth.debugVoiceFrequency(0) != releaseScopeFrequency) {
        std::cerr << "release scope frequency failed: on=" << releaseScopeFrequency
                  << " release=" << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }
    synth.allNotesOff();
    if (synth.debugVoiceFrequency(0) != 0) {
        std::cerr << "all notes off scope frequency failed: " << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }

    settings.voiceMode = resid_vst::VoiceMode::poly;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.noteOn(62, 1.0f);
    synth.noteOn(64, 1.0f);
    const auto polyVoice0 = synth.debugVoiceFrequency(0);
    const auto polyVoice1 = synth.debugVoiceFrequency(1);
    const auto polyVoice2 = synth.debugVoiceFrequency(2);
    if (polyVoice0 == 0 || polyVoice1 == 0 || polyVoice2 == 0 || polyVoice0 == polyVoice1 || polyVoice1 == polyVoice2) {
        std::cerr << "poly round robin failed: v1=" << polyVoice0
                  << " v2=" << polyVoice1
                  << " v3=" << polyVoice2 << "\n";
        return 1;
    }
    synth.noteOn(65, 1.0f);
    const auto polyVoice0Next = synth.debugVoiceFrequency(0);
    if (polyVoice0Next == polyVoice0 || synth.debugVoiceFrequency(1) != polyVoice1 || synth.debugVoiceFrequency(2) != polyVoice2) {
        std::cerr << "poly wrap failed: v1=" << polyVoice0Next
                  << " v2=" << synth.debugVoiceFrequency(1)
                  << " v3=" << synth.debugVoiceFrequency(2) << "\n";
        return 1;
    }
    synth.allNotesOff();

    synth.noteOn(60, 1.0f);
    synth.noteOn(62, 1.0f);
    synth.noteOn(64, 1.0f);
    const auto reusableVoice0 = synth.debugVoiceFrequency(0);
    const auto reusableVoice2 = synth.debugVoiceFrequency(2);
    synth.noteOff(62);
    synth.noteOn(67, 1.0f);
    if (synth.debugVoiceFrequency(0) != reusableVoice0
        || synth.debugVoiceFrequency(1) == 0
        || synth.debugVoiceFrequency(2) != reusableVoice2) {
        std::cerr << "poly did not reuse first unheld voice\n";
        return 1;
    }
    synth.allNotesOff();
    settings.voiceMode = resid_vst::VoiceMode::code;
    settings.codeInstrument.wfLength = 1;
    settings.codeInstrument.wf[0] = { 0x81, 0xa4, 0xff };
    settings.codeInstrument.adsr = 0x0800;
    settings.codeInstrument.vibrato = 0x0000;
    settings.codeInstrument.defaultFilterCutoff = 777;
    settings.codeInstrument.defaultFilterMode = 0x20;
    settings.filterMode = resid_vst::FilterMode::off;
    settings.cutoff = 0.05f;
    synth.setSettings(settings);
    if (synth.debugFilterCutoff() != 777 || synth.debugFilterMode() != 0x20) {
        std::cerr << "code instrument filter defaults were overridden: cutoff=" << synth.debugFilterCutoff()
                  << " mode=" << static_cast<int>(synth.debugFilterMode()) << "\n";
        return 1;
    }
    settings.filterMode = resid_vst::FilterMode::highpass;
    settings.cutoff = 0.95f;
    synth.setSettings(settings);
    if (synth.debugFilterCutoff() != 777 || synth.debugFilterMode() != 0x20) {
        std::cerr << "global filter changed code instrument filter\n";
        return 1;
    }

    settings.codeInstrument.filterLength = 2;
    settings.codeInstrument.filter[0] = { 0x9b, 0xc0, 0x00 };
    settings.codeInstrument.filter[1] = { 0x08, 0x80, 0x00 };
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    if (synth.debugFilterCutoff() != 1536 || synth.debugFilterMode() != 0x10) {
        std::cerr << "absolute filter row failed: cutoff=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    for (int i = 0; i < 960; ++i) {
        synth.nextSample();
    }
    if (synth.debugFilterCutoff() != 1408) {
        std::cerr << "relative filter step scaling failed: cutoff=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    for (int i = 0; i < 960; ++i) {
        synth.nextSample();
    }
    if (synth.debugFilterCutoff() != 1280) {
        std::cerr << "relative filter repeat failed: cutoff=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.filterLength = 0;
    synth.setSettings(settings);

    synth.noteOn(67, 1.0f);
    if (synth.debugVoiceControl(0) != settings.codeInstrument.wf[0].waveform) {
        std::cerr << "code first-frame control failed: control=" << static_cast<int>(synth.debugVoiceControl(0)) << "\n";
        return 1;
    }
    for (int i = 0; i <= 960; ++i) synth.nextSample();
    if (synth.debugVoiceControl(0) != 0x81) {
        std::cerr << "code gate-on WF row failed: control=" << static_cast<int>(synth.debugVoiceControl(0)) << "\n";
        return 1;
    }
    synth.noteOff(67);
    if (synth.debugVoiceControl(0) != 0x80) {
        std::cerr << "code note release failed: control=" << static_cast<int>(synth.debugVoiceControl(0)) << "\n";
        return 1;
    }
    settings.codeInstrument.wf[0].waveform = 0x80;
    synth.setSettings(settings);
    synth.noteOn(67, 1.0f);
    for (int i = 0; i <= 960; ++i) synth.nextSample();
    if (synth.debugVoiceControl(0) != 0x80) {
        std::cerr << "code gate-off row failed: control=" << static_cast<int>(synth.debugVoiceControl(0)) << "\n";
        return 1;
    }
    settings.codeInstrument.wf[0].waveform = 0x81;
    synth.setSettings(settings);
    synth.noteOn(67, 1.0f);
    synth.noteOn(69, 1.0f);
    for (int i = 0; i <= 960; ++i) synth.nextSample();
    if (synth.debugVoiceFrequency(0) == 0 || synth.debugVoiceFrequency(1) != 0 || synth.debugVoiceFrequency(2) != 0) {
        std::cerr << "code mode allocation failed: v1=" << synth.debugVoiceFrequency(0)
                  << " v2=" << synth.debugVoiceFrequency(1)
                  << " v3=" << synth.debugVoiceFrequency(2) << "\n";
        return 1;
    }
    if (synth.debugVoiceFrequency(0) != 1114) {
        std::cerr << "code fixed pitch failed: v1=" << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }
    synth.allNotesOff();

    settings.voice[0].octave = 2;
    settings.voice[0].detuneCents = 50.0f;
    settings.voice[0].wavetable = resid_vst::WavetableMode::octavePulse;
    settings.voice[0].pulseWidth = 0.1f;
    settings.modWheelTarget = resid_vst::ModWheelTarget::pulseWidth;
    settings.modWheelDepth = 1.0f;
    synth.setSettings(settings);
    synth.setPitchBend(1.0f);
    synth.setModWheel(1.0f);
    synth.noteOn(67, 1.0f);
    for (int i = 0; i <= 960; ++i) synth.nextSample();
    if (synth.debugVoiceFrequency(0) != 1114
        || synth.debugVoicePulseWidth(0) != settings.codeInstrument.defaultPulseWidth
        || synth.debugFilterCutoff() != settings.codeInstrument.defaultFilterCutoff) {
        std::cerr << "ordinary controls leaked into code instrument: frequency=" << synth.debugVoiceFrequency(0)
                  << " pulse=" << synth.debugVoicePulseWidth(0)
                  << " filter=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    synth.allNotesOff();
    synth.setPitchBend(0.0f);
    synth.setModWheel(0.0f);
    settings.voice[0].octave = 0;
    settings.voice[0].detuneCents = 0.0f;
    settings.voice[0].wavetable = resid_vst::WavetableMode::off;
    settings.modWheelTarget = resid_vst::ModWheelTarget::vibrato;
    synth.setSettings(settings);

    settings.codeInstrument.wf[0].detune = 0x20;
    synth.setSettings(settings);
    synth.noteOn(67, 1.0f);
    for (int i = 0; i <= 960; ++i) synth.nextSample();
    if (synth.debugVoiceFrequency(0) != 1114 + 0x20) {
        std::cerr << "code raw frequency detune failed: v1=" << synth.debugVoiceFrequency(0) << "\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.wf[0].detune = 0xff;
    synth.setSettings(settings);

    settings.codeInstrument.vibrato = 0x1100;
    synth.setSettings(settings);
    synth.noteOn(67, 1.0f);
    synth.nextSample();
    synth.noteOff(67);
    const auto releaseVibratoStart = synth.debugVoiceFrequency(0);
    auto releaseVibratoMoved = false;
    for (int i = 0; i < 960 * 3; ++i) {
        synth.nextSample();
        releaseVibratoMoved = releaseVibratoMoved || synth.debugVoiceFrequency(0) != releaseVibratoStart;
    }
    if (!releaseVibratoMoved) {
        std::cerr << "code release vibrato stopped\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.vibrato = 0x0000;

    settings.codeInstrument.wfLength = 3;
    settings.codeInstrument.speed = 0x00;
    settings.codeInstrument.wf[0] = { 0x81, 0x00, 0xff };
    settings.codeInstrument.wf[1] = { 0x81, 0x04, 0xff };
    settings.codeInstrument.wf[2] = { 0xfe, 0x00, 0xff };
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    const auto releaseArpStart = synth.debugVoiceFrequency(0);
    synth.noteOff(60);
    for (int i = 0; i < 960; ++i) {
        synth.nextSample();
    }
    if (synth.debugVoiceFrequency(0) == releaseArpStart) {
        std::cerr << "code arpeggio stopped during release\n";
        return 1;
    }
    synth.allNotesOff();

    settings.codeInstrument.wfLength = 3;
    settings.codeInstrument.wf[0] = { 0x51, 0x00, 0xff };
    settings.codeInstrument.wf[1] = { 0xfe, 0x01, 0xff };
    settings.codeInstrument.wf[2] = { 0x20, 0xe0, 0xff };
    settings.codeInstrument.gateOffWfRow = 2;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    for (int i = 0; i < 960 * 2; ++i) synth.nextSample();
    const auto gateOffPointerStart = synth.debugVoiceFrequency(0);
    synth.noteOff(60);
    for (int i = 0; i < 960; ++i) synth.nextSample();
    if (synth.debugVoiceFrequency(0) == gateOffPointerStart) {
        std::cerr << "code WF gate-off pointer was ignored\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.gateOffWfRow = 0xff;
    settings.codeInstrument.wfLength = 1;
    settings.codeInstrument.wf[0] = { 0x81, 0x00, 0xff };

    settings.codeLegato = true;
    settings.codeInstrument.wfLength = 3;
    settings.codeInstrument.wf[0] = { 0x81, 0x00, 0xff };
    settings.codeInstrument.wf[1] = { 0x81, 0x0c, 0xff };
    settings.codeInstrument.wf[2] = { 0xfe, 0x00, 0xff };
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    for (int i = 0; i < 960 * 2; ++i) synth.nextSample();
    const auto legatoBefore = synth.debugVoiceFrequency(0);
    synth.noteOn(64, 1.0f);
    const auto legatoAfter = synth.debugVoiceFrequency(0);
    if (legatoAfter <= legatoBefore) {
        std::cerr << "code legato restarted WF table\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeLegato = false;
    settings.codeInstrument.wfLength = 1;
    settings.codeInstrument.wf[0] = { 0x81, 0x00, 0xff };

    settings.voiceMode = resid_vst::VoiceMode::codePoly;
    settings.codeInstrument.wf[0].arp = 0x00;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.noteOn(64, 1.0f);
    synth.noteOn(67, 1.0f);
    const auto codePoly0 = synth.debugVoiceFrequency(0);
    const auto codePoly1 = synth.debugVoiceFrequency(1);
    const auto codePoly2 = synth.debugVoiceFrequency(2);
    if (codePoly0 == 0 || codePoly1 == 0 || codePoly2 == 0
        || codePoly0 == codePoly1 || codePoly1 == codePoly2) {
        std::cerr << "code poly allocation failed: v1=" << codePoly0
                  << " v2=" << codePoly1 << " v3=" << codePoly2 << "\n";
        return 1;
    }
    synth.noteOff(64);
    if (!synth.isVoiceActive(0) || synth.isVoiceActive(1) || !synth.isVoiceActive(2)) {
        std::cerr << "code poly note release failed\n";
        return 1;
    }
    synth.allNotesOff();

    settings.voiceMode = resid_vst::VoiceMode::mono;
    settings.voice[0].glideEnabled = true;
    settings.voice[0].glideTimeMs = 100.0f;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    const auto glideStart = synth.debugVoiceFrequency(0);
    synth.noteOn(72, 1.0f);
    const auto glideImmediate = synth.debugVoiceFrequency(0);
    for (int i = 0; i < 2400; ++i) synth.nextSample();
    const auto glideMiddle = synth.debugVoiceFrequency(0);
    for (int i = 0; i < 2400; ++i) synth.nextSample();
    const auto glideEnd = synth.debugVoiceFrequency(0);
    if (glideImmediate != glideStart || !(glideStart < glideMiddle && glideMiddle < glideEnd)) {
        std::cerr << "mono legato glide failed: " << glideStart << ", " << glideImmediate
                  << ", " << glideMiddle << ", " << glideEnd << "\n";
        return 1;
    }
    synth.allNotesOff();
    settings.voice[0].glideEnabled = false;

    settings.voiceMode = resid_vst::VoiceMode::codePoly;
    settings.codeInstrument.filterLength = 2;
    settings.codeInstrument.filter[0] = { 0x9b, 0xc0, 0x00 };
    settings.codeInstrument.filter[1] = { 0x08, 0x80, 0x00 };
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    for (int i = 0; i < 960; ++i) {
        synth.nextSample();
    }
    if (synth.debugFilterCutoff() != 1408) {
        std::cerr << "code poly first filter sweep failed: cutoff=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    synth.noteOn(64, 1.0f);
    synth.nextSample();
    if (synth.debugFilterCutoff() != 1536) {
        std::cerr << "code poly newest voice did not restart filter: cutoff=" << synth.debugFilterCutoff() << "\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.filterLength = 0;

    settings.voiceMode = resid_vst::VoiceMode::code;
    settings.codeInstrument.wfLength = 1;
    settings.codeInstrument.wf[0] = { 0x41, 0x7f, 0xff };
    settings.codeInstrument.chordLength = 1;
    settings.codeInstrument.chord[0] = { 0x00, 0x04, 0x07, 0xfe };
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);
    synth.nextSample();
    const auto chordRoot = synth.debugVoiceFrequency(0);
    for (int i = 0; i < 960; ++i) synth.nextSample();
    const auto chordThird = synth.debugVoiceFrequency(0);
    for (int i = 0; i < 960; ++i) synth.nextSample();
    const auto chordFifth = synth.debugVoiceFrequency(0);
    if (!(chordRoot < chordThird && chordThird < chordFifth)) {
        std::cerr << "code chord did not advance sequentially: "
                  << chordRoot << ", " << chordThird << ", " << chordFifth << "\n";
        return 1;
    }
    synth.allNotesOff();
    settings.codeInstrument.chordLength = 0;

    settings.voiceMode = resid_vst::VoiceMode::mono;
    synth.setSettings(settings);
    synth.noteOn(60, 1.0f);

    resid_vst::SidSynth shakerSynth;
    shakerSynth.prepare(48000.0);
    resid_vst::SidSynthSettings shakerSettings;
    shakerSettings.outputGain = 1.0f;
    shakerSettings.chipModel = resid_vst::ChipModel::mos8580;
    shakerSettings.voiceMode = resid_vst::VoiceMode::code;
    shakerSettings.codeInstrument.adsr = 0xe048;
    shakerSettings.codeInstrument.vibrato = 0x9000;
    shakerSettings.codeInstrument.wfLength = 6;
    shakerSettings.codeInstrument.wf[0] = { 0x81, 0x00, 0x00 };
    shakerSettings.codeInstrument.wf[1] = { 0x81, 0x04, 0x00 };
    shakerSettings.codeInstrument.wf[2] = { 0x81, 0x00, 0x00 };
    shakerSettings.codeInstrument.wf[3] = { 0x81, 0x03, 0x00 };
    shakerSettings.codeInstrument.wf[4] = { 0x81, 0x00, 0x00 };
    shakerSettings.codeInstrument.wf[5] = { 0x80, 0x00, 0x00 };
    shakerSynth.setSettings(shakerSettings);
    shakerSynth.noteOn(60, 1.0f);
    float shakerPeak = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        shakerPeak = std::max(shakerPeak, std::abs(shakerSynth.nextSample()));
    }
    if (shakerPeak < 0.1f) {
        std::cerr << "shaker-horrr playback failed: peak=" << shakerPeak
                  << " control=" << static_cast<int>(shakerSynth.debugVoiceControl(0)) << "\n";
        return 1;
    }

    resid_vst::SidSynth snare4xSynth;
    snare4xSynth.prepare(48000.0);
    auto snare4xSettings = shakerSettings;
    snare4xSettings.codeInstrument.adsr = 0x0034;
    snare4xSettings.codeInstrument.vibrato = 0x0000;
    snare4xSettings.codeInstrument.playbackRate = 4;
    snare4xSettings.codeInstrument.wfLength = 5;
    snare4xSettings.codeInstrument.wf[0] = { 0x81, 0xcf, 0x00 };
    snare4xSettings.codeInstrument.wf[1] = { 0x81, 0xcf, 0x00 };
    snare4xSettings.codeInstrument.wf[2] = { 0x81, 0xca, 0x00 };
    snare4xSettings.codeInstrument.wf[3] = { 0x81, 0xca, 0x00 };
    snare4xSettings.codeInstrument.wf[4] = { 0x41, 0xab, 0x00 };
    snare4xSynth.setSettings(snare4xSettings);
    snare4xSynth.noteOn(60, 1.0f);
    for (int i = 0; i < 2160; ++i) snare4xSynth.nextSample();
    if (snare4xSynth.debugVoiceControl(0) != 0x41) {
        std::cerr << "snare-4x multispeed playback failed: control="
                  << static_cast<int>(snare4xSynth.debugVoiceControl(0)) << "\n";
        return 1;
    }

    resid_vst::SidSynth monoPulseSynth;
    resid_vst::SidSynth codePulseSynth;
    monoPulseSynth.prepare(48000.0);
    codePulseSynth.prepare(48000.0);
    resid_vst::SidSynthSettings monoPulseSettings;
    monoPulseSettings.filterMode = resid_vst::FilterMode::off;
    monoPulseSettings.voice[0].waveform = resid_vst::Waveform::pulse;
    auto codePulseSettings = monoPulseSettings;
    codePulseSettings.voiceMode = resid_vst::VoiceMode::code;
    codePulseSettings.codeInstrument.adsr = 0x00f0;
    codePulseSettings.codeInstrument.defaultFilterMode = 0x00;
    codePulseSettings.codeInstrument.wfLength = 1;
    codePulseSettings.codeInstrument.wf[0] = { 0x41, 0x00, 0x00 };
    monoPulseSynth.setSettings(monoPulseSettings);
    codePulseSynth.setSettings(codePulseSettings);
    monoPulseSynth.noteOn(60, 1.0f);
    codePulseSynth.noteOn(60, 1.0f);
    if (monoPulseSynth.debugVoiceControl(0) != 0x41 || codePulseSynth.debugVoiceControl(0) != 0x41
        || monoPulseSynth.debugVoiceFrequency(0) != codePulseSynth.debugVoiceFrequency(0)
        || monoPulseSynth.debugVoicePulseWidth(0) != codePulseSynth.debugVoicePulseWidth(0)
        || monoPulseSynth.debugFilterMode() != codePulseSynth.debugFilterMode()) {
        std::cerr << "plain code pulse does not match mono pulse\n";
        return 1;
    }

    float peak = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        peak = std::max(peak, std::abs(synth.nextSample()));
    }

    synth.noteOff(60);
    float releaseTailPeak = 0.0f;
    for (int i = 0; i < 48000 * 5; ++i) {
        const auto sample = std::abs(synth.nextSample());
        if (i >= 48000 * 4) {
            releaseTailPeak = std::max(releaseTailPeak, sample);
        }
    }

    std::cout << "peak=" << peak << " releaseTailPeak=" << releaseTailPeak << "\n";
    return peak > 0.001f && releaseTailPeak < 0.005f ? 0 : 1;
}
