#include "PluginProcessor.h"

#include <iostream>
#include <functional>
#include <memory>

int main()
{
    std::unique_ptr<ReSidAudioProcessor> processor(dynamic_cast<ReSidAudioProcessor*>(createPluginFilter()));
    if (processor == nullptr) {
        std::cerr << "createPluginFilter returned null\n";
        return 1;
    }

    processor->prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
    if (editor == nullptr) {
        std::cerr << "createEditor returned null\n";
        return 1;
    }

    auto setChoice = [](ReSidAudioProcessor& target, const char* id, int choice) {
        auto* parameter = target.parameterState().getParameter(id);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(choice)));
    };

    setChoice(*processor, "chip", 1);
    setChoice(*processor, "voice_mode", 4);
    auto codeState = processor->parameterState().state.getOrCreateChildWithName("CodeInstrument", nullptr);
    codeState.setProperty("name", "shaker-horrr", nullptr);
    editor.reset(processor->createEditor());

    std::unique_ptr<ReSidAudioProcessor> secondProcessor(dynamic_cast<ReSidAudioProcessor*>(createPluginFilter()));
    std::unique_ptr<juce::AudioProcessorEditor> secondEditor(secondProcessor->createEditor());
    std::function<juce::Component*(juce::Component&, const juce::String&)> findComponent =
        [&](juce::Component& parent, const juce::String& id) -> juce::Component* {
            if (parent.getComponentID() == id) return &parent;
            for (auto* child : parent.getChildren()) {
                if (auto* found = findComponent(*child, id)) return found;
            }
            return nullptr;
        };
    auto* firstChip = dynamic_cast<juce::ComboBox*>(findComponent(*editor, "chip_selector"));
    auto* firstMode = dynamic_cast<juce::ComboBox*>(findComponent(*editor, "mode_selector"));
    auto* firstPreset = dynamic_cast<juce::ComboBox*>(findComponent(*editor, "code_preset_selector"));
    auto* secondChip = dynamic_cast<juce::ComboBox*>(findComponent(*secondEditor, "chip_selector"));
    auto* secondMode = dynamic_cast<juce::ComboBox*>(findComponent(*secondEditor, "mode_selector"));
    if (firstChip == nullptr || firstMode == nullptr || firstPreset == nullptr
        || secondChip == nullptr || secondMode == nullptr
        || firstChip->getSelectedId() != 2 || firstMode->getSelectedId() != 5
        || firstPreset->getText() != "shaker-horrr"
        || secondChip->getSelectedId() != 1 || secondMode->getSelectedId() != 1) {
        std::cerr << "multi-instance editor state synchronization failed: first="
                  << (firstChip != nullptr ? firstChip->getSelectedId() : -1) << "/"
                  << (firstMode != nullptr ? firstMode->getSelectedId() : -1) << "/"
                  << (firstPreset != nullptr ? firstPreset->getText() : "missing")
                  << " second=" << (secondChip != nullptr ? secondChip->getSelectedId() : -1)
                  << "/" << (secondMode != nullptr ? secondMode->getSelectedId() : -1) << "\n";
        return 1;
    }
    processor->releaseResources();

    std::cout << "loaded " << processor->getName() << "\n";
    return 0;
}
