#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

#include "SidSynth.h"

class ReSidAudioProcessor final : public juce::AudioProcessor {
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    ReSidAudioProcessor();
    ~ReSidAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    static constexpr int scopeSize = 512;
    using ScopeSnapshot = std::array<float, scopeSize>;
    void copyVoiceScope(int voiceIndex, ScopeSnapshot& destination) const;
    APVTS& parameterState() { return parameters; }

private:
    static APVTS::ParameterLayout createParameterLayout();
    resid_vst::SidSynthSettings readSettings() const;

    APVTS parameters;
    resid_vst::SidSynth synth;
    std::array<std::array<std::atomic<float>, scopeSize>, 3> voiceScopes {};
    std::atomic<int> scopeWriteIndex { 0 };
    float dcBlockLastInput = 0.0f;
    float dcBlockLastOutput = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReSidAudioProcessor)
};
