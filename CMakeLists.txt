cmake_minimum_required(VERSION 3.25)

project(ReSIDVST VERSION 0.4.28 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RESID_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vice-3.10/src/resid")
set(RESID_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/resid")
configure_file(cmake/siddefs.h.in "${RESID_GENERATED_INCLUDE_DIR}/siddefs.h" @ONLY)

add_library(resid STATIC
    "${RESID_SOURCE_DIR}/sid.cc"
    "${RESID_SOURCE_DIR}/voice.cc"
    "${RESID_SOURCE_DIR}/wave.cc"
    "${RESID_SOURCE_DIR}/envelope.cc"
    "${RESID_SOURCE_DIR}/filter.cc"
    "${RESID_SOURCE_DIR}/dac.cc"
    "${RESID_SOURCE_DIR}/extfilt.cc"
    "${RESID_SOURCE_DIR}/pot.cc"
    "${RESID_SOURCE_DIR}/version.cc"
)
target_include_directories(resid PUBLIC "${RESID_SOURCE_DIR}" "${RESID_GENERATED_INCLUDE_DIR}")
target_compile_definitions(resid PRIVATE VERSION="${PROJECT_VERSION}")
set_target_properties(resid PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(resid_synth STATIC src/SidSynth.cpp)
target_include_directories(resid_synth PUBLIC include)
target_link_libraries(resid_synth PUBLIC resid)
set_target_properties(resid_synth PROPERTIES POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(ReSIDVST
    COMPANY_NAME "Oddvolt"
    PLUGIN_MANUFACTURER_CODE Oddv
    PLUGIN_CODE Rsid
    FORMATS VST3
    PRODUCT_NAME "reSID VST"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(ReSIDVST PRIVATE src/PluginProcessor.cpp src/PluginProcessor.h)
target_link_libraries(ReSIDVST PRIVATE
    resid_synth
    juce::juce_audio_utils
    juce::juce_audio_processors
)
target_compile_definitions(ReSIDVST PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

add_executable(resid_smoke tests/smoke.cpp)
target_link_libraries(resid_smoke PRIVATE resid_synth)

add_executable(plugin_load_smoke tests/plugin_load.cpp src/PluginProcessor.cpp src/PluginProcessor.h)
target_include_directories(plugin_load_smoke PRIVATE src)
target_link_libraries(plugin_load_smoke PRIVATE
    resid_synth
    juce::juce_audio_utils
    juce::juce_audio_processors
)
target_compile_definitions(plugin_load_smoke PRIVATE
    JucePlugin_Build_Standalone=0
    JucePlugin_Build_VST3=1
    JucePlugin_Name="reSID VST"
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_IsSynth=1
    JucePlugin_WantsMidiInput=1
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_Manufacturer="Oddvolt"
    JucePlugin_ManufacturerCode=0x4f646476
    JucePlugin_PluginCode=0x52736964
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
