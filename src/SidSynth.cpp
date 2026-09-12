#include "SidSynth.h"

#include <algorithm>
#include <cmath>

namespace resid_vst {

namespace {
constexpr bool isCodeMode(VoiceMode mode)
{
    return mode == VoiceMode::code || mode == VoiceMode::codePoly;
}
} // namespace

void SidSynth::prepare(double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    configureSid();
}

void SidSynth::reset()
{
    sid.reset();
    cycleRemainder = 0.0;
    heldNotes.clear();
    for (auto& voice : voices) {
        voice = {};
    }
    nextPolyVoice = 0;
    pitchBend = 0.0f;
    modWheel = 0.0f;
    modWheelPhase = 0.0;
    lastVoiceSamples.fill(0.0f);
    for (int i = 0; i < sidVoiceCount; ++i) {
        scopeFilters[static_cast<size_t>(i)].reset();
        scopeOutputFilters[static_cast<size_t>(i)].reset();
    }
    applyGlobalRegisters();
}

void SidSynth::setScopeCaptureEnabled(bool enabled)
{
    if (enabled && !scopeCaptureEnabled) {
        for (int i = 0; i < sidVoiceCount; ++i) {
            auto& filter = scopeFilters[static_cast<size_t>(i)];
            filter.reset();
            filter.writeFC_LO(appliedFilterCutoff & 0x07);
            filter.writeFC_HI(appliedFilterCutoff >> 3);
            filter.writeRES_FILT(appliedFilterResonance | 0x07);
            filter.writeMODE_VOL(appliedFilterMode | 0x0f);
            scopeOutputFilters[static_cast<size_t>(i)].reset();
        }
    }
    scopeCaptureEnabled = enabled;
}

void SidSynth::setSettings(const SidSynthSettings& newSettings)
{
    const auto needsReconfigure = !sidConfigured
        || newSettings.chipModel != configuredChipModel
        || sampleRate != configuredSampleRate;

    settings = newSettings;

    if (needsReconfigure) {
        configureSid();
    } else {
        applyGlobalRegisters();
    }

    for (int i = 0; i < sidVoiceCount; ++i) {
        updateVoice(i);
    }
}

void SidSynth::noteOn(int midiNote, float velocity)
{
    rememberHeldNote(midiNote, velocity);

    if (settings.voiceMode == VoiceMode::unison) {
        const auto legato = voices[0].allocated && voices[0].gate;
        if (legato) {
            for (int i = 0; i < sidVoiceCount; ++i) {
                if (settings.voice[static_cast<size_t>(i)].glideEnabled) {
                    retuneLegatoVoice(i, midiNote, velocity, false);
                } else {
                    triggerVoice(i, midiNote, velocity);
                }
            }
        } else {
            triggerHeldNote(*newestHeldNote());
        }
        return;
    }

    if (settings.voiceMode == VoiceMode::mono || settings.voiceMode == VoiceMode::code) {
        if (settings.voiceMode == VoiceMode::code && settings.codeLegato && voices[0].allocated && voices[0].gate) {
            retuneCodeLegato(midiNote, velocity);
        } else if (settings.voiceMode == VoiceMode::mono && settings.voice[0].glideEnabled
                   && voices[0].allocated && voices[0].gate) {
            retuneLegatoVoice(0, midiNote, velocity, false);
        } else {
            triggerVoice(0, midiNote, velocity);
        }
        for (int i = 1; i < sidVoiceCount; ++i) {
            voices[static_cast<size_t>(i)].gate = false;
            voices[static_cast<size_t>(i)].allocated = false;
            updateVoice(i);
        }
        return;
    }

    const auto voiceIndex = chooseVoice();
    triggerVoice(voiceIndex, midiNote, velocity);
}

void SidSynth::noteOff(int midiNote)
{
    const auto wasHeld = releaseHeldNote(midiNote);

    if (wasHeld && (settings.voiceMode == VoiceMode::mono || settings.voiceMode == VoiceMode::unison || settings.voiceMode == VoiceMode::code)) {
        if (voices[0].gate && voices[0].midiNote != midiNote) {
            return;
        }
        if (const auto* heldNote = newestHeldNote()) {
            if (settings.voiceMode == VoiceMode::code && settings.codeLegato && voices[0].gate) {
                retuneCodeLegato(heldNote->midiNote, heldNote->velocity);
            } else if (settings.voiceMode == VoiceMode::mono && settings.voice[0].glideEnabled && voices[0].gate) {
                retuneLegatoVoice(0, heldNote->midiNote, heldNote->velocity, false);
            } else if (settings.voiceMode == VoiceMode::unison && voices[0].gate) {
                for (int i = 0; i < sidVoiceCount; ++i) {
                    if (settings.voice[static_cast<size_t>(i)].glideEnabled) {
                        retuneLegatoVoice(i, heldNote->midiNote, heldNote->velocity, false);
                    } else {
                        triggerVoice(i, heldNote->midiNote, heldNote->velocity);
                    }
                }
            } else {
                triggerHeldNote(*heldNote);
            }
        } else {
            releaseStackedVoices();
        }
        return;
    }

    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& voice = voices[static_cast<size_t>(i)];
        if (voice.allocated && voice.gate && voice.midiNote == midiNote) {
            voice.gate = false;
            applyCodeGateOffPointers(i);
            writeVoiceGate(i, false);
        }
    }
}

void SidSynth::allNotesOff()
{
    heldNotes.clear();
    nextPolyVoice = 0;
    for (int i = 0; i < sidVoiceCount; ++i) {
        voices[static_cast<size_t>(i)].gate = false;
        voices[static_cast<size_t>(i)].allocated = false;
        writeVoiceGate(i, false);
    }
}

void SidSynth::setPitchBend(float normalizedBend)
{
    pitchBend = std::clamp(normalizedBend, -1.0f, 1.0f);
    for (int i = 0; i < sidVoiceCount; ++i) {
        updateVoice(i);
    }
}

void SidSynth::setModWheel(float normalizedValue)
{
    modWheel = std::clamp(normalizedValue, 0.0f, 1.0f);
    applyGlobalRegisters();
    for (int i = 0; i < sidVoiceCount; ++i) {
        updateVoice(i);
    }
}

float SidSynth::nextSample()
{
    advanceGlide();
    advanceCodeVibrato();
    advanceCodeTables();
    advancePerformanceModulation();
    advanceWavetables();

    cycleRemainder += c64PalClock / sampleRate;
    const auto cycles = static_cast<reSID::cycle_count>(cycleRemainder);
    cycleRemainder -= static_cast<double>(cycles);

    if (cycles > 0) {
        sid.clock(cycles);
    }

    constexpr auto voiceOutputScale = 1.0f / (2048.0f * 255.0f);
    for (int voiceIndex = 0; voiceIndex < sidVoiceCount; ++voiceIndex) {
        const auto index = static_cast<size_t>(voiceIndex);
        const auto rawVoice = sid.voice_output(static_cast<unsigned int>(voiceIndex));
        auto scopeSample = static_cast<float>(rawVoice) * voiceOutputScale;
        if (scopeCaptureEnabled && appliedFilterMode != 0) {
            auto& filter = scopeFilters[index];
            auto& outputFilter = scopeOutputFilters[index];
            if (cycles > 0) {
                filter.clock(cycles, voiceIndex == 0 ? rawVoice : 0,
                             voiceIndex == 1 ? rawVoice : 0, voiceIndex == 2 ? rawVoice : 0);
                outputFilter.clock(cycles, filter.output());
            }
            scopeSample = static_cast<float>(outputFilter.output()) / 32768.0f;
        }
        lastVoiceSamples[index] = std::clamp(
            scopeSample,
            -1.0f,
            1.0f);
    }

    constexpr auto residOutputScale = 1.0f / 32768.0f;
    return std::clamp(static_cast<float>(sid.output()) * residOutputScale * settings.outputGain, -1.0f, 1.0f);
}

uint16_t SidSynth::debugVoiceFrequency(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= sidVoiceCount) {
        return 0;
    }

    const auto& voice = voices[static_cast<size_t>(voiceIndex)];
    if (!voice.allocated) {
        return 0;
    }

    const auto& voiceSettings = settings.voice[static_cast<size_t>(voiceIndex)];
    const auto isCodeAudible = isCodeMode(settings.voiceMode) && (voice.codeControl & 0xf0) != 0;
    if (!isCodeAudible && !voiceProducesWaveform(voiceSettings)) {
        return 0;
    }

    const auto wheelVibrato = !isCodeMode(settings.voiceMode) && settings.modWheelTarget == ModWheelTarget::vibrato
        ? std::sin(modWheelPhase) * modWheel * settings.modWheelDepth * 100.0
        : 0.0;
    const auto performancePitchCents = isCodeMode(settings.voiceMode)
        ? 0.0
        : pitchBend * settings.pitchBendRangeSemitones * 100.0 + wheelVibrato;
    const auto addCodeDetune = [&voice](uint16_t frequency) {
        return static_cast<uint16_t>(frequency + voice.codeDetune);
    };
    if (isCodeMode(settings.voiceMode) && voice.codePitchOverride) {
        return addCodeDetune(noteValueToSidFrequency((voice.codePitchCents + voice.codeVibratoCents + performancePitchCents) / 100.0));
    }

    const auto baseNote = isCodeMode(settings.voiceMode) && settings.codeInstrument.absolutePitch
        ? static_cast<double>(settings.codeInstrument.fixedMidiNote)
        : voice.glideNote;
    const auto tableOffset = isCodeMode(settings.voiceMode) ? voice.codeSemitoneOffset : 0;
    const auto semitoneOffset = isCodeMode(settings.voiceMode)
        ? tableOffset
        : (voice.gate ? wavetableSemitoneOffset(voiceSettings.wavetable, voice.wavetableStep) : 0);
    const auto octave = isCodeMode(settings.voiceMode) ? 0 : voiceSettings.octave;
    const auto detune = (isCodeMode(settings.voiceMode) ? 0.0f : voiceSettings.detuneCents)
        + (isCodeMode(settings.voiceMode) ? voice.codeVibratoCents : 0.0f)
        + static_cast<float>(performancePitchCents);
    const auto frequency = noteValueToSidFrequency(baseNote + octave * 12.0 + semitoneOffset + detune / 100.0);
    return isCodeMode(settings.voiceMode) ? addCodeDetune(frequency) : frequency;
}

uint8_t SidSynth::debugVoiceControl(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= sidVoiceCount) {
        return 0;
    }

    return voices[static_cast<size_t>(voiceIndex)].lastControl;
}

uint16_t SidSynth::debugVoicePulseWidth(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= sidVoiceCount) {
        return 0;
    }
    return appliedPulseWidths[static_cast<size_t>(voiceIndex)];
}

float SidSynth::debugVoiceSample(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= sidVoiceCount) {
        return 0.0f;
    }
    return lastVoiceSamples[static_cast<size_t>(voiceIndex)];
}

bool SidSynth::isVoiceActive(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= sidVoiceCount) {
        return false;
    }

    const auto& voice = voices[static_cast<size_t>(voiceIndex)];
    const auto& voiceSettings = settings.voice[static_cast<size_t>(voiceIndex)];
    const auto isCodeAudible = isCodeMode(settings.voiceMode) && (voice.codeControl & 0xf0) != 0;
    return voice.allocated && voice.gate && (isCodeAudible || voiceProducesWaveform(voiceSettings));
}

bool SidSynth::hasActiveVoice() const
{
    for (int i = 0; i < sidVoiceCount; ++i) {
        if (isVoiceActive(i)) {
            return true;
        }
    }

    return false;
}

void SidSynth::configureSid()
{
    sid.set_chip_model(settings.chipModel == ChipModel::mos8580 ? reSID::MOS8580 : reSID::MOS6581);
    sid.set_envelope_quirks_enabled(false);
    sid.set_sampling_parameters(c64PalClock, reSID::SAMPLE_INTERPOLATE, sampleRate);
    sid.enable_external_filter(true);
    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& filter = scopeFilters[static_cast<size_t>(i)];
        filter.set_chip_model(settings.chipModel == ChipModel::mos8580 ? reSID::MOS8580 : reSID::MOS6581);
        filter.set_voice_mask(1 << i);
        filter.reset();
        scopeOutputFilters[static_cast<size_t>(i)].reset();
    }
    configuredChipModel = settings.chipModel;
    configuredSampleRate = sampleRate;
    sidConfigured = true;
    applyGlobalRegisters();
}

void SidSynth::applyGlobalRegisters()
{
    auto cutoff = sid11Bit(settings.cutoff);
    auto resonance = static_cast<uint8_t>(sidNibble(settings.resonance) << 4);
    if (isCodeMode(settings.voiceMode)) {
        cutoff = settings.codeInstrument.defaultFilterCutoff;
        resonance = static_cast<uint8_t>(settings.codeInstrument.defaultFilterResonance << 4);
        const VoiceState* source = nullptr;
        for (const auto& voice : voices) {
            if (voice.allocated && (source == nullptr || voice.age > source->age)) {
                source = &voice;
            }
        }
        if (source != nullptr) {
            cutoff = static_cast<uint16_t>(std::clamp<int>(source->codeFilterCutoff, 0, 2047));
            resonance = static_cast<uint8_t>(std::clamp<int>(source->codeFilterResonance, 0, 15) << 4);
        }
    }
    if (!isCodeMode(settings.voiceMode) && settings.modWheelTarget == ModWheelTarget::filter) {
        const auto amount = std::clamp(modWheel * settings.modWheelDepth, 0.0f, 1.0f);
        cutoff = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(std::lround(cutoff + (2047 - cutoff) * amount)), 0, 2047));
    }

    uint8_t mode = 0;
    switch (settings.filterMode) {
    case FilterMode::lowpass:
        mode = 0x10;
        break;
    case FilterMode::bandpass:
        mode = 0x20;
        break;
    case FilterMode::highpass:
        mode = 0x40;
        break;
    case FilterMode::notch:
        mode = 0x70;
        break;
    case FilterMode::off:
        mode = 0;
        break;
    }
    if (isCodeMode(settings.voiceMode)) {
        mode = settings.codeInstrument.defaultFilterMode;
        const VoiceState* source = nullptr;
        for (const auto& voice : voices) {
            if (voice.allocated && (source == nullptr || voice.age > source->age)) {
                source = &voice;
            }
        }
        if (source != nullptr) {
            mode = source->codeFilterMode;
        }
    }

    sid.enable_filter(mode != 0);
    appliedFilterCutoff = cutoff;
    appliedFilterResonance = resonance;
    const auto filterWasOff = appliedFilterMode == 0;
    appliedFilterMode = mode;

    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& filter = scopeFilters[static_cast<size_t>(i)];
        if (filterWasOff && mode != 0) {
            filter.reset();
            scopeOutputFilters[static_cast<size_t>(i)].reset();
        }
        filter.enable_filter(mode != 0);
        filter.writeFC_LO(cutoff & 0x07);
        filter.writeFC_HI(cutoff >> 3);
        filter.writeRES_FILT(resonance | 0x07);
        filter.writeMODE_VOL(mode | 0x0f);
    }

    sid.write(0x15, cutoff & 0x07);
    sid.write(0x16, cutoff >> 3);
    sid.write(0x17, resonance | 0x07);
    sid.write(0x18, mode | 0x0f);
}

void SidSynth::updateVoice(int voiceIndex)
{
    const auto& voice = voices[static_cast<size_t>(voiceIndex)];
    const auto& voiceSettings = settings.voice[static_cast<size_t>(voiceIndex)];
    const auto base = static_cast<uint8_t>(voiceIndex * 7);
    const auto isActive = isCodeMode(settings.voiceMode)
        ? (voice.codeControl & 0xf0) != 0
        : voiceProducesWaveform(voiceSettings);
    const auto baseNote = isCodeMode(settings.voiceMode) && settings.codeInstrument.absolutePitch
        ? static_cast<double>(settings.codeInstrument.fixedMidiNote)
        : voice.glideNote;
    const auto tableOffset = isCodeMode(settings.voiceMode) ? voice.codeSemitoneOffset : 0;
    const auto semitoneOffset = isCodeMode(settings.voiceMode)
        ? tableOffset
        : (voice.gate && isActive ? wavetableSemitoneOffset(voiceSettings.wavetable, voice.wavetableStep) : 0);
    const auto wheelVibrato = !isCodeMode(settings.voiceMode) && settings.modWheelTarget == ModWheelTarget::vibrato
        ? std::sin(modWheelPhase) * modWheel * settings.modWheelDepth * 100.0
        : 0.0;
    const auto performancePitchCents = isCodeMode(settings.voiceMode)
        ? 0.0
        : pitchBend * settings.pitchBendRangeSemitones * 100.0 + wheelVibrato;
    const auto octave = isCodeMode(settings.voiceMode) ? 0 : voiceSettings.octave;
    const auto detune = (isCodeMode(settings.voiceMode) ? 0.0f : voiceSettings.detuneCents)
        + (isCodeMode(settings.voiceMode) ? voice.codeVibratoCents : 0.0f)
        + static_cast<float>(performancePitchCents);
    auto frequency = voice.allocated && isActive
        ? (isCodeMode(settings.voiceMode) && voice.codePitchOverride
                ? noteValueToSidFrequency((voice.codePitchCents + voice.codeVibratoCents + performancePitchCents) / 100.0)
                : noteValueToSidFrequency(baseNote + octave * 12.0 + semitoneOffset + detune / 100.0))
        : 0;
    if (frequency != 0 && isCodeMode(settings.voiceMode)) {
        frequency = static_cast<uint16_t>(frequency + voice.codeDetune);
    }
    auto pulseWidth = isCodeMode(settings.voiceMode)
        ? voice.codePulseWidth
        : sid12Bit(voiceSettings.pulseWidth);
    if (!isCodeMode(settings.voiceMode) && settings.modWheelTarget == ModWheelTarget::pulseWidth) {
        const auto amount = std::clamp(modWheel * settings.modWheelDepth, 0.0f, 1.0f);
        pulseWidth = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(std::lround(pulseWidth + (4095 - pulseWidth) * amount)), 0, 4095));
    }

    sid.write(base + 0, frequency & 0xff);
    sid.write(base + 1, frequency >> 8);
    sid.write(base + 2, pulseWidth & 0xff);
    sid.write(base + 3, (pulseWidth >> 8) & 0x0f);
    appliedPulseWidths[static_cast<size_t>(voiceIndex)] = pulseWidth;
    if (isCodeMode(settings.voiceMode)) {
        const auto ad = static_cast<uint8_t>((settings.codeInstrument.adsr >> 8) & 0xff);
        auto sr = static_cast<uint8_t>(settings.codeInstrument.adsr & 0xff);
        if (settings.codeInstrument.velocityRelease) {
            sr = static_cast<uint8_t>((sr & 0xf0) | velocityReleaseNibble(voice.velocity));
        }
        sid.write(base + 5, ad);
        sid.set_voice_clean_sustain_level(static_cast<unsigned int>(voiceIndex), static_cast<unsigned int>((sr >> 4) * 17));
        sid.write(base + 6, sr);
    } else {
        sid.write(base + 5, adsrByte(voiceSettings.attack, voiceSettings.decay));
        sid.set_voice_clean_sustain_level(static_cast<unsigned int>(voiceIndex), cleanSustainByte(voiceSettings.sustain));
        sid.write(base + 6, adsrByte(voiceSettings.sustain, voiceSettings.release));
    }
    writeVoiceGate(voiceIndex, voice.gate);
}

void SidSynth::advanceWavetables()
{
    if (isCodeMode(settings.voiceMode)) {
        return;
    }

    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& voice = voices[static_cast<size_t>(i)];
        const auto& voiceSettings = settings.voice[static_cast<size_t>(i)];

        if (!voice.gate || voiceSettings.wavetable == WavetableMode::off) {
            continue;
        }

        voice.wavetableSamplesUntilStep -= 1.0;
        if (voice.wavetableSamplesUntilStep > 0.0) {
            continue;
        }

        const auto rate = std::clamp(voiceSettings.wavetableRate, 0.1f, 60.0f);
        voice.wavetableSamplesUntilStep += std::max(1.0, sampleRate / static_cast<double>(rate));
        voice.wavetableStep = (voice.wavetableStep + 1) & 0x03;
        updateVoice(i);
    }
}

void SidSynth::advanceCodeTables()
{
    if (!isCodeMode(settings.voiceMode)) {
        return;
    }

    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& voice = voices[static_cast<size_t>(i)];
        // SID-Wizard keeps WF/ARP, pulse and filter programs running after
        // gate-off, throughout the SID envelope's release phase.
        if (!voice.allocated) {
            continue;
        }

        voice.codeSamplesUntilStep -= 1.0;
        if (voice.codeSamplesUntilStep > 0.0) {
            continue;
        }

        const auto playbackRate = std::max(1, static_cast<int>(settings.codeInstrument.playbackRate));
        const auto mainFrame = playbackRate == 1 || voice.codeMultispeedPhase == 0;
        stepCodeVoice(i, mainFrame);
        voice.codeMultispeedPhase = (voice.codeMultispeedPhase + 1) % playbackRate;
        voice.codeSamplesUntilStep += std::max(1.0, sampleRate / (50.0 * playbackRate));
    }
}

void SidSynth::advanceCodeVibrato()
{
    if (!isCodeMode(settings.voiceMode)) {
        return;
    }

    const auto intensity = static_cast<int>((settings.codeInstrument.vibrato >> 12) & 0x0f);
    const auto rate = static_cast<int>((settings.codeInstrument.vibrato >> 8) & 0x0f);
    const auto delayTicks = static_cast<int>(settings.codeInstrument.vibrato & 0xff);
    if (intensity == 0 || rate == 0) {
        return;
    }

    const auto samplesPerFrame = std::max(1.0, sampleRate / 50.0);
    const auto delaySamples = samplesPerFrame * static_cast<double>(delayTicks);
    const auto cycleFrames = static_cast<double>(rate * 2);

    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& voice = voices[static_cast<size_t>(i)];
        if (!voice.allocated) {
            continue;
        }

        const auto previous = voice.codeVibratoCents;
        voice.codeSamplesSinceTrigger += 1.0;
        if (voice.codeSamplesSinceTrigger <= delaySamples) {
            voice.codeVibratoCents = 0.0f;
        } else {
            voice.codeVibratoSamplesUntilStep -= 1.0;
            if (voice.codeVibratoSamplesUntilStep <= 0.0) {
                voice.codeVibratoSamplesUntilStep += samplesPerFrame;
                voice.codeVibratoPhase = std::fmod(voice.codeVibratoPhase + 1.0, cycleFrames);

                const auto halfPhase = voice.codeVibratoPhase / static_cast<double>(rate);
                const auto triangle = halfPhase <= 1.0 ? halfPhase : 2.0 - halfPhase;
                auto shaped = triangle * 2.0 - 1.0;
                if (settings.codeInstrument.vibratoType == 0x20) {
                    shaped = -std::abs(shaped);
                } else if (settings.codeInstrument.vibratoType == 0x30) {
                    shaped = std::abs(shaped);
                } else if (settings.codeInstrument.vibratoType == 0x00) {
                    const auto growth = std::min(1.0,
                        static_cast<double>(delayTicks) * voice.codeSamplesSinceTrigger
                            / (255.0 * samplesPerFrame));
                    shaped *= growth;
                }
                voice.codeVibratoCents = static_cast<float>(shaped * static_cast<double>(intensity * 8));
            }
        }

        if (std::abs(previous - voice.codeVibratoCents) >= 0.1f) {
            updateVoice(i);
        }
    }
}

void SidSynth::advancePerformanceModulation()
{
    if (settings.modWheelTarget != ModWheelTarget::vibrato || modWheel <= 0.0f) {
        return;
    }

    constexpr double twoPi = 6.28318530717958647692;
    constexpr double vibratoRateHz = 5.0;
    modWheelPhase = std::fmod(modWheelPhase + twoPi * vibratoRateHz / sampleRate, twoPi);
    for (int i = 0; i < sidVoiceCount; ++i) {
        if (voices[static_cast<size_t>(i)].allocated) {
            updateVoice(i);
        }
    }
}

void SidSynth::advanceGlide()
{
    for (int i = 0; i < sidVoiceCount; ++i) {
        auto& voice = voices[static_cast<size_t>(i)];
        if (!voice.allocated || voice.glideSamplesRemaining <= 0) {
            continue;
        }
        voice.glideNote += voice.glideStepPerSample;
        --voice.glideSamplesRemaining;
        if (voice.glideSamplesRemaining == 0) {
            voice.glideNote = voice.glideTargetNote;
        }
        updateVoice(i);
    }
}

void SidSynth::resetCodeState(int voiceIndex)
{
    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    voice.codeSamplesUntilStep = 0.0;
    voice.codeWfFramesUntilStep = 0;
    voice.codeMultispeedPhase = 0;
    voice.codeWfRow = 0;
    voice.codePulseRow = 0;
    voice.codeFilterRow = 0;
    voice.codeChordRow = 0;
    voice.codeChordNote = 0;
    voice.codePulseRepeat = 0;
    voice.codePulseDelta = 0;
    voice.codeFilterRepeat = 0;
    voice.codeFilterDelta = 0;
    voice.codeSemitoneOffset = 0;
    voice.codePitchCents = static_cast<double>(voice.midiNote) * 100.0;
    voice.codeTargetPitchCents = voice.codePitchCents;
    voice.codeVibratoPhase = 0.0;
    voice.codeVibratoSamplesUntilStep = 0.0;
    voice.codeSamplesSinceTrigger = 0.0;
    voice.codePitchOverride = false;
    voice.codeDetune = 0;
    voice.codeVibratoCents = 0.0f;
    const auto firstWaveform = settings.codeInstrument.wfLength > 0
        ? settings.codeInstrument.wf[0].waveform
        : static_cast<uint8_t>(0x41);
    voice.codeControl = firstWaveform >= 0x10 && firstWaveform < 0xfe ? firstWaveform : 0x41;
    voice.codePulseWidth = settings.codeInstrument.defaultPulseWidth;
    voice.codeFilterCutoff = settings.codeInstrument.defaultFilterCutoff;
    voice.codeFilterResonance = settings.codeInstrument.defaultFilterResonance;
    voice.codeFilterMode = settings.codeInstrument.defaultFilterMode;
}

void SidSynth::stepCodeVoice(int voiceIndex, bool mainFrame)
{
    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    const auto& instrument = settings.codeInstrument;

    if (mainFrame || (instrument.speed & 0x80) != 0) {
        stepCodeFilter(voice);
    }
    if (mainFrame || (instrument.speed & 0x40) != 0) {
        stepCodePulse(voice);
    }

    if (instrument.wfLength > 0 && voice.codeWfRow < instrument.wfLength) {
        if (voice.codeWfFramesUntilStep > 0) {
            --voice.codeWfFramesUntilStep;
        } else {
            voice.codeWfFramesUntilStep = instrument.speed & 0x3f;

            // SID-Wizard resolves a WF-table jump and plays its target row in
            // the same tick. Bound the loop to protect malformed user tables.
            for (int redirects = 0; redirects <= instrument.wfLength; ++redirects) {
                const auto rowIndex = std::clamp(voice.codeWfRow, 0, SidCodeInstrument::rowCount - 1);
                const auto row = instrument.wf[static_cast<size_t>(rowIndex)];
                if (row.waveform == 0xff) {
                    voice.codeWfRow = instrument.wfLength;
                    break;
                }
                if (row.waveform == 0xfe) {
                    const auto target = row.arp < instrument.wfLength ? row.arp : voice.codeWfRow;
                    if (target == voice.codeWfRow) {
                        break;
                    }
                    voice.codeWfRow = target;
                    continue;
                }

                if (row.waveform < 0x10) {
                    voice.codeWfFramesUntilStep = row.waveform;
                } else {
                    voice.codeControl = row.waveform;
                }

                if (row.arp != 0x80) {
                    if (codeArpIsAbsolute(row.arp)) {
                        const auto target = static_cast<double>(codeArpAbsoluteNote(row.arp)) * 100.0;
                        voice.codePitchCents = target;
                        voice.codeTargetPitchCents = target;
                        voice.codePitchOverride = true;
                    } else {
                        voice.codePitchOverride = false;
                        voice.codeSemitoneOffset = codeArpOffset(row.arp, voice, instrument);
                    }
                    if (row.arp == 0x7f && instrument.chordLength > 0) {
                        ++voice.codeChordNote;
                        if (voice.codeChordNote >= 3) {
                            voice.codeChordNote = 0;
                            const auto chord = instrument.chord[static_cast<size_t>(std::clamp(voice.codeChordRow, 0, SidCodeInstrument::rowCount - 1))];
                            if (chord.next == 0xfe) {
                                voice.codeChordRow = 0;
                            } else if (chord.next != 0xff) {
                                voice.codeChordRow = chord.next % std::max(1, static_cast<int>(instrument.chordLength));
                            }
                        }
                    }
                }

                if (row.detune != 0xff) {
                    voice.codeDetune = row.detune;
                }
                // ARP $7F delegates pitch to the chord table and deliberately
                // holds the current WF row until the chord returns.
                if (row.arp != 0x7f) {
                    ++voice.codeWfRow;
                }
                break;
            }
        }
    }

    updateVoice(voiceIndex);
}

void SidSynth::stepCodePulse(VoiceState& voice)
{
    const auto& instrument = settings.codeInstrument;
    if (instrument.pulseLength == 0) {
        return;
    }

    if (voice.codePulseRow >= instrument.pulseLength) {
        return;
    }

    if (voice.codePulseRepeat > 0) {
        voice.codePulseWidth = static_cast<uint16_t>((voice.codePulseWidth + voice.codePulseDelta) & 0x0fff);
        --voice.codePulseRepeat;
        if (voice.codePulseRepeat == 0) {
            ++voice.codePulseRow;
        }
        return;
    }

    auto row = instrument.pulse[static_cast<size_t>(std::clamp(voice.codePulseRow, 0, SidCodeInstrument::rowCount - 1))];
    if (row.command == 0xff) {
        voice.codePulseRow = instrument.pulseLength;
        return;
    }
    if (row.command == 0xfe) {
        const auto target = row.value < instrument.pulseLength ? row.value : voice.codePulseRow;
        if (target == voice.codePulseRow) {
            return;
        }
        voice.codePulseRow = target;
        voice.codePulseRepeat = 0;
        row = instrument.pulse[static_cast<size_t>(target)];
        // A jump to a sweep only arms that sweep for the next frame. A jump
        // to an absolute row applies it immediately in SID-Wizard.
        if (row.command < 0x80 || row.command >= 0xfe) {
            return;
        }
    }

    if (row.command >= 0x80) {
        const auto keyTrack = signedByte(row.keyTrack) * (voice.midiNote - 60);
        const auto absolute = ((row.command & 0x7f) << 8) | row.value;
        voice.codePulseWidth = static_cast<uint16_t>((absolute + keyTrack) & 0x0fff);
    } else {
        voice.codePulseRepeat = row.command;
        voice.codePulseDelta = signedByte(row.value);
        if (voice.codePulseRepeat == 0) {
            ++voice.codePulseRow;
            return;
        }
        voice.codePulseWidth = static_cast<uint16_t>((voice.codePulseWidth + voice.codePulseDelta) & 0x0fff);
        --voice.codePulseRepeat;
        if (voice.codePulseRepeat > 0) {
            return;
        }
    }
    ++voice.codePulseRow;
}

void SidSynth::stepCodeFilter(VoiceState& voice)
{
    const auto& instrument = settings.codeInstrument;
    if (instrument.filterLength == 0) {
        return;
    }

    if (voice.codeFilterRow >= instrument.filterLength) {
        return;
    }

    if (voice.codeFilterRepeat > 0) {
        voice.codeFilterCutoff = static_cast<uint16_t>((voice.codeFilterCutoff + voice.codeFilterDelta) & 0x07ff);
        --voice.codeFilterRepeat;
        if (voice.codeFilterRepeat == 0) {
            ++voice.codeFilterRow;
        }
        applyGlobalRegisters();
        return;
    }

    auto row = instrument.filter[static_cast<size_t>(std::clamp(voice.codeFilterRow, 0, SidCodeInstrument::rowCount - 1))];
    if (row.command == 0xff) {
        voice.codeFilterRow = instrument.filterLength;
        return;
    }
    if (row.command == 0xfe) {
        const auto target = row.value < instrument.filterLength ? row.value : voice.codeFilterRow;
        if (target == voice.codeFilterRow) {
            return;
        }
        voice.codeFilterRow = target;
        voice.codeFilterRepeat = 0;
        row = instrument.filter[static_cast<size_t>(target)];
        if (row.command < 0x80 || row.command >= 0xfe) {
            return;
        }
    }

    if (row.command >= 0x80) {
        const auto keyTrackAmount = row.keyTrack >= 0x80 && row.keyTrack < 0x90 ? 0 : signedByte(row.keyTrack);
        const auto keyTrack = keyTrackAmount * (voice.midiNote - 60);
        voice.codeFilterCutoff = static_cast<uint16_t>(std::clamp<int>((row.value << 3) + keyTrack, 0, 2047));
        voice.codeFilterResonance = static_cast<uint8_t>(row.command & 0x0f);
        voice.codeFilterMode = codeFilterModeBits(row.command);
    } else {
        voice.codeFilterRepeat = row.command;
        voice.codeFilterDelta = signedByte(row.value);
        if (voice.codeFilterRepeat == 0) {
            ++voice.codeFilterRow;
            applyGlobalRegisters();
            return;
        }
        voice.codeFilterCutoff = static_cast<uint16_t>((voice.codeFilterCutoff + voice.codeFilterDelta) & 0x07ff);
        --voice.codeFilterRepeat;
        if (voice.codeFilterRepeat > 0) {
            applyGlobalRegisters();
            return;
        }
    }
    ++voice.codeFilterRow;
    applyGlobalRegisters();
}

void SidSynth::writeVoiceGate(int voiceIndex, bool gate)
{
    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    const auto& voiceSettings = settings.voice[static_cast<size_t>(voiceIndex)];
    const auto base = static_cast<uint8_t>(voiceIndex * 7);
    const auto isCodeAudible = isCodeMode(settings.voiceMode) && (voice.codeControl & 0xf0) != 0;
    const auto effectiveGate = gate && (isCodeAudible || voiceProducesWaveform(voiceSettings));
    const auto control = isCodeMode(settings.voiceMode)
        ? (gate ? voice.codeControl : static_cast<uint8_t>(voice.codeControl & 0xfe))
        : static_cast<uint8_t>(waveformBits(wavetableWaveform(voiceSettings, voice.wavetableStep))
                               | (effectiveGate ? 0x01 : 0x00));
    voice.lastControl = control;
    sid.write(base + 4, control);
}

void SidSynth::triggerVoice(int voiceIndex, int midiNote, float velocity)
{
    auto& voice = voices[static_cast<size_t>(voiceIndex)];

    if (voice.gate) {
        writeVoiceGate(voiceIndex, false);
    }

    voice.midiNote = midiNote;
    voice.velocity = std::clamp(velocity, 0.0f, 1.0f);
    voice.age = ++noteCounter;
    voice.wavetableStep = 0;
    voice.wavetableSamplesUntilStep = 0.0;
    voice.allocated = true;
    voice.gate = true;
    voice.glideNote = static_cast<double>(midiNote);
    voice.glideTargetNote = voice.glideNote;
    voice.glideStepPerSample = 0.0;
    voice.glideSamplesRemaining = 0;
    resetCodeState(voiceIndex);

    updateVoice(voiceIndex);
}

void SidSynth::triggerHeldNote(const HeldNote& heldNote)
{
    if (settings.voiceMode == VoiceMode::unison) {
        for (int i = 0; i < sidVoiceCount; ++i) {
            triggerVoice(i, heldNote.midiNote, heldNote.velocity);
        }
        return;
    }

    triggerVoice(0, heldNote.midiNote, heldNote.velocity);
    for (int i = 1; i < sidVoiceCount; ++i) {
        voices[static_cast<size_t>(i)].gate = false;
        voices[static_cast<size_t>(i)].allocated = false;
        updateVoice(i);
    }
}

void SidSynth::retuneCodeLegato(int midiNote, float velocity)
{
    retuneLegatoVoice(0, midiNote, velocity, true);
}

void SidSynth::retuneLegatoVoice(int voiceIndex, int midiNote, float velocity, bool preserveCodeTables)
{
    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    voice.midiNote = midiNote;
    voice.velocity = std::clamp(velocity, 0.0f, 1.0f);
    voice.age = ++noteCounter;
    const auto& voiceSettings = settings.voice[static_cast<size_t>(voiceIndex)];
    if (voiceSettings.glideEnabled && voiceSettings.glideTimeMs > 0.0f) {
        const auto samples = std::max(1, static_cast<int>(std::lround(sampleRate * voiceSettings.glideTimeMs / 1000.0)));
        voice.glideTargetNote = static_cast<double>(midiNote);
        voice.glideStepPerSample = (voice.glideTargetNote - voice.glideNote) / samples;
        voice.glideSamplesRemaining = samples;
    } else {
        voice.glideNote = static_cast<double>(midiNote);
        voice.glideTargetNote = voice.glideNote;
        voice.glideStepPerSample = 0.0;
        voice.glideSamplesRemaining = 0;
    }
    if (preserveCodeTables && !voice.codePitchOverride) {
        voice.codePitchCents = static_cast<double>(midiNote) * 100.0;
        voice.codeTargetPitchCents = voice.codePitchCents;
    }
    updateVoice(voiceIndex);
}

void SidSynth::applyCodeGateOffPointers(int voiceIndex)
{
    if (!isCodeMode(settings.voiceMode)) {
        return;
    }

    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    const auto& instrument = settings.codeInstrument;
    if (instrument.gateOffWfRow < instrument.wfLength) {
        voice.codeWfRow = instrument.gateOffWfRow;
    }
    if (instrument.gateOffPulseRow < instrument.pulseLength) {
        voice.codePulseRow = instrument.gateOffPulseRow;
        voice.codePulseRepeat = 0;
    }
    if (instrument.gateOffFilterRow < instrument.filterLength) {
        voice.codeFilterRow = instrument.gateOffFilterRow;
        voice.codeFilterRepeat = 0;
    }
}

void SidSynth::rememberHeldNote(int midiNote, float velocity)
{
    releaseHeldNote(midiNote);
    heldNotes.push_back({ midiNote, std::clamp(velocity, 0.0f, 1.0f), ++noteCounter });
}

bool SidSynth::releaseHeldNote(int midiNote)
{
    const auto oldSize = heldNotes.size();
    heldNotes.erase(
        std::remove_if(
            heldNotes.begin(),
            heldNotes.end(),
            [midiNote](const HeldNote& heldNote) { return heldNote.midiNote == midiNote; }),
        heldNotes.end());
    return heldNotes.size() != oldSize;
}

const SidSynth::HeldNote* SidSynth::newestHeldNote() const
{
    if (heldNotes.empty()) {
        return nullptr;
    }

    return &*std::max_element(
        heldNotes.begin(),
        heldNotes.end(),
        [](const HeldNote& a, const HeldNote& b) { return a.age < b.age; });
}

void SidSynth::releaseStackedVoices()
{
    const auto voicesToRelease = settings.voiceMode == VoiceMode::mono || settings.voiceMode == VoiceMode::code ? 1 : sidVoiceCount;
    for (int i = 0; i < voicesToRelease; ++i) {
        voices[static_cast<size_t>(i)].gate = false;
        applyCodeGateOffPointers(i);
        writeVoiceGate(i, false);
    }
}

int SidSynth::chooseVoice()
{
    for (int voiceIndex = 0; voiceIndex < sidVoiceCount; ++voiceIndex) {
        if (!voices[static_cast<size_t>(voiceIndex)].gate) {
            nextPolyVoice = (voiceIndex + 1) % sidVoiceCount;
            return voiceIndex;
        }
    }

    const auto voiceIndex = nextPolyVoice;
    nextPolyVoice = (nextPolyVoice + 1) % sidVoiceCount;
    return voiceIndex;
}

uint16_t SidSynth::noteToSidFrequency(int midiNote, int octave, float detuneCents, int semitoneOffset)
{
    const auto clampedNote = std::clamp(midiNote, 0, 127);
    const auto octaveSemitones = std::clamp(octave, -2, 2) * 12;
    const auto note = static_cast<double>(clampedNote + octaveSemitones + semitoneOffset) + (static_cast<double>(detuneCents) / 100.0);
    return noteValueToSidFrequency(note);
}

uint16_t SidSynth::noteValueToSidFrequency(double note)
{
    const auto hz = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    const auto sidValue = std::lround((hz * 16777216.0) / c64PalClock);
    return static_cast<uint16_t>(std::clamp<long>(sidValue, 0, 0xffff));
}

int SidSynth::signedByte(uint8_t value)
{
    return value < 0x80 ? static_cast<int>(value) : static_cast<int>(value) - 0x100;
}

int SidSynth::codeArpOffset(uint8_t arp, const VoiceState& voice, const SidCodeInstrument& instrument)
{
    if (arp == 0x00) {
        return 0;
    }

    if (arp >= 0x01 && arp <= 0x5f) {
        return arp;
    }

    if (arp == 0x7f && instrument.chordLength > 0) {
        const auto row = instrument.chord[static_cast<size_t>(std::clamp(voice.codeChordRow, 0, SidCodeInstrument::rowCount - 1))];
        switch (voice.codeChordNote) {
        case 0:
            return signedByte(row.offset0);
        case 1:
            return signedByte(row.offset1);
        case 2:
        default:
            return signedByte(row.offset2);
        }
    }

    if (arp >= 0xe0) {
        return signedByte(arp);
    }

    return 0;
}

bool SidSynth::codeArpIsAbsolute(uint8_t arp)
{
    return arp >= 0x81 && arp <= 0xdf;
}

int SidSynth::codeArpAbsoluteNote(uint8_t arp)
{
    return std::clamp(static_cast<int>(arp & 0x7f), 0, 127);
}

uint8_t SidSynth::codeFilterModeBits(uint8_t command)
{
    switch ((command >> 4) & 0x0f) {
    case 0x9:
        return 0x10;
    case 0xa:
        return 0x20;
    case 0xb:
        return 0x30;
    case 0xc:
        return 0x40;
    case 0xd:
        return 0x50;
    case 0xe:
        return 0x60;
    case 0xf:
        return 0x70;
    case 0x8:
    default:
        return 0x00;
    }
}

uint8_t SidSynth::velocityReleaseNibble(float velocity)
{
    const auto midiVelocity = static_cast<int>(std::lround(std::clamp(velocity, 0.0f, 1.0f) * 127.0f));
    if (midiVelocity < 32) {
        return 2;
    }
    if (midiVelocity < 64) {
        return 4;
    }
    if (midiVelocity < 96) {
        return 8;
    }
    return 12;
}

int SidSynth::wavetableSemitoneOffset(WavetableMode mode, int step)
{
    switch (mode) {
    case WavetableMode::octavePulse:
        return (step & 1) != 0 ? 12 : 0;
    case WavetableMode::fifthSaw:
        return (step & 1) != 0 ? 7 : 0;
    case WavetableMode::off:
    case WavetableMode::pulseSaw:
    case WavetableMode::pulseNoise:
    default:
        return 0;
    }
}

bool SidSynth::voiceProducesWaveform(const SidVoiceSettings& voiceSettings)
{
    return voiceSettings.wavetable != WavetableMode::off || voiceSettings.waveform != Waveform::off;
}

Waveform SidSynth::wavetableWaveform(const SidVoiceSettings& voiceSettings, int step)
{
    switch (voiceSettings.wavetable) {
    case WavetableMode::pulseSaw:
        return (step & 1) != 0 ? Waveform::saw : Waveform::pulse;
    case WavetableMode::pulseNoise:
        return (step & 1) != 0 ? Waveform::noise : Waveform::pulse;
    case WavetableMode::octavePulse:
        return Waveform::pulse;
    case WavetableMode::fifthSaw:
        return Waveform::saw;
    case WavetableMode::off:
    default:
        return voiceSettings.waveform;
    }
}

uint8_t SidSynth::waveformBits(Waveform waveform)
{
    switch (waveform) {
    case Waveform::off:
        return 0x00;
    case Waveform::triangle:
        return 0x10;
    case Waveform::saw:
        return 0x20;
    case Waveform::noise:
        return 0x80;
    case Waveform::pulse:
    default:
        return 0x40;
    }
}

uint8_t SidSynth::adsrByte(float highNibble, float lowNibble)
{
    return static_cast<uint8_t>((sidNibble(highNibble) << 4) | sidNibble(lowNibble));
}

uint8_t SidSynth::sidNibble(float value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 15.0f), 0l, 15l));
}

uint8_t SidSynth::cleanSustainByte(float value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f), 0l, 255l));
}

uint16_t SidSynth::sid12Bit(float value)
{
    return static_cast<uint16_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 4095.0f), 0l, 4095l));
}

uint16_t SidSynth::sid11Bit(float value)
{
    return static_cast<uint16_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 2047.0f), 0l, 2047l));
}

} // namespace resid_vst
