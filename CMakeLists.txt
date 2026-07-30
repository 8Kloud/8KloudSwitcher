cmake_minimum_required(VERSION 3.25)
project(KloudSwitcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Threads REQUIRED)
find_package(NDI)
find_package(Vulkan REQUIRED)

add_library(kloud_core STATIC
  src/core/MediaClock.cpp
  src/core/Stats.cpp
  src/engine/SwitcherCore.cpp
  src/audio/MixerCore.cpp
  src/audio/AudioEngine.cpp
  src/ctl/ControlProtocol.cpp
)
target_include_directories(kloud_core PUBLIC src)
target_link_libraries(kloud_core PUBLIC Threads::Threads)
target_compile_options(kloud_core PRIVATE -Wall -Wextra)

# -- shaders: GLSL -> SPIR-V (glslc) -> embedded C arrays --
find_program(GLSLC glslc REQUIRED)
set(KLOUD_SHADER_HDRS "")
function(kloud_shader name src)
  set(spv ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
  set(hdr ${CMAKE_BINARY_DIR}/shaders/${name}.spv.h)
  add_custom_command(OUTPUT ${hdr}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} -O --target-env=vulkan1.3 -I ${CMAKE_SOURCE_DIR}/shaders
            -o ${spv} ${CMAKE_SOURCE_DIR}/shaders/${src}
    COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${src} ${CMAKE_SOURCE_DIR}/shaders/ycbcr.glsl
            ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    VERBATIM)
  list(APPEND KLOUD_SHADER_HDRS ${hdr})
  set(KLOUD_SHADER_HDRS ${KLOUD_SHADER_HDRS} PARENT_SCOPE)
endfunction()
kloud_shader(composite_comp composite.comp)
kloud_shader(multiview_tile_comp multiview_tile.comp)
kloud_shader(pack_uyvy_comp pack_uyvy.comp)
kloud_shader(pack_nv12_comp pack_nv12.comp)
kloud_shader(proxy_down_comp proxy_down.comp)
add_custom_target(kloud_shaders DEPENDS ${KLOUD_SHADER_HDRS})

add_library(kloud_gpu STATIC
  src/gpu/VkEngine.cpp
  src/gpu/UploadRing.cpp
  src/gpu/Nv12Ring.cpp
  src/gpu/Compositor.cpp
)
target_link_libraries(kloud_gpu PUBLIC kloud_core Vulkan::Vulkan)
target_include_directories(kloud_gpu PUBLIC ${CMAKE_BINARY_DIR})
target_compile_options(kloud_gpu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_dependencies(kloud_gpu kloud_shaders)

# -- CUDA driver API (libcuda from the driver; headers from the toolkit) --
find_library(CUDA_DRIVER_LIB cuda HINTS /usr/lib /usr/lib64)
find_path(CUDA_DRIVER_INC cuda.h HINTS /opt/cuda/include /usr/local/cuda/include)
if(NOT CUDA_DRIVER_LIB OR NOT CUDA_DRIVER_INC)
  message(FATAL_ERROR "CUDA driver API not found (libcuda.so + cuda.h)")
endif()
add_library(CudaDriver INTERFACE)
target_link_libraries(CudaDriver INTERFACE ${CUDA_DRIVER_LIB})
target_include_directories(CudaDriver SYSTEM INTERFACE ${CUDA_DRIVER_INC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavcodec libavformat libavutil libavfilter libswresample libswscale)

pkg_check_modules(FFNVCODEC REQUIRED IMPORTED_TARGET ffnvcodec)

add_library(kloud_media STATIC
  src/media/CudaCtx.cpp
  src/media/FfmpegNvenc.cpp
  src/media/NvencDirect.cpp
  src/media/VideoEncoder.cpp
  src/media/AacEncoder.cpp
)
target_link_libraries(kloud_media PUBLIC kloud_gpu CudaDriver PkgConfig::FFMPEG
                      PkgConfig::FFNVCODEC ${CMAKE_DL_LIBS})
target_compile_options(kloud_media PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# -- OMT (Open Media Transport): optional, vendored in third_party/omt.
# Build once with third_party/{libvmx,libomt} (see docs/omt.md); libomt.so is
# a NativeAOT build that dlopens libvmx.so from the same directory.
set(OMT_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/omt CACHE PATH "OMT SDK root")
if(EXISTS ${OMT_SDK_DIR}/include/libomt.h AND EXISTS ${OMT_SDK_DIR}/lib/libomt.so)
  set(OMT_FOUND TRUE)
  add_library(OMT::omt SHARED IMPORTED)
  set_target_properties(OMT::omt PROPERTIES
    IMPORTED_LOCATION ${OMT_SDK_DIR}/lib/libomt.so
    INTERFACE_INCLUDE_DIRECTORIES ${OMT_SDK_DIR}/include)
  set(_omt_libdir ${OMT_SDK_DIR}/lib)
  message(STATUS "OMT SDK found: ${OMT_SDK_DIR}")
else()
  set(OMT_FOUND FALSE)
  message(STATUS "OMT SDK not found (${OMT_SDK_DIR}) - OMT input disabled.")
endif()

if(NDI_FOUND)
  add_library(kloud_ndi STATIC src/ndi/NdiLib.cpp)
  target_link_libraries(kloud_ndi PUBLIC kloud_core NDI::ndi)
  target_compile_options(kloud_ndi PRIVATE -Wall -Wextra)

  get_filename_component(_ndi_libdir ${NDI_LIBRARY} DIRECTORY)

  foreach(tool kloud-testgen kloud-latmeter)
    add_executable(${tool} tools/${tool}/main.cpp)
    target_link_libraries(${tool} PRIVATE kloud_ndi)
    target_include_directories(${tool} PRIVATE tools)
    target_compile_options(${tool} PRIVATE -Wall -Wextra)
    # Locate libndi.so.6 next to the SDK without requiring ldconfig.
    set_target_properties(${tool} PROPERTIES
      BUILD_RPATH ${_ndi_libdir}
      INSTALL_RPATH ${_ndi_libdir})
  endforeach()
  if(OMT_FOUND)
    # testgen gains an OMT sender for benching (--omt).
    target_compile_definitions(kloud-testgen PRIVATE KLOUD_HAVE_OMT)
    target_link_libraries(kloud-testgen PRIVATE OMT::omt)
    set_target_properties(kloud-testgen PROPERTIES
      BUILD_RPATH "${_ndi_libdir};${_omt_libdir}"
      INSTALL_RPATH "${_ndi_libdir};${_omt_libdir}")
  endif()

  add_library(kloud_engine STATIC
    src/engine/Engine.cpp
    src/ctl/ControlServer.cpp
    src/in/SrtInput.cpp
    src/in/StillInput.cpp
    src/ndi/NdiFinder.cpp
    src/ndi/NdiReceiver.cpp
    src/out/FileRecorder.cpp
    src/out/NdiOutput.cpp
    src/out/SrtOutput.cpp
  )
  target_link_libraries(kloud_engine PUBLIC kloud_gpu kloud_ndi kloud_media)
  target_compile_options(kloud_engine PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  if(OMT_FOUND)
    target_sources(kloud_engine PRIVATE src/omt/OmtInput.cpp)
    target_compile_definitions(kloud_engine PUBLIC KLOUD_HAVE_OMT)
    target_link_libraries(kloud_engine PUBLIC OMT::omt)
  endif()

  set(_kloud_rpath ${_ndi_libdir})
  if(OMT_FOUND)
    set(_kloud_rpath "${_ndi_libdir};${_omt_libdir}")
  endif()

  add_executable(kloud-headless tools/kloud-headless/main.cpp)
  target_link_libraries(kloud-headless PRIVATE kloud_engine)
  target_include_directories(kloud-headless PRIVATE tools)
  set_target_properties(kloud-headless PROPERTIES
    BUILD_RPATH "${_kloud_rpath}" INSTALL_RPATH "${_kloud_rpath}")

  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6_FOUND)
    add_executable(8kloud-switcher
      src/ui/main.cpp
      src/ui/MainWindow.cpp
      src/ui/MainWindow.h
      src/ui/MultiviewWidget.cpp
      src/ui/MultiviewWidget.h
      src/ui/EngineBridge.cpp
      src/ui/EngineBridge.h
      src/ui/MixerPanel.cpp
      src/ui/MixerPanel.h
      src/ui/ShowFile.cpp
      src/ui/ShowFile.h
      src/ui/resources.qrc
    )
    set_target_properties(8kloud-switcher PROPERTIES
      AUTOMOC ON
      AUTORCC ON
      BUILD_RPATH "${_kloud_rpath}"
      INSTALL_RPATH "${_kloud_rpath}")
    target_link_libraries(8kloud-switcher PRIVATE kloud_engine Qt6::Widgets)
    target_compile_options(8kloud-switcher PRIVATE -Wall -Wextra)
  else()
    message(WARNING "Qt6 Widgets not found - GUI app will not be built.")
  endif()
else()
  message(WARNING "NDI SDK not found - NDI tools will not be built. "
                  "Set NDI_SDK_DIR or populate third_party/ndi.")
endif()

option(KLOUD_BUILD_TESTS "Build unit tests" ON)
if(KLOUD_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
  )
  FetchContent_MakeAvailable(Catch2)

  enable_testing()
  add_executable(kloud_tests
    tests/clock_test.cpp
    tests/ring_test.cpp
    tests/switcher_core_test.cpp
    tests/framesync_test.cpp
    tests/strip_test.cpp
    tests/audio_test.cpp
    tests/aac_test.cpp
    tests/control_protocol_test.cpp
    tests/media_playlist_test.cpp
    tests/gpu_composite_test.cpp
    tests/interop_test.cpp
    tests/nvenc_direct_test.cpp
  )
  target_link_libraries(kloud_tests PRIVATE kloud_core kloud_gpu kloud_media Catch2::Catch2WithMain)
  target_include_directories(kloud_tests PRIVATE tools)

  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(kloud_tests)

  if(TARGET 8kloud-switcher)
    add_executable(kloud_ui_tests
      tests/ui_output_format_test.cpp
      tests/ui_input_picker_test.cpp
      src/ui/MainWindow.cpp
      src/ui/MainWindow.h
      src/ui/MultiviewWidget.cpp
      src/ui/MultiviewWidget.h
      src/ui/EngineBridge.cpp
      src/ui/EngineBridge.h
      src/ui/MixerPanel.cpp
      src/ui/MixerPanel.h
      src/ui/ShowFile.cpp
      src/ui/ShowFile.h
    )
    set_target_properties(kloud_ui_tests PROPERTIES
      AUTOMOC ON
      BUILD_RPATH "${_kloud_rpath}"
      INSTALL_RPATH "${_kloud_rpath}")
    target_link_libraries(kloud_ui_tests PRIVATE
      kloud_engine Qt6::Widgets Catch2::Catch2WithMain)
    target_compile_options(kloud_ui_tests PRIVATE -Wall -Wextra)
    catch_discover_tests(kloud_ui_tests
      TEST_PREFIX "ui: "
      PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
endif()
