#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "SidWizardInstruments.h"

namespace {

constexpr auto parameterStateId = "ReSidState";
constexpr auto colourBg = 0xff101214;
constexpr auto colourBgDeep = 0xff090b0d;
constexpr auto colourPanel = 0xff23272b;
constexpr auto colourPanel2 = 0xff30353a;
constexpr auto colourText = 0xffe6ecef;
constexpr auto colourMuted = 0xffa1a7ad;
constexpr auto colourLine = 0xff9aa0a7;
constexpr auto colourAccent = 0xff59f8da;
constexpr auto colourWarn = 0xffffbf68;

constexpr uint8_t rebaseImportedJump(uint8_t target, int row, int length, int tableAddress)
{
    // SidWizard stores table positions as byte addresses. Each command row is
    // three bytes, while targets >= $40 mean "jump to this row".
    if (target >= 0x40) {
        return static_cast<uint8_t>(row);
    }

    const auto byteOffset = static_cast<int>(target) - tableAddress;
    if (byteOffset >= 0 && byteOffset % 3 == 0) {
        const auto localRow = byteOffset / 3;
        if (localRow < length) {
            return static_cast<uint8_t>(localRow);
        }
    }

    return 0;
}

static_assert(rebaseImportedJump(0x1f, 7, 8, 0x10) == 5);
static_assert(rebaseImportedJump(0xd0, 2, 3, 0x10) == 2);

class ReSidAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ReSidAudioProcessorEditor(ReSidAudioProcessor& processor)
        : AudioProcessorEditor(processor),
          audioProcessor(processor),
          parameterState(processor.parameterState()),
          voices {
              std::make_unique<VoicePanel>(processor, processor.parameterState(), 0),
              std::make_unique<VoicePanel>(processor, processor.parameterState(), 1),
              std::make_unique<VoicePanel>(processor, processor.parameterState(), 2)
          },
          codeEditor(processor.parameterState())
    {
        setLookAndFeel(&lookAndFeel);

        configureCombo(chipBox);
        configureCombo(voiceModeBox);
        configureCombo(filterModeBox);
        configureCombo(cleanSilenceBox);
        configureCombo(modWheelTargetBox);
        codeLegatoButton.setButtonText("ON");
        chipBox.addItemList(juce::StringArray { "6581", "8580" }, 1);
        voiceModeBox.addItemList(juce::StringArray { "Mono", "Poly", "Unison", "Code", "Code Poly" }, 1);
        chipBox.setComponentID("chip_selector");
        voiceModeBox.setComponentID("mode_selector");
        filterModeBox.addItemList(juce::StringArray { "Off", "Lowpass", "Bandpass", "Highpass", "Notch" }, 1);
        cleanSilenceBox.addItemList(juce::StringArray { "Off", "On" }, 1);
        modWheelTargetBox.addItemList(juce::StringArray { "Off", "Vibrato", "Filter", "Pulse Width" }, 1);
        configureKnob(cutoffSlider);
        configureKnob(resonanceSlider);
        configureKnob(gainSlider);
        configureKnob(pitchBendRangeSlider);
        configureKnob(modWheelDepthSlider);

        addAndMakeVisible(chipBox);
        addAndMakeVisible(voiceModeBox);
        addAndMakeVisible(filterModeBox);
        addAndMakeVisible(cleanSilenceBox);
        addAndMakeVisible(modWheelTargetBox);
        addAndMakeVisible(codeLegatoButton);
        addAndMakeVisible(cutoffSlider);
        addAndMakeVisible(resonanceSlider);
        addAndMakeVisible(gainSlider);
        addAndMakeVisible(pitchBendRangeSlider);
        addAndMakeVisible(modWheelDepthSlider);

        addLabel("CHIP", chipLabel);
        addLabel("MODE", voiceModeLabel);
        addLabel("FILTER", filterModeLabel);
        addLabel("CLEAN", cleanSilenceLabel);
        addLabel("CUT", cutoffLabel);
        addLabel("RES", resonanceLabel);
        addLabel("GAIN", gainLabel);
        addLabel("BEND", pitchBendRangeLabel);
        addLabel("MOD TARGET", modWheelTargetLabel);
        addLabel("MOD DEPTH", modWheelDepthLabel);
        addLabel("CODE LEGATO", codeLegatoLabel);

        chipAttachment = std::make_unique<APVTS::ComboBoxAttachment>(processor.parameterState(), "chip", chipBox);
        voiceModeAttachment = std::make_unique<APVTS::ComboBoxAttachment>(processor.parameterState(), "voice_mode", voiceModeBox);
        filterModeAttachment = std::make_unique<APVTS::ComboBoxAttachment>(processor.parameterState(), "filter_mode", filterModeBox);
        cleanSilenceAttachment = std::make_unique<APVTS::ComboBoxAttachment>(processor.parameterState(), "clean_silence", cleanSilenceBox);
        cutoffAttachment = std::make_unique<APVTS::SliderAttachment>(processor.parameterState(), "cutoff", cutoffSlider);
        resonanceAttachment = std::make_unique<APVTS::SliderAttachment>(processor.parameterState(), "resonance", resonanceSlider);
        gainAttachment = std::make_unique<APVTS::SliderAttachment>(processor.parameterState(), "gain", gainSlider);
        pitchBendRangeAttachment = std::make_unique<APVTS::SliderAttachment>(processor.parameterState(), "pitch_bend_range", pitchBendRangeSlider);
        modWheelTargetAttachment = std::make_unique<APVTS::ComboBoxAttachment>(processor.parameterState(), "mod_wheel_target", modWheelTargetBox);
        modWheelDepthAttachment = std::make_unique<APVTS::SliderAttachment>(processor.parameterState(), "mod_wheel_depth", modWheelDepthSlider);
        codeLegatoAttachment = std::make_unique<APVTS::ButtonAttachment>(processor.parameterState(), "code_legato", codeLegatoButton);

        for (auto& voice : voices) {
            addAndMakeVisible(*voice);
        }
        addAndMakeVisible(codeEditor);

        setSize(1040, 1170);
        syncControlsFromProcessor();
        startTimerHz(30);
    }

    ~ReSidAudioProcessorEditor() override
    {
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(colourBg));

        auto bounds = getLocalBounds().toFloat();
        graphics.setGradientFill(juce::ColourGradient(
            juce::Colour(0xff262a2f), 0.0f, 0.0f,
            juce::Colour(colourBgDeep), 0.0f, bounds.getBottom(), false));
        graphics.fillRect(bounds);

        graphics.setColour(juce::Colour(0x11ffffff));
        for (int y = 0; y < getHeight(); y += 3) {
            graphics.drawHorizontalLine(y, 0.0f, static_cast<float>(getWidth()));
        }

        auto header = getLocalBounds().removeFromTop(headerHeight).reduced(18, 12);
        graphics.setColour(juce::Colour(colourPanel));
        graphics.fillRect(header);
        graphics.setColour(juce::Colour(colourLine));
        graphics.drawRect(header, 1);

        auto logo = header.removeFromLeft(300);
        graphics.setColour(juce::Colour(colourText));
        graphics.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        graphics.drawText("Oddvolt reSID", logo, juce::Justification::centredLeft);

        auto bars = logo.withLeft(logo.getRight() - 98).withWidth(72).reduced(0, 10);
        const std::array<juce::Colour, 4> barColours {
            juce::Colour(colourAccent), juce::Colour(0xff5b7ea9), juce::Colour(colourWarn), juce::Colour(0xffff7a7a)
        };
        for (int i = 0; i < 4; ++i) {
            graphics.setColour(barColours[static_cast<size_t>(i)]);
            graphics.fillRect(bars.removeFromTop(6));
            bars.removeFromTop(4);
        }

        graphics.setColour(juce::Colour(colourMuted));
        graphics.setFont(juce::FontOptions(13.0f));
        graphics.drawText("v" JucePlugin_VersionString, header.removeFromRight(82), juce::Justification::centredRight);
    }

    void timerCallback() override
    {
        syncControlsFromProcessor();
        codeEditor.syncPresetSelection();
        for (auto& voice : voices) {
            voice->syncControlsFromProcessor();
            voice->repaintScope();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18);
        area.removeFromTop(headerHeight);

        auto top = area.removeFromTop(90);
        layoutLabelled(chipLabel, chipBox, top.removeFromLeft(120));
        layoutLabelled(voiceModeLabel, voiceModeBox, top.removeFromLeft(140));
        layoutLabelled(filterModeLabel, filterModeBox, top.removeFromLeft(150));
        layoutLabelled(cleanSilenceLabel, cleanSilenceBox, top.removeFromLeft(120));

        auto knobs = top.removeFromRight(330);
        layoutLabelled(cutoffLabel, cutoffSlider, knobs.removeFromLeft(110));
        layoutLabelled(resonanceLabel, resonanceSlider, knobs.removeFromLeft(110));
        layoutLabelled(gainLabel, gainSlider, knobs.removeFromLeft(110));

        auto modulation = area.removeFromTop(70);
        layoutLabelled(pitchBendRangeLabel, pitchBendRangeSlider, modulation.removeFromLeft(110));
        layoutLabelled(modWheelTargetLabel, modWheelTargetBox, modulation.removeFromLeft(180));
        layoutLabelled(modWheelDepthLabel, modWheelDepthSlider, modulation.removeFromLeft(110));
        layoutLabelled(codeLegatoLabel, codeLegatoButton, modulation.removeFromLeft(120));

        area.removeFromTop(10);
        auto voiceArea = area.removeFromTop(540);
        const auto panelWidth = voiceArea.getWidth() / 3;
        for (int i = 0; i < 3; ++i) {
            voices[static_cast<size_t>(i)]->setBounds(voiceArea.removeFromLeft(panelWidth).reduced(5, 0));
        }

        area.removeFromTop(10);
        codeEditor.setBounds(area);
    }

private:
    using APVTS = juce::AudioProcessorValueTreeState;

    class LookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        LookAndFeel()
        {
            setColour(juce::Slider::thumbColourId, juce::Colour(colourAccent));
            setColour(juce::Slider::trackColourId, juce::Colour(0xff315a55));
            setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(colourAccent));
            setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(colourLine).withAlpha(0.35f));
            setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff181b1f));
            setColour(juce::ComboBox::textColourId, juce::Colour(colourText));
            setColour(juce::ComboBox::outlineColourId, juce::Colour(colourLine));
            setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff181b1f));
            setColour(juce::PopupMenu::textColourId, juce::Colour(colourText));
        }

        void drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height, float sliderPos,
                              float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override
        {
            auto area = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));
            const auto side = juce::jmin(area.getWidth(), juce::jmax(20.0f, area.getHeight() - 8.0f));
            auto bounds = juce::Rectangle<float>(side, side)
                              .withCentre({ area.getCentreX(), area.getY() + side * 0.5f + 2.0f })
                              .reduced(2.0f);
            const auto centre = bounds.getCentre();
            const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
            const auto radius = bounds.getWidth() * 0.5f;

            graphics.setColour(juce::Colour(0xff111417));
            graphics.fillEllipse(bounds);
            graphics.setColour(juce::Colour(colourPanel2));
            graphics.fillEllipse(bounds.reduced(3.0f));
            graphics.setColour(juce::Colour(colourBgDeep));
            graphics.fillEllipse(bounds.reduced(radius * 0.28f));
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.55f));
            graphics.drawEllipse(bounds.reduced(1.0f), 1.4f);
            graphics.setColour(juce::Colour(colourAccent).withAlpha(0.55f));
            juce::Path arc;
            const auto arcBounds = bounds.expanded(2.0f);
            arc.addCentredArc(arcBounds.getCentreX(), arcBounds.getCentreY(),
                              arcBounds.getWidth() * 0.5f, arcBounds.getHeight() * 0.5f,
                              0.0f, rotaryStartAngle, angle, true);
            graphics.strokePath(arc, juce::PathStrokeType(2.0f));

            juce::Path pointer;
            pointer.addRoundedRectangle(-1.8f, -radius + 6.0f, 3.6f, radius * 0.52f, 1.8f);
            graphics.setColour(juce::Colour(colourAccent));
            graphics.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
        }
    };

    class ScopeComponent final : public juce::Component {
    public:
        ScopeComponent(ReSidAudioProcessor& processor, int voiceIndex)
            : audioProcessor(processor), voice(voiceIndex)
        {
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(5.0f);
            graphics.setColour(juce::Colour(colourBgDeep));
            graphics.fillRect(bounds);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.45f));
            graphics.drawRect(bounds, 1.0f);

            ReSidAudioProcessor::ScopeSnapshot samples {};
            audioProcessor.copyVoiceScope(voice, samples);

            auto peak = 0.0f;
            for (const auto sample : samples) {
                peak = std::max(peak, std::abs(sample));
            }
            constexpr auto maximumDisplayGain = 2.5f;
            constexpr auto displayHeadroom = 0.88f;
            const auto displayGain = peak > 0.0001f
                ? std::min(maximumDisplayGain, displayHeadroom / peak)
                : maximumDisplayGain;

            juce::Path path;
            const auto midY = bounds.getCentreY();
            const auto scaleY = bounds.getHeight() * 0.46f;
            for (int i = 0; i < ReSidAudioProcessor::scopeSize; ++i) {
                const auto x = juce::jmap(static_cast<float>(i), 0.0f, static_cast<float>(ReSidAudioProcessor::scopeSize - 1), bounds.getX() + 4.0f, bounds.getRight() - 4.0f);
                const auto y = midY - samples[static_cast<size_t>(i)] * displayGain * scaleY;
                if (i == 0) {
                    path.startNewSubPath(x, y);
                } else {
                    path.lineTo(x, y);
                }
            }

            graphics.setColour(juce::Colour(colourPanel2));
            graphics.drawHorizontalLine(static_cast<int>(midY), bounds.getX() + 4.0f, bounds.getRight() - 4.0f);
            graphics.setColour(juce::Colour(colourAccent));
            graphics.strokePath(path, juce::PathStrokeType(1.6f));
        }

    private:
        ReSidAudioProcessor& audioProcessor;
        int voice = 0;
    };

    class AdsrComponent final : public juce::Component, private juce::Timer {
    public:
        AdsrComponent(APVTS& parameters, const juce::String& prefix)
            : attack(parameter(parameters, prefix + "attack")),
              decay(parameter(parameters, prefix + "decay")),
              sustain(parameter(parameters, prefix + "sustain")),
              release(parameter(parameters, prefix + "release"))
        {
            configureHexEditor(adHex, [this] { applyHexByte(adHex.getText(), attack, decay); });
            configureHexEditor(srHex, [this] { applyHexByte(srHex.getText(), sustain, release); });
            addAndMakeVisible(adHex);
            addAndMakeVisible(srHex);
            startTimerHz(20);
        }

        ~AdsrComponent() override
        {
            endGesture();
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(5.0f);
            graphics.setColour(juce::Colour(colourBgDeep));
            graphics.fillRect(bounds);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.45f));
            graphics.drawRect(bounds, 1.0f);

            auto graphBounds = bounds.reduced(11.0f, 10.0f);
            graphBounds.removeFromBottom(24.0f);
            const auto points = envelopePoints(graphBounds);
            juce::Path fill;
            fill.startNewSubPath(points[0]);
            for (size_t i = 1; i < points.size(); ++i) {
                fill.lineTo(points[i]);
            }
            fill.lineTo(points.back().x, bounds.getBottom() - 10.0f);
            fill.lineTo(points[0].x, bounds.getBottom() - 10.0f);
            fill.closeSubPath();

            graphics.setColour(juce::Colour(colourAccent).withAlpha(0.14f));
            graphics.fillPath(fill);

            juce::Path line;
            line.startNewSubPath(points[0]);
            for (size_t i = 1; i < points.size(); ++i) {
                line.lineTo(points[i]);
            }
            graphics.setColour(juce::Colour(colourAccent));
            graphics.strokePath(line, juce::PathStrokeType(2.2f));

            drawNode(graphics, points[1], "A", activeNode == Node::attack ? colourWarn : colourAccent);
            drawNode(graphics, points[2], "D", activeNode == Node::decay ? colourWarn : colourAccent);
            drawNode(graphics, points[3], "S", activeNode == Node::sustain ? colourWarn : colourAccent);
            drawNode(graphics, points[4], "R", activeNode == Node::release ? colourWarn : colourAccent);

            graphics.setColour(juce::Colour(colourMuted));
            graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            auto titleArea = bounds.removeFromTop(16.0f);
            graphics.drawText("ADSR", titleArea, juce::Justification::centredLeft);

            auto hexArea = getLocalBounds().toFloat().reduced(14.0f, 8.0f).removeFromBottom(24.0f);
            drawHexBlock(graphics, hexArea.removeFromLeft(82.0f), "AD");
            hexArea.removeFromLeft(8.0f);
            drawHexBlock(graphics, hexArea.removeFromLeft(82.0f), "SR");
        }

        void resized() override
        {
            auto row = getLocalBounds().reduced(14, 8).removeFromBottom(24);
            auto ad = row.removeFromLeft(82);
            ad.removeFromLeft(28);
            adHex.setBounds(ad.reduced(2, 2));
            row.removeFromLeft(8);
            auto sr = row.removeFromLeft(82);
            sr.removeFromLeft(28);
            srHex.setBounds(sr.reduced(2, 2));
        }

        void mouseDown(const juce::MouseEvent& event) override
        {
            adHex.giveAwayKeyboardFocus();
            srHex.giveAwayKeyboardFocus();
            if (!graphBounds().contains(event.position)) {
                activeNode = Node::none;
                return;
            }

            activeNode = nearestNode(event.position);
            beginGesture(activeNode);
            mouseDrag(event);
        }

        void mouseDrag(const juce::MouseEvent& event) override
        {
            if (activeNode == Node::none) {
                return;
            }

            auto bounds = graphBounds();
            const auto xNorm = std::clamp((event.position.x - bounds.getX()) / bounds.getWidth(), 0.0f, 1.0f);
            const auto yNorm = std::clamp((event.position.y - bounds.getY()) / bounds.getHeight(), 0.0f, 1.0f);

            switch (activeNode) {
            case Node::attack:
                setNibble(attack, juce::jmap(xNorm, 0.0f, 0.30f, 0.0f, 15.0f));
                break;
            case Node::decay:
                setNibble(decay, juce::jmap(xNorm, 0.30f, 0.62f, 0.0f, 15.0f));
                break;
            case Node::sustain:
                setNibble(sustain, (1.0f - yNorm) * 15.0f);
                break;
            case Node::release:
                setNibble(release, juce::jmap(xNorm, 0.70f, 1.0f, 0.0f, 15.0f));
                break;
            case Node::none:
                break;
            }

            repaint();
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            endGesture();
            adHex.giveAwayKeyboardFocus();
            srHex.giveAwayKeyboardFocus();
            activeNode = Node::none;
            repaint();
        }

    private:
        enum class Node { none, attack, decay, sustain, release };

        static juce::RangedAudioParameter* parameter(APVTS& parameters, const juce::String& id)
        {
            return parameters.getParameter(id);
        }

        static float nibble(const juce::RangedAudioParameter* parameter)
        {
            return parameter == nullptr ? 0.0f : std::clamp(parameter->getValue() * 15.0f, 0.0f, 15.0f);
        }

        static void setNibble(juce::RangedAudioParameter* parameter, float value)
        {
            if (parameter == nullptr) {
                return;
            }

            const auto snapped = static_cast<float>(std::clamp(std::lround(value), 0l, 15l));
            parameter->setValueNotifyingHost(parameter->convertTo0to1(snapped));
        }

        std::array<juce::Point<float>, 6> envelopePoints(juce::Rectangle<float> bounds) const
        {
            const auto bottom = bounds.getBottom();
            const auto top = bounds.getY();
            const auto width = bounds.getWidth();
            const auto sustainY = juce::jmap(nibble(sustain), 0.0f, 15.0f, bottom, top);
            const auto attackX = bounds.getX() + width * juce::jmap(nibble(attack), 0.0f, 15.0f, 0.0f, 0.30f);
            const auto decayX = attackX + width * juce::jmap(nibble(decay), 0.0f, 15.0f, 0.04f, 0.32f);
            const auto sustainEndX = std::max(decayX + 10.0f, bounds.getX() + width * 0.68f);
            const auto releaseX = sustainEndX + width * juce::jmap(nibble(release), 0.0f, 15.0f, 0.02f, 0.30f);

            return {
                juce::Point<float> { bounds.getX(), bottom },
                juce::Point<float> { attackX, top },
                juce::Point<float> { std::min(decayX, bounds.getRight() - 20.0f), sustainY },
                juce::Point<float> { std::min(sustainEndX, bounds.getRight() - 10.0f), sustainY },
                juce::Point<float> { std::min(releaseX, bounds.getRight()), bottom },
                juce::Point<float> { bounds.getRight(), bottom }
            };
        }

        Node nearestNode(juce::Point<float> position) const
        {
            const auto points = envelopePoints(graphBounds());
            std::array<Node, 4> nodes { Node::attack, Node::decay, Node::sustain, Node::release };
            std::array<juce::Point<float>, 4> handles { points[1], points[2], points[3], points[4] };
            auto best = Node::attack;
            auto bestDistance = std::numeric_limits<float>::max();
            for (size_t i = 0; i < handles.size(); ++i) {
                const auto distance = handles[i].getDistanceFrom(position);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = nodes[i];
                }
            }
            return best;
        }

        void drawNode(juce::Graphics& graphics, juce::Point<float> point, const juce::String& label, uint32_t colour) const
        {
            auto node = juce::Rectangle<float>(point.x - 5.0f, point.y - 5.0f, 10.0f, 10.0f);
            graphics.setColour(juce::Colour(colour));
            graphics.fillRect(node);
            graphics.setColour(juce::Colour(colourBgDeep));
            graphics.drawRect(node, 1.0f);
            graphics.setColour(juce::Colour(colourMuted));
            graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            graphics.drawText(label, node.translated(8.0f, -8.0f), juce::Justification::centredLeft);
        }

        void drawHexBlock(juce::Graphics& graphics, juce::Rectangle<float> area, const juce::String& label) const
        {
            graphics.setColour(juce::Colour(0xff15191d));
            graphics.fillRect(area);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.45f));
            graphics.drawRect(area, 1.0f);
            auto labelArea = area.removeFromLeft(28.0f);
            graphics.setColour(juce::Colour(colourAccent).withAlpha(0.16f));
            graphics.fillRect(labelArea.reduced(2.0f));
            graphics.setColour(juce::Colour(colourAccent));
            graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            graphics.drawText(label, labelArea, juce::Justification::centred);
        }

        void beginGesture(Node node)
        {
            endGesture();
            activeParameter = parameterForNode(node);
            if (activeParameter != nullptr) {
                activeParameter->beginChangeGesture();
            }
        }

        void endGesture()
        {
            if (activeParameter != nullptr) {
                activeParameter->endChangeGesture();
                activeParameter = nullptr;
            }
        }

        juce::RangedAudioParameter* parameterForNode(Node node) const
        {
            switch (node) {
            case Node::attack:
                return attack;
            case Node::decay:
                return decay;
            case Node::sustain:
                return sustain;
            case Node::release:
                return release;
            case Node::none:
                return nullptr;
            }
            return nullptr;
        }

        void timerCallback() override
        {
            syncHexEditors();
            repaint();
        }

        juce::Rectangle<float> graphBounds() const
        {
            auto bounds = getLocalBounds().toFloat().reduced(16.0f, 15.0f);
            bounds.removeFromBottom(24.0f);
            return bounds;
        }

        void configureHexEditor(juce::TextEditor& editor, std::function<void()> commit)
        {
            editor.setInputRestrictions(2, "0123456789abcdefABCDEF");
            editor.setJustification(juce::Justification::centred);
            editor.setSelectAllWhenFocused(true);
            editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff101316));
            editor.setColour(juce::TextEditor::textColourId, juce::Colour(colourText));
            editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0x00000000));
            editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(colourAccent));
            editor.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            editor.onReturnKey = commit;
            editor.onFocusLost = commit;
        }

        void syncHexEditors()
        {
            if (!adHex.hasKeyboardFocus(true)) {
                adHex.setText(byteText(attack, decay), juce::dontSendNotification);
            }
            if (!srHex.hasKeyboardFocus(true)) {
                srHex.setText(byteText(sustain, release), juce::dontSendNotification);
            }
        }

        juce::String byteText(const juce::RangedAudioParameter* high, const juce::RangedAudioParameter* low) const
        {
            const auto byte = (static_cast<int>(std::lround(nibble(high))) << 4)
                | static_cast<int>(std::lround(nibble(low)));
            return juce::String::toHexString(byte).paddedLeft('0', 2).toUpperCase();
        }

        void applyHexByte(const juce::String& text, juce::RangedAudioParameter* high, juce::RangedAudioParameter* low)
        {
            const auto value = text.getHexValue32() & 0xff;
            if (high != nullptr) {
                high->beginChangeGesture();
                setNibble(high, static_cast<float>((value >> 4) & 0x0f));
                high->endChangeGesture();
            }
            if (low != nullptr) {
                low->beginChangeGesture();
                setNibble(low, static_cast<float>(value & 0x0f));
                low->endChangeGesture();
            }
            syncHexEditors();
        }

        juce::RangedAudioParameter* attack = nullptr;
        juce::RangedAudioParameter* decay = nullptr;
        juce::RangedAudioParameter* sustain = nullptr;
        juce::RangedAudioParameter* release = nullptr;
        juce::RangedAudioParameter* activeParameter = nullptr;
        Node activeNode = Node::none;
        juce::TextEditor adHex;
        juce::TextEditor srHex;
    };

    class WaveformSelector final : public juce::Component, private juce::Timer {
    public:
        WaveformSelector(APVTS& parameters, const juce::String& parameterId)
            : parameter(parameters.getParameter(parameterId))
        {
            startTimerHz(15);
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat();
            graphics.setColour(juce::Colour(colourBgDeep));
            graphics.fillRect(bounds);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.4f));
            graphics.drawRect(bounds, 1.0f);

            const auto selected = selectedIndex();
            const auto cellWidth = bounds.getWidth() / 5.0f;
            for (int i = 0; i < 5; ++i) {
                auto cell = juce::Rectangle<float>(bounds.getX() + cellWidth * static_cast<float>(i), bounds.getY(), cellWidth, bounds.getHeight()).reduced(3.0f);
                const auto isSelected = i == selected;
                graphics.setColour(isSelected ? juce::Colour(colourAccent).withAlpha(0.20f) : juce::Colour(colourPanel2).withAlpha(0.55f));
                graphics.fillRect(cell);
                graphics.setColour(isSelected ? juce::Colour(colourAccent) : juce::Colour(colourLine).withAlpha(0.5f));
                graphics.drawRect(cell, 1.0f);
                drawGlyph(graphics, cell.reduced(5.0f, 6.0f), i, isSelected);
            }
        }

        void mouseDown(const juce::MouseEvent& event) override
        {
            if (getWidth() <= 0) {
                return;
            }
            const auto index = std::clamp(static_cast<int>((event.position.x / static_cast<float>(getWidth())) * 5.0f), 0, 4);
            setWaveform(index);
        }

    private:
        void setWaveform(int index)
        {
            if (parameter == nullptr) {
                return;
            }

            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
            parameter->endChangeGesture();
            repaint();
        }

        int selectedIndex() const
        {
            return parameter == nullptr
                ? 0
                : static_cast<int>(std::clamp(std::lround(parameter->convertFrom0to1(parameter->getValue())), 0l, 4l));
        }

        static void drawGlyph(juce::Graphics& graphics, juce::Rectangle<float> area, int waveform, bool selected)
        {
            juce::Path path;
            const auto left = area.getX();
            const auto right = area.getRight();
            const auto top = area.getY();
            const auto mid = area.getCentreY();
            const auto bottom = area.getBottom();

            switch (waveform) {
            case 0:
                path.startNewSubPath(left, bottom);
                path.lineTo(left + area.getWidth() * 0.25f, top);
                path.lineTo(left + area.getWidth() * 0.50f, bottom);
                path.lineTo(left + area.getWidth() * 0.75f, top);
                path.lineTo(right, bottom);
                break;
            case 1:
                path.startNewSubPath(left, bottom);
                path.lineTo(left + area.getWidth() * 0.45f, top);
                path.lineTo(left + area.getWidth() * 0.45f, bottom);
                path.lineTo(right, top);
                break;
            case 2:
                path.startNewSubPath(left, bottom);
                path.lineTo(left, top);
                path.lineTo(left + area.getWidth() * 0.55f, top);
                path.lineTo(left + area.getWidth() * 0.55f, bottom);
                path.lineTo(right, bottom);
                break;
            case 3: {
                static constexpr std::array<float, 12> noise { 0.62f, 0.18f, 0.82f, 0.35f, 0.70f, 0.10f, 0.52f, 0.28f, 0.90f, 0.45f, 0.74f, 0.22f };
                path.startNewSubPath(left, juce::jmap(noise[0], 0.0f, 1.0f, bottom, top));
                for (size_t i = 1; i < noise.size(); ++i) {
                    path.lineTo(juce::jmap(static_cast<float>(i), 0.0f, static_cast<float>(noise.size() - 1), left, right),
                                juce::jmap(noise[i], 0.0f, 1.0f, bottom, top));
                }
                break;
            }
            case 4:
            default:
                path.startNewSubPath(left, mid);
                path.lineTo(right, mid);
                graphics.setColour(selected ? juce::Colour(colourAccent) : juce::Colour(colourMuted));
                graphics.drawLine(left + 2.0f, bottom, right - 2.0f, top, 1.5f);
                break;
            }

            graphics.setColour(selected ? juce::Colour(colourAccent) : juce::Colour(colourText).withAlpha(0.78f));
            graphics.strokePath(path, juce::PathStrokeType(2.0f));
        }

        void timerCallback() override
        {
            repaint();
        }

        juce::RangedAudioParameter* parameter = nullptr;
    };

    class CodeEditorPanel final : public juce::Component {
    public:
        explicit CodeEditorPanel(APVTS& parameters)
            : state(parameters.state.getOrCreateChildWithName("CodeInstrument", nullptr))
        {
            title.setText("CODE INSTRUMENT", juce::dontSendNotification);
            title.setJustificationType(juce::Justification::centredLeft);
            title.setColour(juce::Label::textColourId, juce::Colour(colourText));
            title.setFont(juce::FontOptions(17.0f, juce::Font::bold));
            addAndMakeVisible(title);

            presetLabel.setText("LOAD", juce::dontSendNotification);
            presetLabel.setJustificationType(juce::Justification::centred);
            presetLabel.setColour(juce::Label::textColourId, juce::Colour(colourMuted));
            presetLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            addAndMakeVisible(presetLabel);

            presetBox.setJustificationType(juce::Justification::centred);
            presetBox.setComponentID("code_preset_selector");
            for (int i = 0; i < static_cast<int>(presets().size()); ++i) {
                presetBox.addItem(presets()[static_cast<size_t>(i)].name, i + 1);
            }
            presetBox.setSelectedId(1, juce::dontSendNotification);
            presetBox.onChange = [this] {
                const auto index = presetBox.getSelectedId() - 1;
                if (index >= 0 && index < static_cast<int>(presets().size())) {
                    applyPreset(presets()[static_cast<size_t>(index)]);
                }
            };
            addAndMakeVisible(presetBox);

            configureNameEditor();
            addAndMakeVisible(nameEditor);

            addMetaCell("ADSR", "adsr", 4, 0x0800);
            addMetaCell("SPD", "speed", 2, 0x00);
            addMetaCell("VIBR", "vibrato", 4, 0x1000);

            for (int table = 0; table < tableCount; ++table) {
                lengthCells[static_cast<size_t>(table)] = addCell(lengthId(table), 2, 0, rowCount, defaultLengthForTable(table));
                for (int row = 0; row < rowCount; ++row) {
                    for (int column = 0; column < maxColumns; ++column) {
                        tableCells[static_cast<size_t>(table)][static_cast<size_t>(row)][static_cast<size_t>(column)] =
                            addCell(cellId(table, row, column), 2, 0, 0xff, defaultTableValue(table, row, column));
                        tableCells[static_cast<size_t>(table)][static_cast<size_t>(row)][static_cast<size_t>(column)]->onArrow =
                            [this, table](int delta) { scrollTable(table, delta); };
                    }
                }
            }

            refreshActivity();
            syncPresetSelection();
        }

        void syncPresetSelection()
        {
            const auto currentName = state.getProperty("name", "").toString();
            auto selectedId = 0;
            for (int i = 0; i < static_cast<int>(presets().size()); ++i) {
                if (currentName == presets()[static_cast<size_t>(i)].name) {
                    selectedId = i + 1;
                    break;
                }
            }
            if (presetBox.getSelectedId() != selectedId) {
                presetBox.setSelectedId(selectedId, juce::dontSendNotification);
            }
            if (!nameEditor.hasKeyboardFocus(true) && nameEditor.getText() != currentName) {
                nameEditor.setText(currentName, juce::dontSendNotification);
            }
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat();
            graphics.setColour(juce::Colour(colourPanel));
            graphics.fillRect(bounds);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.55f));
            graphics.drawRect(bounds.reduced(1.0f), 1.0f);

            auto help = getLocalBounds().toFloat().reduced(14.0f).removeFromBottom(61.0f);
            graphics.setColour(juce::Colour(colourMuted));
            graphics.setFont(juce::FontOptions(11.0f));
            graphics.drawText("WF 10..FD=control FE=jump | ARP 01..5F=up 80=hold E0..FF=down | PULSE/FILT 8x..Fx=absolute 00..7F=add",
                              help.removeFromTop(15.0f), juce::Justification::centredLeft);
            graphics.drawText("VIBR: first hex digit=depth, second=speed, last two=delay before vibrato starts",
                              help.removeFromTop(15.0f), juce::Justification::centredLeft);
            graphics.drawText("SPD: bits 0..5=WF/ARP delay, bit 6=multispeed pulse, bit 7=multispeed filter (80 = delay 0 + filter multispeed)",
                              help.removeFromTop(15.0f), juce::Justification::centredLeft);
            graphics.drawText("DT: 00..FE=detune up, FF=hold | KT: signed keyboard tracking from MIDI note 60 for pulse/filter",
                              help, juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            auto header = area.removeFromTop(32);
            title.setBounds(header.removeFromLeft(185));
            auto presetBlock = header.removeFromLeft(220);
            presetLabel.setBounds(presetBlock.removeFromLeft(38).reduced(0, 4));
            presetBox.setBounds(presetBlock.reduced(0, 4));
            header.removeFromLeft(10);
            auto nameBlock = header.removeFromLeft(250);
            nameEditor.setBounds(nameBlock.reduced(0, 4));

            auto meta = header;
            for (size_t i = 0; i < metaCells.size(); ++i) {
                auto& item = metaCells[i];
                const auto blockWidth = i == 1 ? 94 : 118;
                auto block = meta.removeFromLeft(blockWidth).reduced(4, 2);
                item.label.setBounds(block.removeFromLeft(34));
                item.cell->setBounds(block);
            }

            area.removeFromTop(4);
            area.removeFromBottom(65);
            const auto tableWidth = area.getWidth() / tableCount;
            for (int table = 0; table < tableCount; ++table) {
                layoutTable(table, area.removeFromLeft(tableWidth).reduced(4, 0));
            }
        }

    private:
        class HexCell final : public juce::Label, private juce::KeyListener {
        public:
            HexCell(juce::ValueTree stateToUse, juce::Identifier idToUse, int digitsToUse, int minToUse, int maxToUse, int defaultValue)
                : state(stateToUse), id(idToUse), digits(digitsToUse), minValue(minToUse), maxValue(maxToUse)
            {
                setJustificationType(juce::Justification::centred);
                setEditable(true, true, false);
                setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
                setColour(juce::Label::textColourId, juce::Colour(colourText));
                setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
                setColour(juce::Label::textWhenEditingColourId, juce::Colour(colourText));
                setColour(juce::Label::backgroundWhenEditingColourId, juce::Colour(0xff090b0d));
                setColour(juce::TextEditor::highlightColourId, juce::Colour(colourAccent).withAlpha(0.35f));
                setFont(juce::FontOptions(12.0f, juce::Font::bold));

                if (!state.hasProperty(id)) {
                    state.setProperty(id, juce::jlimit(minValue, maxValue, defaultValue), nullptr);
                }
                setValue(static_cast<int>(state[id]), juce::dontSendNotification);
                onTextChange = [this] { commitText(); };
            }

            int value() const
            {
                return juce::jlimit(minValue, maxValue, static_cast<int>(state[id]));
            }

            void refreshFromState()
            {
                setValue(static_cast<int>(state.getProperty(id, minValue)), juce::dontSendNotification);
            }

            void setInactive(bool shouldBeInactive)
            {
                inactive = shouldBeInactive;
                setAlpha(inactive ? 0.38f : 1.0f);
            }

            std::function<void()> onCommit;
            std::function<void(int)> onArrow;

        private:
            bool handleArrowKey(const juce::KeyPress& key)
            {
                if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
                    if (onArrow) {
                        onArrow(key == juce::KeyPress::upKey ? -1 : 1);
                    }
                    return true;
                }
                return false;
            }

            bool keyPressed(const juce::KeyPress& key) override
            {
                if (handleArrowKey(key)) {
                    return true;
                }
                return juce::Label::keyPressed(key);
            }

            bool keyPressed(const juce::KeyPress& key, juce::Component*) override
            {
                return handleArrowKey(key);
            }

            void editorShown(juce::TextEditor* editor) override
            {
                editor->addKeyListener(this);
            }

            void editorAboutToBeHidden(juce::TextEditor* editor) override
            {
                editor->removeKeyListener(this);
            }

            void commitText()
            {
                if (suppress) {
                    return;
                }

                auto text = getText().trim().removeCharacters("$# ");
                if (text.startsWithIgnoreCase("0x")) {
                    text = text.substring(2);
                }
                const auto parsed = text.isEmpty() ? 0 : text.getHexValue32();
                setValue(parsed, juce::dontSendNotification);
                if (onCommit) {
                    onCommit();
                }
            }

            void setValue(int newValue, juce::NotificationType notification)
            {
                const auto clamped = juce::jlimit(minValue, maxValue, newValue);
                state.setProperty(id, clamped, nullptr);
                const juce::ScopedValueSetter<bool> setter(suppress, true);
                setText(juce::String::toHexString(clamped).paddedLeft('0', digits).toUpperCase(), notification);
            }

            juce::ValueTree state;
            juce::Identifier id;
            int digits = 2;
            int minValue = 0;
            int maxValue = 0xff;
            bool inactive = false;
            bool suppress = false;
        };

        static constexpr int rowCount = resid_vst::SidCodeInstrument::rowCount;
        static constexpr int tableCount = 4;
        static constexpr int maxColumns = 4;

        struct TableSpec {
            const char* key;
            const char* title;
            std::array<const char*, maxColumns> labels;
            std::array<int, maxColumns> defaults;
            int columns;
        };

        struct MetaCell {
            juce::Label label;
            HexCell* cell = nullptr;
        };

        struct Preset {
            const char* name = "";
            uint16_t adsr = 0x00f0;
            uint8_t speed = 0x00;
            uint16_t vibrato = 0x0000;
            uint8_t vibratoType = 0x10;
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
            std::array<uint8_t, tableCount> lengths {};
            std::array<std::array<std::array<uint8_t, maxColumns>, rowCount>, tableCount> cells {};
        };

        static const TableSpec& specFor(int index)
        {
            static const std::array<TableSpec, tableCount> tableSpecs {
                TableSpec { "wf", "WFARP", { "WF", "ARP", "DT", "" }, { 0x41, 0x00, 0xff, 0x00 }, 3 },
                TableSpec { "pulse", "PULSE", { "CMD", "VAL", "KT", "" }, { 0x00, 0x00, 0x00, 0x00 }, 3 },
                TableSpec { "filter", "FILT", { "CMD", "VAL", "KT", "" }, { 0x00, 0x00, 0x00, 0x00 }, 3 },
                TableSpec { "chord", "CHORD", { "O1", "O2", "O3", "NX" }, { 0x00, 0x00, 0x00, 0xff }, 4 }
            };
            return tableSpecs[static_cast<size_t>(juce::jlimit(0, tableCount - 1, index))];
        }

        static Preset makePreset(const char* name, uint16_t adsr, uint8_t speed, uint16_t vibrato)
        {
            Preset preset;
            preset.name = name;
            preset.adsr = adsr;
            preset.speed = speed;
            preset.vibrato = vibrato;
            for (int table = 0; table < tableCount; ++table) {
                preset.lengths[static_cast<size_t>(table)] = static_cast<uint8_t>(defaultLengthForTable(table));
                for (int row = 0; row < rowCount; ++row) {
                    for (int column = 0; column < maxColumns; ++column) {
                        preset.cells[static_cast<size_t>(table)][static_cast<size_t>(row)][static_cast<size_t>(column)] =
                            static_cast<uint8_t>(defaultTableValue(table, row, column));
                    }
                }
            }
            return preset;
        }

        static void copyImportedCommandRows(Preset& preset,
                                            int table,
                                            const sidwizard_import::InstrumentCommandProgram& program,
                                            int tableAddress)
        {
            const auto length = std::min<int>(program.length, rowCount);
            preset.lengths[static_cast<size_t>(table)] = static_cast<uint8_t>(length);

            for (int row = 0; row < length; ++row) {
                const auto& importedRow = program.rows[static_cast<size_t>(row)];
                preset.cells[static_cast<size_t>(table)][static_cast<size_t>(row)] = {
                    importedRow.value,
                    importedRow.value == 0xfe
                        ? rebaseImportedJump(importedRow.arg, row, length, tableAddress)
                        : importedRow.arg,
                    importedRow.aux,
                    0x00
                };
            }
        }

        static void copyImportedChordRows(Preset& preset,
                                          const sidwizard_import::InstrumentChordProgram& program)
        {
            const auto length = std::min<int>(program.length, rowCount);
            preset.lengths[3] = static_cast<uint8_t>(length);

            for (int row = 0; row < length; ++row) {
                const auto& importedRow = program.rows[static_cast<size_t>(row)];
                preset.cells[3][static_cast<size_t>(row)] = {
                    static_cast<uint8_t>(importedRow.offsets[0]),
                    static_cast<uint8_t>(importedRow.offsets[1]),
                    static_cast<uint8_t>(importedRow.offsets[2]),
                    importedRow.next
                };
            }
        }

        static Preset makeImportedPreset(const sidwizard_import::InstrumentPreset& imported)
        {
            auto preset = makePreset(imported.name,
                                     static_cast<uint16_t>((imported.defaultAttack << 12)
                                                           | (imported.defaultDecay << 8)
                                                           | ((imported.defaultSustain >> 2) << 4)
                                                           | imported.defaultRelease),
                                     imported.speed,
                                     imported.vibrato);
            preset.vibratoType = imported.vibratoType;
            preset.defaultPulseWidth = std::min<uint16_t>(imported.defaultPulseWidth, 4095);
            preset.defaultFilterCutoff = std::min<uint16_t>(imported.defaultFilterCutoff, 2047);
            preset.defaultFilterResonance = imported.defaultFilterResonance & 0x0f;
            preset.defaultFilterMode = static_cast<uint8_t>((imported.defaultFilterMode & 0x07) << 4);
            preset.absolutePitch = imported.absolutePitch;
            preset.fixedMidiNote = imported.fixedMidiNote;
            preset.velocityRelease = imported.velocityRelease;
            if (juce::String(imported.name).containsIgnoreCase("4x")) {
                preset.playbackRate = 4;
            }

            struct GateOffRows { const char* name; uint8_t wf; uint8_t pulse; uint8_t filter; };
            static constexpr std::array<GateOffRows, 18> gateOffRows {{
                { "bass-popfilt", 4, 0xff, 0xff }, { "bass-slapfil", 6, 0xff, 0xff },
                { "bell-glassy", 3, 0xff, 0xff }, { "brassglissdn", 4, 0xff, 0xff },
                { "chord-autofi", 0xff, 3, 3 }, { "dbass-bowedf", 4, 0xff, 0xff },
                { "flute-ocarin", 6, 0xff, 0xff }, { "glockenspiel", 2, 0xff, 0xff },
                { "hihat-pedaly", 1, 0xff, 0xff }, { "organ-rockup", 2, 0xff, 0xff },
                { "saw-triverb", 2, 0xff, 0xff }, { "sfx-cat-myau", 2, 4, 3 },
                { "sfx-windfilt", 0xff, 0xff, 3 }, { "solo-41-echo", 3, 0xff, 0xff },
                { "solo-octecho", 3, 0xff, 0xff }, { "solo-openfil", 4, 0xff, 0xff },
                { "solo-thin", 0xff, 3, 2 }, { "solo41thecho", 0xff, 4, 0xff }
            }};
            for (const auto& rows : gateOffRows) {
                if (std::strcmp(imported.name, rows.name) == 0) {
                    preset.gateOffWfRow = rows.wf;
                    preset.gateOffPulseRow = rows.pulse;
                    preset.gateOffFilterRow = rows.filter;
                    break;
                }
            }

            const auto waveIndex = std::min<size_t>(imported.waveProgram, sidwizard_import::kImportedInstrumentProgramCount - 1);
            const auto pulseIndex = std::min<size_t>(imported.pulseWidthProgram, sidwizard_import::kImportedInstrumentProgramCount - 1);
            const auto& waveProgram = sidwizard_import::gImportedInstrumentWavePrograms[waveIndex];
            const auto& pulseProgram = sidwizard_import::gImportedInstrumentPulseWidthPrograms[pulseIndex];
            // Each table is terminated by one $FF byte in the SWI data. The
            // stored jump operands are byte addresses relative to the full
            // instrument, so those terminators must be part of the bases.
            const auto pulseAddress = 0x11 + 3 * waveProgram.length;
            const auto filterAddress = 0x12 + 3 * (waveProgram.length + pulseProgram.length);

            if (imported.waveProgram < sidwizard_import::kImportedInstrumentProgramCount) {
                copyImportedCommandRows(preset, 0, waveProgram, 0x10);
            }
            if (imported.pulseWidthProgram < sidwizard_import::kImportedInstrumentProgramCount) {
                copyImportedCommandRows(preset, 1, pulseProgram, pulseAddress);
            }
            if (imported.filterProgram < sidwizard_import::kImportedInstrumentProgramCount) {
                copyImportedCommandRows(preset, 2, sidwizard_import::gImportedInstrumentFilterPrograms[imported.filterProgram], filterAddress);
            }
            if (imported.chordProgram < sidwizard_import::kImportedInstrumentChordProgramCount) {
                copyImportedChordRows(preset, sidwizard_import::gImportedInstrumentChordPrograms[imported.chordProgram]);
            }

            // SWI files store only a reference to the song-global chord
            // table, not the chord notes themselves. Give standalone imports
            // that invoke ARP $7F a useful, visible major-chord fallback.
            const auto invokesExternalChord = std::any_of(
                waveProgram.rows,
                waveProgram.rows + waveProgram.length,
                [](const auto& row) { return row.arg == 0x7f; });
            if (invokesExternalChord && preset.lengths[3] == 0) {
                preset.lengths[3] = 1;
                preset.cells[3][0] = { 0x00, 0x04, 0x07, 0xfe };
            }

            return preset;
        }

        static const std::vector<Preset>& presets()
        {
            static const std::vector<Preset> list = [] {
                std::vector<Preset> result;
                result.reserve(6 + sidwizard_import::kImportedInstrumentPresetCount);

                auto noise = makePreset("Noise Drum", 0x0800, 0x00, 0x1000);
                noise.lengths[0] = 4;
                noise.cells[0][0] = { 0x81, 0xc8, 0xff, 0x00 };
                noise.cells[0][1] = { 0x81, 0xa4, 0xff, 0x00 };
                noise.cells[0][2] = { 0x81, 0xa4, 0xff, 0x00 };
                noise.cells[0][3] = { 0xfe, 0x02, 0xff, 0x00 };
                result.push_back(noise);

                auto bass = makePreset("Pulse Bass", 0x0250, 0x01, 0x0000);
                bass.lengths[0] = 2;
                bass.cells[0][0] = { 0x41, 0x00, 0xff, 0x00 };
                bass.cells[0][1] = { 0xfe, 0x00, 0xff, 0x00 };
                bass.lengths[1] = 4;
                bass.cells[1][0] = { 0x84, 0x80, 0x00, 0x00 };
                bass.cells[1][1] = { 0x00, 0x18, 0x00, 0x00 };
                bass.cells[1][2] = { 0x00, 0x08, 0x00, 0x00 };
                bass.cells[1][3] = { 0xfe, 0x01, 0x00, 0x00 };
                result.push_back(bass);

                auto pluck = makePreset("Saw Pluck", 0x0440, 0x00, 0x0000);
                pluck.lengths[0] = 3;
                pluck.cells[0][0] = { 0x21, 0x0c, 0xff, 0x00 };
                pluck.cells[0][1] = { 0x21, 0x00, 0xff, 0x00 };
                pluck.cells[0][2] = { 0xfe, 0x01, 0xff, 0x00 };
                result.push_back(pluck);

                auto lead = makePreset("PWM Lead", 0x08a3, 0x00, 0x0000);
                lead.lengths[0] = 1;
                lead.cells[0][0] = { 0x41, 0x00, 0xff, 0x00 };
                lead.lengths[1] = 5;
                lead.cells[1][0] = { 0x82, 0x40, 0x00, 0x00 };
                lead.cells[1][1] = { 0x00, 0x20, 0x00, 0x00 };
                lead.cells[1][2] = { 0x00, 0x20, 0x00, 0x00 };
                lead.cells[1][3] = { 0x88, 0x00, 0x00, 0x00 };
                lead.cells[1][4] = { 0xfe, 0x01, 0x00, 0x00 };
                result.push_back(lead);

                auto chord = makePreset("Chord Stab", 0x0360, 0x01, 0x0000);
                chord.lengths[0] = 2;
                chord.cells[0][0] = { 0x41, 0x7f, 0xff, 0x00 };
                chord.cells[0][1] = { 0xfe, 0x00, 0xff, 0x00 };
                chord.lengths[3] = 1;
                chord.cells[3][0] = { 0x00, 0x04, 0x07, 0xff };
                result.push_back(chord);

                auto zap = makePreset("Filter Zap", 0x0710, 0x00, 0x0000);
                zap.lengths[0] = 2;
                zap.cells[0][0] = { 0x21, 0x18, 0xff, 0x00 };
                zap.cells[0][1] = { 0xfe, 0x00, 0xff, 0x00 };
                zap.lengths[2] = 4;
                zap.cells[2][0] = { 0x87, 0xf0, 0x00, 0x00 };
                zap.cells[2][1] = { 0x00, 0x20, 0x00, 0x00 };
                zap.cells[2][2] = { 0x00, 0x10, 0x00, 0x00 };
                zap.cells[2][3] = { 0xfe, 0x01, 0x00, 0x00 };
                result.push_back(zap);

                for (const auto& imported : sidwizard_import::gImportedInstrumentPresets) {
                    result.push_back(makeImportedPreset(imported));
                }

                return result;
            }();
            return list;
        }

        void configureNameEditor()
        {
            nameEditor.setText(state.getProperty("name", "NOISE DRUM").toString(), juce::dontSendNotification);
            nameEditor.setInputRestrictions(16);
            nameEditor.setJustification(juce::Justification::centredLeft);
            nameEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff101316));
            nameEditor.setColour(juce::TextEditor::textColourId, juce::Colour(colourText));
            nameEditor.setColour(juce::TextEditor::outlineColourId, juce::Colour(colourLine).withAlpha(0.45f));
            nameEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(colourAccent));
            nameEditor.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            nameEditor.onTextChange = [this] {
                state.setProperty("name", nameEditor.getText().substring(0, 16), nullptr);
            };
        }

        void addMetaCell(const char* labelText, const char* idText, int digits, int defaultValue)
        {
            auto& item = metaCells[metaCellCount++];
            item.label.setText(labelText, juce::dontSendNotification);
            item.label.setJustificationType(juce::Justification::centred);
            item.label.setColour(juce::Label::textColourId, juce::Colour(colourAccent));
            item.label.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            addAndMakeVisible(item.label);
            item.cell = addCell(juce::Identifier(idText), digits, 0, digits == 4 ? 0xffff : 0xff, defaultValue);
        }

        static int defaultLengthForTable(int table)
        {
            return table == 0 ? 4 : 0;
        }

        static int defaultTableValue(int table, int row, int column)
        {
            if (table == 0) {
                static constexpr std::array<std::array<int, 3>, 4> drumRows {
                    std::array<int, 3> { 0x81, 0xc8, 0xff },
                    std::array<int, 3> { 0x81, 0xa4, 0xff },
                    std::array<int, 3> { 0x81, 0xa4, 0xff },
                    std::array<int, 3> { 0xfe, 0x02, 0xff }
                };
                if (row < static_cast<int>(drumRows.size()) && column < 3) {
                    return drumRows[static_cast<size_t>(row)][static_cast<size_t>(column)];
                }
            }

            const auto& spec = specFor(table);
            return column < spec.columns ? spec.defaults[static_cast<size_t>(column)] : 0;
        }

        void applyPreset(const Preset& preset)
        {
            state.setProperty("name", juce::String(preset.name), nullptr);
            state.setProperty("adsr", static_cast<int>(preset.adsr), nullptr);
            state.setProperty("speed", static_cast<int>(preset.speed), nullptr);
            state.setProperty("vibrato", static_cast<int>(preset.vibrato), nullptr);
            state.setProperty("vibrato_type", static_cast<int>(preset.vibratoType), nullptr);
            state.setProperty("default_pulse_width", static_cast<int>(preset.defaultPulseWidth), nullptr);
            state.setProperty("default_filter_cutoff", static_cast<int>(preset.defaultFilterCutoff), nullptr);
            state.setProperty("default_filter_resonance", static_cast<int>(preset.defaultFilterResonance), nullptr);
            state.setProperty("default_filter_mode", static_cast<int>(preset.defaultFilterMode), nullptr);
            state.setProperty("absolute_pitch", preset.absolutePitch, nullptr);
            state.setProperty("fixed_midi_note", static_cast<int>(preset.fixedMidiNote), nullptr);
            state.setProperty("velocity_release", preset.velocityRelease, nullptr);
            state.setProperty("gate_off_wf_row", static_cast<int>(preset.gateOffWfRow), nullptr);
            state.setProperty("gate_off_pulse_row", static_cast<int>(preset.gateOffPulseRow), nullptr);
            state.setProperty("gate_off_filter_row", static_cast<int>(preset.gateOffFilterRow), nullptr);
            state.setProperty("playback_rate", static_cast<int>(preset.playbackRate), nullptr);

            for (int table = 0; table < tableCount; ++table) {
                state.setProperty(lengthId(table), static_cast<int>(preset.lengths[static_cast<size_t>(table)]), nullptr);
                for (int row = 0; row < rowCount; ++row) {
                    for (int column = 0; column < maxColumns; ++column) {
                        state.setProperty(cellId(table, row, column),
                                          static_cast<int>(preset.cells[static_cast<size_t>(table)][static_cast<size_t>(row)][static_cast<size_t>(column)]),
                                          nullptr);
                    }
                }
            }

            nameEditor.setText(preset.name, juce::dontSendNotification);
            for (auto& item : metaCells) {
                if (item.cell != nullptr) {
                    item.cell->refreshFromState();
                }
            }
            for (auto& cell : ownedCells) {
                cell->refreshFromState();
            }
            refreshActivity();
        }

        HexCell* addCell(juce::Identifier id, int digits, int minValue, int maxValue, int defaultValue)
        {
            auto cell = std::make_unique<HexCell>(state, id, digits, minValue, maxValue, defaultValue);
            auto* raw = cell.get();
            raw->onCommit = [this] { refreshActivity(); };
            addAndMakeVisible(*raw);
            ownedCells.push_back(std::move(cell));
            return raw;
        }

        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        {
            for (int table = 0; table < tableCount; ++table) {
                if (tableBounds[static_cast<size_t>(table)].contains(event.getPosition())) {
                    scrollTable(table, wheel.deltaY < 0.0f ? 1 : -1);
                    return;
                }
            }
        }

        void layoutTable(int tableIndex, juce::Rectangle<int> area)
        {
            const auto& spec = specFor(tableIndex);
            tableBounds[static_cast<size_t>(tableIndex)] = area;
            auto header = area.removeFromTop(22);
            drawTableHeaderBounds[static_cast<size_t>(tableIndex)] = header;

            header.removeFromLeft(72);
            lengthCells[static_cast<size_t>(tableIndex)]->setBounds(header.removeFromRight(30));

            auto columns = area.removeFromTop(16);
            columns.removeFromLeft(30);
            const auto cellWidth = columns.getWidth() / spec.columns;
            for (int column = 0; column < spec.columns; ++column) {
                columnLabelBounds[static_cast<size_t>(tableIndex)][static_cast<size_t>(column)] = columns.removeFromLeft(cellWidth);
            }

            const auto rowHeight = 18;
            const auto visibleRows = std::clamp(area.getHeight() / rowHeight, 1, rowCount);
            tableVisibleRows[static_cast<size_t>(tableIndex)] = visibleRows;
            tableScrollOffsets[static_cast<size_t>(tableIndex)] =
                std::clamp(tableScrollOffsets[static_cast<size_t>(tableIndex)], 0, std::max(0, rowCount - visibleRows));
            const auto firstVisibleRow = tableScrollOffsets[static_cast<size_t>(tableIndex)];

            for (int row = 0; row < rowCount; ++row) {
                const auto visible = row >= firstVisibleRow && row < firstVisibleRow + visibleRows;
                juce::Rectangle<int> rowArea;
                if (visible) {
                    rowArea = area.removeFromTop(rowHeight);
                    rowLabelBounds[static_cast<size_t>(tableIndex)][static_cast<size_t>(row)] = rowArea.removeFromLeft(30);
                } else {
                    rowLabelBounds[static_cast<size_t>(tableIndex)][static_cast<size_t>(row)] = {};
                }
                const auto valueWidth = visible ? rowArea.getWidth() / spec.columns : 0;
                for (int column = 0; column < maxColumns; ++column) {
                    auto* cell = tableCells[static_cast<size_t>(tableIndex)][static_cast<size_t>(row)][static_cast<size_t>(column)];
                    if (visible && column < spec.columns) {
                        cell->setVisible(true);
                        cell->setBounds(rowArea.removeFromLeft(valueWidth).reduced(2, 0));
                    } else {
                        cell->setVisible(false);
                    }
                }
            }
        }

        void scrollTable(int table, int delta)
        {
            auto& offset = tableScrollOffsets[static_cast<size_t>(table)];
            const auto visibleRows = tableVisibleRows[static_cast<size_t>(table)];
            const auto maxOffset = std::max(0, rowCount - std::max(1, visibleRows));
            const auto next = std::clamp(offset + delta, 0, maxOffset);
            if (next == offset) {
                return;
            }
            offset = next;
            resized();
            repaint();
        }

        void refreshActivity()
        {
            for (int table = 0; table < tableCount; ++table) {
                const auto length = lengthCells[static_cast<size_t>(table)]->value();
                for (int row = 0; row < rowCount; ++row) {
                    for (int column = 0; column < maxColumns; ++column) {
                        tableCells[static_cast<size_t>(table)][static_cast<size_t>(row)][static_cast<size_t>(column)]->setInactive(row >= length);
                    }
                }
            }
            repaint();
        }

        void paintOverChildren(juce::Graphics& graphics) override
        {
            graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            for (int table = 0; table < tableCount; ++table) {
                const auto& spec = specFor(table);
                auto header = drawTableHeaderBounds[static_cast<size_t>(table)].toFloat();
                const auto lengthBounds = lengthCells[static_cast<size_t>(table)]->getBounds().toFloat();
                const auto headerBody = header.withRight(lengthBounds.getX() - 4.0f);
                graphics.setColour(juce::Colour(0xff15191d));
                graphics.fillRect(headerBody);
                graphics.setColour(juce::Colour(colourLine).withAlpha(0.42f));
                graphics.drawRect(headerBody, 1.0f);
                graphics.drawRect(tableBounds[static_cast<size_t>(table)].toFloat().reduced(0.5f), 1.0f);
                graphics.setColour(juce::Colour(colourText));
                graphics.drawText(spec.title, header.removeFromLeft(70.0f), juce::Justification::centredLeft);
                graphics.setColour(juce::Colour(colourMuted));
                graphics.drawText("LEN", lengthBounds.translated(-38.0f, 0.0f).withWidth(34.0f), juce::Justification::centredRight);

                for (int column = 0; column < spec.columns; ++column) {
                    graphics.setColour(juce::Colour(colourMuted));
                    graphics.drawText(spec.labels[static_cast<size_t>(column)],
                                      columnLabelBounds[static_cast<size_t>(table)][static_cast<size_t>(column)],
                                      juce::Justification::centred);
                }

                const auto firstVisibleRow = tableScrollOffsets[static_cast<size_t>(table)];
                const auto visibleRows = tableVisibleRows[static_cast<size_t>(table)];
                for (int row = firstVisibleRow; row < std::min(rowCount, firstVisibleRow + visibleRows); ++row) {
                    const auto inactive = row >= lengthCells[static_cast<size_t>(table)]->value();
                    graphics.setColour(juce::Colour(inactive ? colourLine : colourAccent).withAlpha(inactive ? 0.35f : 0.75f));
                    graphics.drawText(juce::String::toHexString(row).paddedLeft('0', 2).toUpperCase(),
                                      rowLabelBounds[static_cast<size_t>(table)][static_cast<size_t>(row)],
                                      juce::Justification::centred);
                }
            }
        }

        static juce::Identifier lengthId(int table)
        {
            return juce::Identifier(juce::String(specFor(table).key) + "_len");
        }

        static juce::Identifier cellId(int table, int row, int column)
        {
            return juce::Identifier(juce::String(specFor(table).key) + "_" + juce::String(row) + "_" + juce::String(column));
        }

        juce::ValueTree state;
        juce::Label title;
        juce::Label presetLabel;
        juce::ComboBox presetBox;
        juce::TextEditor nameEditor;
        std::array<MetaCell, 3> metaCells {};
        int metaCellCount = 0;
        std::array<HexCell*, tableCount> lengthCells {};
        std::array<std::array<std::array<HexCell*, maxColumns>, rowCount>, tableCount> tableCells {};
        std::vector<std::unique_ptr<HexCell>> ownedCells;
        std::array<juce::Rectangle<int>, tableCount> drawTableHeaderBounds {};
        std::array<juce::Rectangle<int>, tableCount> tableBounds {};
        std::array<int, tableCount> tableScrollOffsets {};
        std::array<int, tableCount> tableVisibleRows {};
        std::array<std::array<juce::Rectangle<int>, maxColumns>, tableCount> columnLabelBounds {};
        std::array<std::array<juce::Rectangle<int>, rowCount>, tableCount> rowLabelBounds {};
    };

    class VoicePanel final : public juce::Component {
    public:
        VoicePanel(ReSidAudioProcessor& processor, APVTS& parameters, int voiceIndex)
            : audioProcessor(processor),
              parameterState(parameters),
              index(voiceIndex),
              scope(processor, voiceIndex),
              waveformSelector(parameters, juce::String("v") + juce::String(voiceIndex + 1) + "_waveform"),
              adsr(parameters, juce::String("v") + juce::String(voiceIndex + 1) + "_")
        {
            title.setText("OSCILLATOR " + juce::String(voiceIndex + 1), juce::dontSendNotification);
            title.setJustificationType(juce::Justification::centredLeft);
            title.setColour(juce::Label::textColourId, juce::Colour(colourText));
            title.setFont(juce::FontOptions(20.0f, juce::Font::bold));
            addAndMakeVisible(title);

            configureCombo(wavetableBox);
            wavetableBox.addItemList(juce::StringArray { "Off", "Pulse/Saw", "Pulse/Noise", "Octave Pulse", "Fifth Saw" }, 1);
            addAndMakeVisible(waveformSelector);
            addAndMakeVisible(wavetableBox);
            addLabel("WAVE", waveformLabel);
            addLabel("TABLE", wavetableLabel);

            configurePulseWidth(pulseWidthSlider);
            configureKnob(octaveSlider);
            configureKnob(detuneSlider);
            configureKnob(wavetableRateSlider);
            configureKnob(glideTimeSlider);
            glideTimeSlider.setTextValueSuffix(" ms");
            glideButton.setButtonText("ON");

            addSlider("PW", pulseWidthSlider, pulseWidthLabel);
            addSlider("OCT", octaveSlider, octaveLabel);
            addSlider("FINE", detuneSlider, detuneLabel);
            addSlider("RATE", wavetableRateSlider, wavetableRateLabel);
            addAndMakeVisible(glideButton);
            addSlider("TIME", glideTimeSlider, glideTimeLabel);
            addLabel("GLIDE", glideLabel);
            addAndMakeVisible(scope);
            addAndMakeVisible(adsr);

            const auto prefix = juce::String("v") + juce::String(voiceIndex + 1) + "_";
            wavetableAttachment = std::make_unique<APVTS::ComboBoxAttachment>(parameters, prefix + "wavetable", wavetableBox);
            pulseWidthAttachment = std::make_unique<APVTS::SliderAttachment>(parameters, prefix + "pulse_width", pulseWidthSlider);
            octaveAttachment = std::make_unique<APVTS::SliderAttachment>(parameters, prefix + "octave", octaveSlider);
            detuneAttachment = std::make_unique<APVTS::SliderAttachment>(parameters, prefix + "detune", detuneSlider);
            wavetableRateAttachment = std::make_unique<APVTS::SliderAttachment>(parameters, prefix + "wavetable_rate", wavetableRateSlider);
            glideAttachment = std::make_unique<APVTS::ButtonAttachment>(parameters, prefix + "glide", glideButton);
            glideTimeAttachment = std::make_unique<APVTS::SliderAttachment>(parameters, prefix + "glide_time", glideTimeSlider);
            syncControlsFromProcessor();
        }

        void syncControlsFromProcessor()
        {
            const auto prefix = juce::String("v") + juce::String(index + 1) + "_";
            const auto raw = [this, &prefix](const char* suffix) {
                return parameterState.getRawParameterValue(prefix + suffix)->load();
            };
            wavetableBox.setSelectedId(static_cast<int>(raw("wavetable")) + 1, juce::dontSendNotification);
            pulseWidthSlider.setValue(raw("pulse_width"), juce::dontSendNotification);
            octaveSlider.setValue(raw("octave"), juce::dontSendNotification);
            detuneSlider.setValue(raw("detune"), juce::dontSendNotification);
            wavetableRateSlider.setValue(raw("wavetable_rate"), juce::dontSendNotification);
            glideButton.setToggleState(raw("glide") > 0.5f, juce::dontSendNotification);
            glideTimeSlider.setValue(raw("glide_time"), juce::dontSendNotification);
        }

        void repaintScope()
        {
            scope.repaint();
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat();
            graphics.setColour(juce::Colour(colourPanel));
            graphics.fillRect(bounds);
            graphics.setColour(juce::Colour(colourLine).withAlpha(0.55f));
            graphics.drawRect(bounds.reduced(1.0f), 1.0f);

            auto stripe = bounds.removeFromTop(40.0f);
            graphics.setColour(index == 0 ? juce::Colour(colourAccent) : index == 1 ? juce::Colour(0xff5b7ea9) : juce::Colour(colourWarn));
            graphics.fillRect(stripe.withTrimmedBottom(24.0f).reduced(12.0f, 6.0f));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            title.setBounds(area.removeFromTop(32));

            auto selectors = area.removeFromTop(64);
            layoutLabelled(waveformLabel, waveformSelector, selectors.removeFromLeft(selectors.getWidth() / 2).reduced(0, 4));
            layoutLabelled(wavetableLabel, wavetableBox, selectors.reduced(0, 4));

            auto scopeArea = area.removeFromTop(92);
            scope.setBounds(scopeArea.reduced(0, 8));

            auto envelopeArea = area.removeFromTop(150);
            adsr.setBounds(envelopeArea.reduced(0, 6));

            area.removeFromTop(8);
            auto mainKnobs = area.removeFromTop(72);
            const int knobWidth = mainKnobs.getWidth() / 4;
            layoutLabelled(pulseWidthLabel, pulseWidthSlider, mainKnobs.removeFromLeft(knobWidth).reduced(4, 0));
            layoutLabelled(octaveLabel, octaveSlider, mainKnobs.removeFromLeft(knobWidth).reduced(4, 0));
            layoutLabelled(detuneLabel, detuneSlider, mainKnobs.removeFromLeft(knobWidth).reduced(4, 0));
            layoutLabelled(wavetableRateLabel, wavetableRateSlider, mainKnobs.removeFromLeft(knobWidth).reduced(4, 0));

            auto glideArea = area.removeFromTop(66);
            layoutLabelled(glideLabel, glideButton, glideArea.removeFromLeft(glideArea.getWidth() / 2).reduced(12, 0));
            layoutLabelled(glideTimeLabel, glideTimeSlider, glideArea.reduced(12, 0));
        }

    private:
        void addLabel(const juce::String& text, juce::Label& label)
        {
            label.setText(text, juce::dontSendNotification);
            label.setJustificationType(juce::Justification::centred);
            label.setColour(juce::Label::textColourId, juce::Colour(colourMuted));
            label.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            addAndMakeVisible(label);
        }

        void addSlider(const juce::String& text, juce::Slider& slider, juce::Label& label)
        {
            addAndMakeVisible(slider);
            addLabel(text, label);
        }

        ReSidAudioProcessor& audioProcessor;
        APVTS& parameterState;
        int index = 0;
        juce::Label title;
        juce::ComboBox wavetableBox;
        juce::Slider pulseWidthSlider;
        juce::Slider octaveSlider;
        juce::Slider detuneSlider;
        juce::Slider wavetableRateSlider;
        juce::ToggleButton glideButton;
        juce::Slider glideTimeSlider;
        juce::Label waveformLabel;
        juce::Label wavetableLabel;
        juce::Label pulseWidthLabel;
        juce::Label octaveLabel;
        juce::Label detuneLabel;
        juce::Label wavetableRateLabel;
        juce::Label glideLabel;
        juce::Label glideTimeLabel;
        ScopeComponent scope;
        WaveformSelector waveformSelector;
        AdsrComponent adsr;

        std::unique_ptr<APVTS::ComboBoxAttachment> wavetableAttachment;
        std::unique_ptr<APVTS::SliderAttachment> pulseWidthAttachment;
        std::unique_ptr<APVTS::SliderAttachment> octaveAttachment;
        std::unique_ptr<APVTS::SliderAttachment> detuneAttachment;
        std::unique_ptr<APVTS::SliderAttachment> wavetableRateAttachment;
        std::unique_ptr<APVTS::ButtonAttachment> glideAttachment;
        std::unique_ptr<APVTS::SliderAttachment> glideTimeAttachment;
    };

    void syncControlsFromProcessor()
    {
        const auto raw = [this](const char* id) {
            return parameterState.getRawParameterValue(id)->load();
        };
        chipBox.setSelectedId(static_cast<int>(raw("chip")) + 1, juce::dontSendNotification);
        voiceModeBox.setSelectedId(static_cast<int>(raw("voice_mode")) + 1, juce::dontSendNotification);
        filterModeBox.setSelectedId(static_cast<int>(raw("filter_mode")) + 1, juce::dontSendNotification);
        cleanSilenceBox.setSelectedId(static_cast<int>(raw("clean_silence")) + 1, juce::dontSendNotification);
        modWheelTargetBox.setSelectedId(static_cast<int>(raw("mod_wheel_target")) + 1, juce::dontSendNotification);
        cutoffSlider.setValue(raw("cutoff"), juce::dontSendNotification);
        resonanceSlider.setValue(raw("resonance"), juce::dontSendNotification);
        gainSlider.setValue(raw("gain"), juce::dontSendNotification);
        pitchBendRangeSlider.setValue(raw("pitch_bend_range"), juce::dontSendNotification);
        modWheelDepthSlider.setValue(raw("mod_wheel_depth"), juce::dontSendNotification);
        codeLegatoButton.setToggleState(raw("code_legato") > 0.5f, juce::dontSendNotification);
    }

    static void configureCombo(juce::ComboBox& combo)
    {
        combo.setJustificationType(juce::Justification::centred);
    }

    static void configureVertical(juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearVertical);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 18);
    }

    static void configurePulseWidth(juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
        slider.setNumDecimalPlacesToDisplay(0);
        slider.setDoubleClickReturnValue(true, 2048.0);
        slider.setMouseDragSensitivity(800);
        slider.textFromValueFunction = [](double value) {
            const auto sidValue = static_cast<int>(std::clamp(std::lround(value), 0l, 4095l));
            return "$" + juce::String::toHexString(sidValue).paddedLeft('0', 3).toUpperCase();
        };
        slider.valueFromTextFunction = [](const juce::String& text) {
            const auto trimmed = text.trim();
            const auto hasHexPrefix = trimmed.startsWithChar('$') || trimmed.startsWithChar('#') || trimmed.startsWithIgnoreCase("0x");
            auto cleaned = trimmed.removeCharacters("$# ");
            if (cleaned.startsWithIgnoreCase("0x")) {
                cleaned = cleaned.substring(2);
            }
            if (cleaned.isEmpty()) {
                return 2048.0;
            }
            const auto hex = hasHexPrefix || cleaned.containsAnyOf("ABCDEFabcdef") || cleaned.length() <= 3;
            const auto value = hex ? cleaned.getHexValue32() : cleaned.getIntValue();
            return static_cast<double>(std::clamp(value, 0, 4095));
        };
    }

    static void configureKnob(juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 18);
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    }

    static void layoutLabelled(juce::Label& label, juce::Component& component, juce::Rectangle<int> area)
    {
        label.setBounds(area.removeFromTop(18));
        if (dynamic_cast<juce::ComboBox*>(&component) != nullptr) {
            constexpr auto compactComboHeight = 24;
            component.setBounds(area.withSizeKeepingCentre(area.getWidth(), compactComboHeight));
        } else {
            component.setBounds(area);
        }
    }

    void addLabel(const juce::String& text, juce::Label& label)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, juce::Colour(colourMuted));
        label.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        addAndMakeVisible(label);
    }

    static constexpr int headerHeight = 72;

    ReSidAudioProcessor& audioProcessor;
    APVTS& parameterState;
    LookAndFeel lookAndFeel;
    std::array<std::unique_ptr<VoicePanel>, 3> voices;
    CodeEditorPanel codeEditor;
    juce::ComboBox chipBox;
    juce::ComboBox voiceModeBox;
    juce::ComboBox filterModeBox;
    juce::ComboBox cleanSilenceBox;
    juce::ComboBox modWheelTargetBox;
    juce::ToggleButton codeLegatoButton;
    juce::Slider cutoffSlider;
    juce::Slider resonanceSlider;
    juce::Slider gainSlider;
    juce::Slider pitchBendRangeSlider;
    juce::Slider modWheelDepthSlider;
    juce::Label chipLabel;
    juce::Label voiceModeLabel;
    juce::Label filterModeLabel;
    juce::Label cleanSilenceLabel;
    juce::Label cutoffLabel;
    juce::Label resonanceLabel;
    juce::Label gainLabel;
    juce::Label pitchBendRangeLabel;
    juce::Label modWheelTargetLabel;
    juce::Label modWheelDepthLabel;
    juce::Label codeLegatoLabel;
    std::unique_ptr<APVTS::ComboBoxAttachment> chipAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> voiceModeAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> filterModeAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> cleanSilenceAttachment;
    std::unique_ptr<APVTS::SliderAttachment> cutoffAttachment;
    std::unique_ptr<APVTS::SliderAttachment> resonanceAttachment;
    std::unique_ptr<APVTS::SliderAttachment> gainAttachment;
    std::unique_ptr<APVTS::SliderAttachment> pitchBendRangeAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> modWheelTargetAttachment;
    std::unique_ptr<APVTS::SliderAttachment> modWheelDepthAttachment;
    std::unique_ptr<APVTS::ButtonAttachment> codeLegatoAttachment;
};

template <typename Enum>
Enum enumFromChoice(const juce::AudioProcessorValueTreeState& parameters, const char* id)
{
    return static_cast<Enum>(static_cast<int>(*parameters.getRawParameterValue(id)));
}

float value(const juce::AudioProcessorValueTreeState& parameters, const char* id)
{
    return *parameters.getRawParameterValue(id);
}

float sidValue(const juce::AudioProcessorValueTreeState& parameters, const char* id, float maxValue)
{
    return std::clamp(value(parameters, id) / maxValue, 0.0f, 1.0f);
}

int treeInt(const juce::ValueTree& tree, const juce::Identifier& id, int fallback)
{
    if (!tree.isValid()) {
        return fallback;
    }
    return static_cast<int>(tree.getProperty(id, fallback));
}

juce::Identifier tableCellId(const juce::String& table, int row, int column)
{
    return juce::Identifier(table + "_" + juce::String(row) + "_" + juce::String(column));
}

int defaultCodeTableLength(const juce::String& table)
{
    return table == "wf" ? 4 : 0;
}

int defaultCodeTableValue(const juce::String& table, int row, int column)
{
    if (table == "wf") {
        static constexpr std::array<std::array<int, 3>, 4> drumRows {
            std::array<int, 3> { 0x81, 0xc8, 0xff },
            std::array<int, 3> { 0x81, 0xa4, 0xff },
            std::array<int, 3> { 0x81, 0xa4, 0xff },
            std::array<int, 3> { 0xfe, 0x02, 0xff }
        };
        if (row < static_cast<int>(drumRows.size()) && column < 3) {
            return drumRows[static_cast<size_t>(row)][static_cast<size_t>(column)];
        }
        const std::array<int, 3> defaults { 0x41, 0x00, 0xff };
        return column < static_cast<int>(defaults.size()) ? defaults[static_cast<size_t>(column)] : 0;
    }

    if (table == "chord" && column == 3) {
        return 0xff;
    }

    return 0x00;
}

} // namespace

ReSidAudioProcessor::ReSidAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, parameterStateId, createParameterLayout())
{
}

void ReSidAudioProcessor::prepareToPlay(double sampleRate, int)
{
    synth.prepare(sampleRate);
    synth.setSettings(readSettings());
    dcBlockLastInput = 0.0f;
    dcBlockLastOutput = 0.0f;
    scopeWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& scope : voiceScopes) {
        for (auto& sample : scope) {
            sample.store(0.0f, std::memory_order_relaxed);
        }
    }
}

void ReSidAudioProcessor::releaseResources()
{
    synth.allNotesOff();
}

bool ReSidAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void ReSidAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const auto settings = readSettings();
    synth.setSettings(settings);
    const auto cleanSilenceEnabled = value(parameters, "clean_silence") > 0.5f;

    auto midiIterator = midiMessages.cbegin();
    juce::MidiMessageMetadata metadata;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        while (midiIterator != midiMessages.cend() && (*midiIterator).samplePosition <= sample) {
            metadata = *midiIterator;
            const auto message = metadata.getMessage();

            if (message.isNoteOn()) {
                synth.noteOn(message.getNoteNumber(), message.getFloatVelocity());
            } else if (message.isNoteOff()) {
                synth.noteOff(message.getNoteNumber());
            } else if (message.isAllNotesOff() || message.isAllSoundOff()) {
                synth.allNotesOff();
            } else if (message.isPitchWheel()) {
                const auto wheel = message.getPitchWheelValue();
                const auto normalized = wheel >= 8192
                    ? static_cast<float>(wheel - 8192) / 8191.0f
                    : static_cast<float>(wheel - 8192) / 8192.0f;
                synth.setPitchBend(normalized);
            } else if (message.isController() && message.getControllerNumber() == 1) {
                synth.setModWheel(static_cast<float>(message.getControllerValue()) / 127.0f);
            } else if (message.isResetAllControllers()) {
                synth.setPitchBend(0.0f);
                synth.setModWheel(0.0f);
            }

            ++midiIterator;
        }

        auto sidSample = synth.nextSample();
        const auto dcBlocked = sidSample - dcBlockLastInput + 0.995f * dcBlockLastOutput;
        dcBlockLastInput = sidSample;
        dcBlockLastOutput = dcBlocked;

        sidSample = dcBlocked;
        if (cleanSilenceEnabled && !synth.hasActiveVoice() && std::abs(sidSample) < 0.01f) {
            sidSample = 0.0f;
        }

        const auto scopeIndex = scopeWriteIndex.fetch_add(1, std::memory_order_relaxed) & (scopeSize - 1);
        for (int voice = 0; voice < 3; ++voice) {
            voiceScopes[static_cast<size_t>(voice)][static_cast<size_t>(scopeIndex)].store(
                synth.debugVoiceSample(voice),
                std::memory_order_relaxed);
        }

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
            buffer.setSample(channel, sample, sidSample);
        }
    }
}

void ReSidAudioProcessor::copyVoiceScope(int voiceIndex, ScopeSnapshot& destination) const
{
    if (voiceIndex < 0 || voiceIndex >= 3) {
        destination.fill(0.0f);
        return;
    }

    const auto writeIndex = scopeWriteIndex.load(std::memory_order_relaxed);
    const auto& source = voiceScopes[static_cast<size_t>(voiceIndex)];
    for (int i = 0; i < scopeSize; ++i) {
        const auto sourceIndex = (writeIndex + i) & (scopeSize - 1);
        destination[static_cast<size_t>(i)] = source[static_cast<size_t>(sourceIndex)].load(std::memory_order_relaxed);
    }

}

juce::AudioProcessorEditor* ReSidAudioProcessor::createEditor()
{
    return new ReSidAudioProcessorEditor(*this);
}

void ReSidAudioProcessor::setCurrentProgram(int)
{
}

const juce::String ReSidAudioProcessor::getProgramName(int)
{
    return {};
}

void ReSidAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void ReSidAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream(destData, true);
    parameters.state.writeToStream(stream);
}

void ReSidAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto tree = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes)); tree.isValid()) {
        parameters.replaceState(tree);
    }
}

ReSidAudioProcessor::APVTS::ParameterLayout ReSidAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterChoice>("chip", "Chip", juce::StringArray { "6581", "8580" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("voice_mode", "Voice Mode", juce::StringArray { "Mono", "Poly", "Unison", "Code", "Code Poly" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("filter_mode", "Filter", juce::StringArray { "Off", "Lowpass", "Bandpass", "Highpass", "Notch" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("clean_silence", "Clean Silence", juce::StringArray { "Off", "On" }, 1));

    params.push_back(std::make_unique<juce::AudioParameterInt>("cutoff", "Cutoff", 0, 2047, 1535));
    params.push_back(std::make_unique<juce::AudioParameterInt>("resonance", "Resonance", 0, 15, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("gain", "Gain", juce::NormalisableRange<float>(0.0f, 1.5f, 0.01f), 0.8f));
    params.push_back(std::make_unique<juce::AudioParameterInt>("pitch_bend_range", "Pitch Bend Range", 1, 12, 2));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("mod_wheel_target", "Mod Wheel Target", juce::StringArray { "Off", "Vibrato", "Filter", "Pulse Width" }, 1));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("mod_wheel_depth", "Mod Wheel Depth", juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("code_legato", "Code Legato", false));

    for (int i = 0; i < 3; ++i) {
        const auto prefix = juce::String("v") + juce::String(i + 1) + "_";
        const auto name = juce::String("V") + juce::String(i + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterChoice>(prefix + "waveform", name + "Waveform", juce::StringArray { "Triangle", "Saw", "Pulse", "Noise", "Off" }, 2));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(prefix + "wavetable", name + "Wavetable", juce::StringArray { "Off", "Pulse/Saw", "Pulse/Noise", "Octave Pulse", "Fifth Saw" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(prefix + "wavetable_rate", name + "WT Rate", juce::NormalisableRange<float>(0.1f, 60.0f, 0.1f, 0.35f), 8.0f));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "octave", name + "Octave", -2, 2, 0));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(prefix + "detune", name + "Detune", juce::NormalisableRange<float>(-50.0f, 50.0f, 1.0f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "attack", name + "Attack", 0, 15, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "decay", name + "Decay", 0, 15, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "sustain", name + "Sustain", 0, 15, 15));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "release", name + "Release", 0, 15, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(prefix + "pulse_width", name + "Pulse Width", 0, 4095, 2048));
        params.push_back(std::make_unique<juce::AudioParameterBool>(prefix + "glide", name + "Glide", false));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(prefix + "glide_time", name + "Glide Time",
            juce::NormalisableRange<float>(0.0f, 2000.0f, 1.0f, 0.4f), 120.0f));
    }

    return { params.begin(), params.end() };
}

resid_vst::SidSynthSettings ReSidAudioProcessor::readSettings() const
{
    resid_vst::SidSynthSettings settings;
    settings.chipModel = enumFromChoice<resid_vst::ChipModel>(parameters, "chip");
    settings.voiceMode = enumFromChoice<resid_vst::VoiceMode>(parameters, "voice_mode");
    settings.filterMode = enumFromChoice<resid_vst::FilterMode>(parameters, "filter_mode");
    settings.cutoff = sidValue(parameters, "cutoff", 2047.0f);
    settings.resonance = sidValue(parameters, "resonance", 15.0f);
    settings.outputGain = value(parameters, "gain");
    settings.pitchBendRangeSemitones = value(parameters, "pitch_bend_range");
    settings.modWheelTarget = enumFromChoice<resid_vst::ModWheelTarget>(parameters, "mod_wheel_target");
    settings.modWheelDepth = value(parameters, "mod_wheel_depth");
    settings.codeLegato = value(parameters, "code_legato") > 0.5f;

    for (int i = 0; i < 3; ++i) {
        const auto prefix = juce::String("v") + juce::String(i + 1) + "_";
        auto& voice = settings.voice[static_cast<size_t>(i)];
        voice.waveform = enumFromChoice<resid_vst::Waveform>(parameters, (prefix + "waveform").toRawUTF8());
        voice.wavetable = enumFromChoice<resid_vst::WavetableMode>(parameters, (prefix + "wavetable").toRawUTF8());
        voice.wavetableRate = value(parameters, (prefix + "wavetable_rate").toRawUTF8());
        voice.octave = static_cast<int>(value(parameters, (prefix + "octave").toRawUTF8()));
        voice.detuneCents = value(parameters, (prefix + "detune").toRawUTF8());
        voice.attack = sidValue(parameters, (prefix + "attack").toRawUTF8(), 15.0f);
        voice.decay = sidValue(parameters, (prefix + "decay").toRawUTF8(), 15.0f);
        voice.sustain = sidValue(parameters, (prefix + "sustain").toRawUTF8(), 15.0f);
        voice.release = sidValue(parameters, (prefix + "release").toRawUTF8(), 15.0f);
        voice.pulseWidth = sidValue(parameters, (prefix + "pulse_width").toRawUTF8(), 4095.0f);
        voice.glideEnabled = value(parameters, (prefix + "glide").toRawUTF8()) > 0.5f;
        voice.glideTimeMs = value(parameters, (prefix + "glide_time").toRawUTF8());
    }

    auto codeState = parameters.state.getChildWithName("CodeInstrument");
    auto& code = settings.codeInstrument;
    code.adsr = static_cast<uint16_t>(std::clamp(treeInt(codeState, "adsr", 0x0800), 0, 0xffff));
    code.speed = static_cast<uint8_t>(std::clamp(treeInt(codeState, "speed", 0x00), 0, 0xff));
    code.vibrato = static_cast<uint16_t>(std::clamp(treeInt(codeState, "vibrato", 0x1000), 0, 0xffff));
    code.vibratoType = static_cast<uint8_t>(treeInt(codeState, "vibrato_type", 0x10) & 0x30);
    code.defaultPulseWidth = static_cast<uint16_t>(std::clamp(treeInt(codeState, "default_pulse_width", 0x0800), 0, 0x0fff));
    code.defaultFilterCutoff = static_cast<uint16_t>(std::clamp(treeInt(codeState, "default_filter_cutoff", 1535), 0, 2047));
    code.defaultFilterResonance = static_cast<uint8_t>(std::clamp(treeInt(codeState, "default_filter_resonance", 0), 0, 15));
    code.defaultFilterMode = static_cast<uint8_t>(std::clamp(treeInt(codeState, "default_filter_mode", 0x00), 0, 0x70));
    code.absolutePitch = static_cast<bool>(treeInt(codeState, "absolute_pitch", 0));
    code.fixedMidiNote = static_cast<uint8_t>(std::clamp(treeInt(codeState, "fixed_midi_note", 48), 0, 127));
    code.velocityRelease = static_cast<bool>(treeInt(codeState, "velocity_release", 0));
    code.gateOffWfRow = static_cast<uint8_t>(std::clamp(treeInt(codeState, "gate_off_wf_row", 0xff), 0, 0xff));
    code.gateOffPulseRow = static_cast<uint8_t>(std::clamp(treeInt(codeState, "gate_off_pulse_row", 0xff), 0, 0xff));
    code.gateOffFilterRow = static_cast<uint8_t>(std::clamp(treeInt(codeState, "gate_off_filter_row", 0xff), 0, 0xff));
    code.playbackRate = static_cast<uint8_t>(std::clamp(treeInt(codeState, "playback_rate", 1), 1, 4));
    code.wfLength = static_cast<uint8_t>(std::clamp(treeInt(codeState, "wf_len", defaultCodeTableLength("wf")), 0, resid_vst::SidCodeInstrument::rowCount));
    code.pulseLength = static_cast<uint8_t>(std::clamp(treeInt(codeState, "pulse_len", defaultCodeTableLength("pulse")), 0, resid_vst::SidCodeInstrument::rowCount));
    code.filterLength = static_cast<uint8_t>(std::clamp(treeInt(codeState, "filter_len", defaultCodeTableLength("filter")), 0, resid_vst::SidCodeInstrument::rowCount));
    code.chordLength = static_cast<uint8_t>(std::clamp(treeInt(codeState, "chord_len", defaultCodeTableLength("chord")), 0, resid_vst::SidCodeInstrument::rowCount));

    for (int row = 0; row < resid_vst::SidCodeInstrument::rowCount; ++row) {
        code.wf[static_cast<size_t>(row)].waveform = static_cast<uint8_t>(treeInt(codeState, tableCellId("wf", row, 0), defaultCodeTableValue("wf", row, 0)) & 0xff);
        code.wf[static_cast<size_t>(row)].arp = static_cast<uint8_t>(treeInt(codeState, tableCellId("wf", row, 1), defaultCodeTableValue("wf", row, 1)) & 0xff);
        code.wf[static_cast<size_t>(row)].detune = static_cast<uint8_t>(treeInt(codeState, tableCellId("wf", row, 2), defaultCodeTableValue("wf", row, 2)) & 0xff);

        code.pulse[static_cast<size_t>(row)].command = static_cast<uint8_t>(treeInt(codeState, tableCellId("pulse", row, 0), defaultCodeTableValue("pulse", row, 0)) & 0xff);
        code.pulse[static_cast<size_t>(row)].value = static_cast<uint8_t>(treeInt(codeState, tableCellId("pulse", row, 1), defaultCodeTableValue("pulse", row, 1)) & 0xff);
        code.pulse[static_cast<size_t>(row)].keyTrack = static_cast<uint8_t>(treeInt(codeState, tableCellId("pulse", row, 2), defaultCodeTableValue("pulse", row, 2)) & 0xff);

        code.filter[static_cast<size_t>(row)].command = static_cast<uint8_t>(treeInt(codeState, tableCellId("filter", row, 0), defaultCodeTableValue("filter", row, 0)) & 0xff);
        code.filter[static_cast<size_t>(row)].value = static_cast<uint8_t>(treeInt(codeState, tableCellId("filter", row, 1), defaultCodeTableValue("filter", row, 1)) & 0xff);
        code.filter[static_cast<size_t>(row)].keyTrack = static_cast<uint8_t>(treeInt(codeState, tableCellId("filter", row, 2), defaultCodeTableValue("filter", row, 2)) & 0xff);

        code.chord[static_cast<size_t>(row)].offset0 = static_cast<uint8_t>(treeInt(codeState, tableCellId("chord", row, 0), defaultCodeTableValue("chord", row, 0)) & 0xff);
        code.chord[static_cast<size_t>(row)].offset1 = static_cast<uint8_t>(treeInt(codeState, tableCellId("chord", row, 1), defaultCodeTableValue("chord", row, 1)) & 0xff);
        code.chord[static_cast<size_t>(row)].offset2 = static_cast<uint8_t>(treeInt(codeState, tableCellId("chord", row, 2), defaultCodeTableValue("chord", row, 2)) & 0xff);
        code.chord[static_cast<size_t>(row)].next = static_cast<uint8_t>(treeInt(codeState, tableCellId("chord", row, 3), defaultCodeTableValue("chord", row, 3)) & 0xff);
    }

    return settings;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReSidAudioProcessor();
}
