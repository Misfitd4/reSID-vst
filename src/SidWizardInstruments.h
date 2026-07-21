#pragma once

#include <cstddef>
#include <cstdint>

namespace sidwizard_import {

struct InstrumentCommandRow {
    uint8_t value = 0xff;
    uint8_t arg = 0x00;
    uint8_t aux = 0x00;
};

struct InstrumentChordRow {
    int8_t offsets[3] = { 0, 0, 0 };
    uint8_t next = 0xff;
};

struct InstrumentCommandProgram {
    const InstrumentCommandRow* rows = nullptr;
    uint8_t length = 0;
};

struct InstrumentChordProgram {
    const InstrumentChordRow* rows = nullptr;
    uint8_t length = 0;
};

struct InstrumentPreset {
    const char* name = "off";
    uint16_t waveProgram = 0;
    uint16_t pulseWidthProgram = 0;
    uint16_t filterProgram = 0;
    uint16_t chordProgram = 0;
    uint16_t vibrato = 0;
    uint8_t speed = 0;
    uint8_t vibratoType = 0x10;
    bool absolutePitch = false;
    uint8_t fixedMidiNote = 48;
    bool velocityRelease = false;
    uint8_t unisonVoices = 3;
    bool dualVoiceCore = false;
    uint8_t defaultAttack = 0;
    uint8_t defaultDecay = 4;
    uint8_t defaultSustain = 0x30;
    uint8_t defaultRelease = 4;
    uint16_t defaultPulseWidth = 0x0800;
    uint16_t defaultFilterCutoff = 0;
    uint8_t defaultFilterResonance = 0;
    uint8_t defaultFilterMode = 0;
};

#include "SidWizardInstruments.inc"

constexpr size_t kImportedInstrumentPresetCount =
    sizeof(gImportedInstrumentPresets) / sizeof(gImportedInstrumentPresets[0]);
constexpr size_t kImportedInstrumentProgramCount =
    sizeof(gImportedInstrumentWavePrograms) / sizeof(gImportedInstrumentWavePrograms[0]);
constexpr size_t kImportedInstrumentChordProgramCount =
    sizeof(gImportedInstrumentChordPrograms) / sizeof(gImportedInstrumentChordPrograms[0]);

static_assert(gImportedInstrumentPresets[186].speed == 0x01);
static_assert(gImportedInstrumentPresets[186].vibratoType == 0x10);

} // namespace sidwizard_import
