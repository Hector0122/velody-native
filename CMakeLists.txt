cmake_minimum_required(VERSION 3.22)
project(VelodyNative VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
endif()

add_subdirectory(libs/tracktion_engine/modules/juce)
add_subdirectory(libs/tracktion_engine/modules)

juce_add_gui_app(Velody
    PRODUCT_NAME "Velody"
    BUNDLE_ID "com.velody.native"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Velody graba desde tu interfaz de audio"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(Velody)

target_sources(Velody PRIVATE Source/Main.cpp)

# The Tracktion demo components (timeline, plugin windows) are reused as-is from the submodule.
target_include_directories(Velody PRIVATE libs/tracktion_engine/examples)

target_compile_definitions(Velody PRIVATE
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
    $<$<PLATFORM_ID:Linux>:JUCE_PLUGINHOST_LADSPA=1>
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(Velody PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

if (APPLE)
    target_compile_options(Velody PRIVATE -fno-aligned-allocation)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Velody PRIVATE -latomic)
endif()

# Unit tests (juce::UnitTest, no extra dependency): ctest --test-dir build
enable_testing()
juce_add_console_app(VelodyTests PRODUCT_NAME "VelodyTests")
juce_generate_juce_header(VelodyTests)
target_sources(VelodyTests PRIVATE Tests/LibraryTests.cpp Tests/SongEditTests.cpp Tests/MixerPanelTests.cpp Tests/MetronomeTests.cpp)
target_compile_definitions(VelodyTests PRIVATE JUCE_USE_MP3AUDIOFORMAT=1 JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(VelodyTests PRIVATE
    tracktion::tracktion_core tracktion::tracktion_engine tracktion::tracktion_graph
    juce::juce_audio_devices juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_recommended_config_flags)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(VelodyTests PRIVATE -latomic)
endif()
add_test(NAME VelodyTests COMMAND VelodyTests)
