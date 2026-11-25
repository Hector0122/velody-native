cmake_minimum_required(VERSION 3.22)
project(VelodyNative VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 20)
if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
endif()

add_subdirectory(libs/tracktion_engine/modules/juce EXCLUDE_FROM_ALL) # EXCLUDE_FROM_ALL: keep JUCE/Tracktion headers and tools out of the package
add_subdirectory(libs/tracktion_engine/modules EXCLUDE_FROM_ALL)

juce_add_gui_app(Velody
    PRODUCT_NAME "Velody"
    VERSION ${PROJECT_VERSION}
    BUNDLE_ID "com.velody.native"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Velody graba desde tu interfaz de audio"
    NEEDS_CURL TRUE # HTTPS for cloud sync (JUCE's Linux HTTP backend is curl)
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(Velody)

target_sources(Velody PRIVATE Source/Main.cpp)
target_compile_definitions(Velody PRIVATE VELODY_PYTHON_WORKER_DIR="${CMAKE_SOURCE_DIR}/python-worker")

# The Tracktion demo components (timeline, plugin windows) are reused as-is from the submodule.
target_include_directories(Velody PRIVATE libs/tracktion_engine/examples)

target_compile_definitions(Velody PRIVATE
    JUCE_USE_MP3AUDIOFORMAT=1
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
    $<$<PLATFORM_ID:Linux>:JUCE_PLUGINHOST_LADSPA=1>
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_CURL=1
    JUCE_WEB_BROWSER=0)

target_link_libraries(Velody PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_recommended_config_flags)

if (APPLE)
    target_compile_options(Velody PRIVATE -fno-aligned-allocation)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Velody PRIVATE -latomic)
endif()

# Unit tests (juce::UnitTest, no extra dependency): ctest --test-dir build
enable_testing()
juce_add_console_app(VelodyTests PRODUCT_NAME "VelodyTests" NEEDS_CURL TRUE)
juce_generate_juce_header(VelodyTests)
target_sources(VelodyTests PRIVATE Tests/LibraryTests.cpp Tests/SongEditTests.cpp Tests/MixerPanelTests.cpp Tests/MetronomeTests.cpp Tests/PracticeTests.cpp Tests/PythonWorkerTests.cpp Tests/SongMetaTests.cpp Tests/ExporterTests.cpp Tests/SongInfoTests.cpp Tests/SongDeleteTests.cpp Tests/CloudTests.cpp Tests/CloudUiTests.cpp Tests/ImportQueueTests.cpp Tests/VibesTests.cpp Tests/PracticeMetaTests.cpp Tests/SetupTests.cpp Tests/LibraryAnalysisTests.cpp Tests/MixPanTests.cpp Tests/TrimTests.cpp Tests/RecordCountInTests.cpp Tests/SongSearchTests.cpp Tests/UpdaterTests.cpp Tests/PluginPathsTests.cpp)
target_compile_definitions(VelodyTests PRIVATE VELODY_PYTHON_WORKER_DIR="${CMAKE_SOURCE_DIR}/python-worker" JUCE_USE_MP3AUDIOFORMAT=1 TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_USE_CURL=1 JUCE_WEB_BROWSER=0)
target_link_libraries(VelodyTests PRIVATE
    tracktion::tracktion_core tracktion::tracktion_engine tracktion::tracktion_graph
    juce::juce_audio_devices juce::juce_audio_processors juce::juce_audio_utils juce::juce_cryptography
    juce::juce_recommended_config_flags)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(VelodyTests PRIVATE -latomic)
endif()
add_test(NAME VelodyTests COMMAND VelodyTests)

# Linux package: cmake --build build --target Velody && cd build && cpack -G DEB
# Named velody-native so it installs next to the Electron app's "velody" package.
include(GNUInstallDirs)
install(PROGRAMS $<TARGET_FILE:Velody> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME velody-native)
install(DIRECTORY python-worker/ DESTINATION ${CMAKE_INSTALL_DATADIR}/velody-native/python-worker
        PATTERN "__pycache__" EXCLUDE PATTERN ".pytest_cache" EXCLUDE PATTERN "tests" EXCLUDE)
install(FILES packaging/velody-native.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/velody-native.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)

set(CPACK_PACKAGE_NAME velody-native)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Hector Paramo")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Velody nativo: practica sobre stems separados, graba y usa plugins VST3/LV2")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # library dependencies worked out from the binary
# JUCE dlopens X11, JACK and GIO (trash) at runtime, so shlibdeps can't see them: listed by hand.
# Separation/analysis use Python + ffmpeg; Demucs lives in the venv the Electron app's setup creates.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "python3, ffmpeg, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libxi6, libxrender1, libglib2.0-0t64 | libglib2.0-0, libcurl4t64 | libcurl4")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire-jack | libjack-jackd2-0")
include(CPack)
