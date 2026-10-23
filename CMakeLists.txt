cmake_minimum_required(VERSION 3.20)
project(ToneForge C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Windows 11 only runs on x86-64-v2 CPUs, so SSE4.2 is always available.
set(TF_ARCH_FLAGS -march=x86-64-v2)
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE ${TF_ARCH_FLAGS})

# Engine, DSP and app logic, shared by the GUI app and the offline self-test.
# An OBJECT library keeps every module's static registrar in the final link.
add_library(tfcore OBJECT
    src/audio/miniaudio_impl.c
    src/audio/AudioEngine.cpp
    src/audio/MidiInput.cpp
    src/util/Paths.cpp
    src/engine/Module.cpp
    src/engine/Registry.cpp
    src/engine/Rig.cpp
    src/engine/Preset.cpp
    src/engine/FactoryPresets.cpp
    src/modules/Dynamics.cpp
    src/modules/Drive.cpp
    src/modules/FilterEq.cpp
    src/modules/Modulation.cpp
    src/modules/TimeFx.cpp
    src/modules/Pitch.cpp
    src/modules/Amp.cpp
    src/modules/Cab.cpp)
target_include_directories(tfcore PUBLIC src third_party/miniaudio)
# Let the compiler vectorize filter loops; NaN/Inf handling stays IEEE-correct.
target_compile_options(tfcore PRIVATE ${TF_ARCH_FLAGS} -Wall -Wno-unused-function
    $<$<COMPILE_LANGUAGE:CXX>:-O3 -fno-math-errno -fno-trapping-math -fno-signed-zeros -fassociative-math -freciprocal-math>)
target_compile_definitions(tfcore PUBLIC _USE_MATH_DEFINES UNICODE _UNICODE)

set(TF_SYSTEM_LIBS winmm ole32 comdlg32 shell32 uuid)

option(TF_BUILD_GUI "Build the ToneForge GUI app" ON)
if(TF_BUILD_GUI)
add_executable(ToneForge WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/Settings.cpp
    src/ui/Widgets.cpp
    src/ui/Ui.cpp)
target_link_libraries(ToneForge PRIVATE tfcore imgui opengl32 gdi32 dwmapi ${TF_SYSTEM_LIBS})
target_compile_options(ToneForge PRIVATE ${TF_ARCH_FLAGS} -Wall)
target_link_options(ToneForge PRIVATE -static)
endif()

add_executable(tf_selftest tools/selftest.cpp)
target_link_libraries(tf_selftest PRIVATE tfcore ${TF_SYSTEM_LIBS})
target_compile_options(tf_selftest PRIVATE ${TF_ARCH_FLAGS} -Wall)
target_link_options(tf_selftest PRIVATE -static)
