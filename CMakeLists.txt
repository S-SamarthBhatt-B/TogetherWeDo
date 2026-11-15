cmake_minimum_required(VERSION 3.24)
project(TogetherWeDo VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Together Engine's current renderer and integration tests require Windows 10/11.")
endif()

if(MSVC AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "Together Engine requires Visual Studio 2022 or newer (MSVC 19.30 or newer).")
endif()

add_library(TogetherEngineCore STATIC
    Engine/Core/src/Logger.cpp
)
target_include_directories(TogetherEngineCore PUBLIC Engine/Core/include)
target_compile_features(TogetherEngineCore PUBLIC cxx_std_20)

add_library(TogetherEngineD3D11 STATIC
    Engine/Renderer/src/Camera.cpp
    Engine/Renderer/src/D3D11Renderer.cpp
)
target_include_directories(TogetherEngineD3D11 PUBLIC Engine/Renderer/include)
target_link_libraries(TogetherEngineD3D11 PUBLIC TogetherEngineCore d3d11 dxgi d3dcompiler)
target_compile_definitions(TogetherEngineD3D11 PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)

add_library(TogetherEngineScene STATIC
    Engine/Scene/src/Scene.cpp
)
target_include_directories(TogetherEngineScene PUBLIC Engine/Scene/include)
target_compile_features(TogetherEngineScene PUBLIC cxx_std_20)
target_link_libraries(TogetherEngineScene PUBLIC TogetherEngineCore)
target_compile_definitions(TogetherEngineScene PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)

add_library(TogetherEngineAssets STATIC
    Engine/Assets/src/AssetPipeline.cpp
    Engine/Assets/src/ThirdPartyImplementations.cpp
)
target_include_directories(TogetherEngineAssets PUBLIC Engine/Assets/include
    PRIVATE ThirdParty/cgltf ThirdParty/stb ThirdParty/dr_libs ThirdParty)
target_compile_features(TogetherEngineAssets PUBLIC cxx_std_20)
target_link_libraries(TogetherEngineAssets PUBLIC bcrypt)
target_compile_definitions(TogetherEngineAssets PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)

add_executable(TogetherWeDo WIN32 Game/Client/src/WindowsMain.cpp)
target_link_libraries(TogetherWeDo PRIVATE TogetherEngineD3D11 user32 gdi32)

add_executable(TogetherEngineEditor WIN32 Editor/src/WindowsEditor.cpp)
target_link_libraries(TogetherEngineEditor PRIVATE TogetherEngineD3D11 TogetherEngineScene TogetherEngineAssets user32 gdi32 comdlg32 shell32 uxtheme)
target_compile_definitions(TogetherEngineEditor PRIVATE "TWDO_ROOT_DIR=\"${CMAKE_SOURCE_DIR}\"")

include(CTest)
if(BUILD_TESTING)
    add_executable(TogetherEngineTests Tests/EngineTests.cpp)
    target_link_libraries(TogetherEngineTests PRIVATE TogetherEngineD3D11 user32 gdi32)
    add_test(NAME TogetherEngineTests COMMAND TogetherEngineTests)
    add_executable(TogetherEngineSceneTests Tests/SceneTests.cpp)
    target_link_libraries(TogetherEngineSceneTests PRIVATE TogetherEngineScene)
    add_test(NAME TogetherEngineSceneTests COMMAND TogetherEngineSceneTests)
    add_executable(TogetherEngineAssetTests Tests/AssetTests.cpp)
    target_link_libraries(TogetherEngineAssetTests PRIVATE TogetherEngineAssets)
    target_compile_definitions(TogetherEngineAssetTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    add_test(NAME TogetherEngineAssetTests COMMAND TogetherEngineAssetTests)
endif()

if(MSVC)
    set(engine_targets TogetherEngineCore TogetherEngineD3D11 TogetherEngineScene TogetherEngineAssets TogetherWeDo TogetherEngineEditor)
    if(BUILD_TESTING)
        list(APPEND engine_targets TogetherEngineTests TogetherEngineSceneTests TogetherEngineAssetTests)
    endif()
    foreach(target IN LISTS engine_targets)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endforeach()
else()
    set(engine_targets TogetherEngineCore TogetherEngineD3D11 TogetherEngineScene TogetherEngineAssets TogetherWeDo TogetherEngineEditor)
    if(BUILD_TESTING)
        list(APPEND engine_targets TogetherEngineTests TogetherEngineSceneTests TogetherEngineAssetTests)
    endif()
    foreach(target IN LISTS engine_targets)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
