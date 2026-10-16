cmake_minimum_required(VERSION 3.21)
project(OpenCFDMesher VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT DEFINED PACKAGE_ARCH OR PACKAGE_ARCH STREQUAL "")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" TARGET_ARCH_LOWER)
    if(CMAKE_OSX_ARCHITECTURES)
        set(PACKAGE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    elseif(TARGET_ARCH_LOWER MATCHES "arm64|aarch64")
        set(PACKAGE_ARCH "arm64")
    else()
        set(PACKAGE_ARCH "x86_64")
    endif()
endif()

if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HB_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(PREPEND CMAKE_PREFIX_PATH "${HB_PREFIX}" "/opt/homebrew" "/usr/local")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "/opt/anaconda3" "/opt/miniconda3")
elseif(WIN32)
    if(DEFINED ENV{CONDA_PREFIX})
        string(REPLACE "\\" "/" CONDA_PREFIX_PATH "$ENV{CONDA_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${CONDA_PREFIX_PATH}/Library" "${CONDA_PREFIX_PATH}")
    endif()
elseif(UNIX)
    if(DEFINED ENV{CONDA_PREFIX})
        list(PREPEND CMAKE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
    endif()
endif()

find_path(GMSH_INCLUDE_DIR gmsh.h
    HINTS
        "$ENV{CONDA_PREFIX}/Library/include"
        "$ENV{CONDA_PREFIX}/include"
        "${HB_PREFIX}/include"
        /opt/homebrew/include
        /usr/local/include
        /usr/include
    REQUIRED
)

find_library(GMSH_LIBRARY NAMES gmsh libgmsh gmsh-4 gmsh4
    HINTS
        "$ENV{CONDA_PREFIX}/Library/lib"
        "$ENV{CONDA_PREFIX}/lib"
        "${HB_PREFIX}/lib"
        /opt/homebrew/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
    REQUIRED
)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
find_package(VTK REQUIRED COMPONENTS
    CommonCore
    CommonDataModel
    CommonColor
    CommonTransforms
    FiltersCore
    FiltersGeneral
    FiltersExtraction
    FiltersSources
    InteractionStyle
    InteractionWidgets
    RenderingCore
    RenderingOpenGL2
    RenderingAnnotation
    GUISupportQt
)

if(APPLE)
    add_executable(OpenCFDMesher MACOSX_BUNDLE src/main.cpp src/os_platform.h)
    set_target_properties(OpenCFDMesher PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.simuoplabs.opencfdmesher"
        MACOSX_BUNDLE_BUNDLE_NAME "OpenCFDMesher"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        INSTALL_RPATH "@executable_path/../Frameworks;${HB_PREFIX}/lib;/opt/homebrew/lib;/usr/local/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
elseif(WIN32)
    add_executable(OpenCFDMesher WIN32 src/main.cpp src/os_platform.h)
    target_compile_definitions(OpenCFDMesher PRIVATE NOMINMAX _USE_MATH_DEFINES)
    # Force standard main() entry point so WIN32 GUI subsystem links without WinMain errors
    target_link_options(OpenCFDMesher PRIVATE "/ENTRY:mainCRTStartup")
else()
    add_executable(OpenCFDMesher src/main.cpp src/os_platform.h)
    set_target_properties(OpenCFDMesher PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE
    )
endif()

target_include_directories(OpenCFDMesher PRIVATE ${GMSH_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(OpenCFDMesher PRIVATE
    ${GMSH_LIBRARY}
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ${VTK_LIBRARIES}
)

if(WIN32)
    target_link_libraries(OpenCFDMesher PRIVATE psapi opengl32 shell32 user32 gdi32)
endif()

vtk_module_autoinit(
    TARGETS OpenCFDMesher
    MODULES ${VTK_LIBRARIES}
)

if(APPLE)
    add_custom_command(TARGET OpenCFDMesher POST_BUILD
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:OpenCFDMesher>"
        COMMENT "Ad-hoc signing macOS bundle..."
    )
endif()

# ==============================================================================
# INSTALL & CPACK PACKAGING (.dmg, .exe, .deb)
# ==============================================================================
if(APPLE)
    install(TARGETS OpenCFDMesher BUNDLE DESTINATION .)
    execute_process(COMMAND brew --prefix qt OUTPUT_VARIABLE QT_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(MACDEPLOYQT_PATH "${QT_PREFIX}/bin/macdeployqt")

    install(CODE "
        execute_process(COMMAND \"${MACDEPLOYQT_PATH}\" \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\" -always-overwrite)
        execute_process(COMMAND codesign --force --deep -s - \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\")
    ")
elseif(WIN32)
    include(InstallRequiredSystemLibraries)
    install(TARGETS OpenCFDMesher RUNTIME DESTINATION bin)
    if(DEFINED ENV{CONDA_PREFIX})
        string(REPLACE "\\" "/" CONDA_WIN_PREFIX "$ENV{CONDA_PREFIX}")
        file(GLOB CONDA_DLLS "${CONDA_WIN_PREFIX}/Library/bin/*.dll")
        install(FILES ${CONDA_DLLS} DESTINATION bin)
        if(EXISTS "${CONDA_WIN_PREFIX}/Library/lib/qt6/plugins/platforms")
            install(DIRECTORY "${CONDA_WIN_PREFIX}/Library/lib/qt6/plugins/platforms" DESTINATION bin)
        endif()
        if(EXISTS "${CONDA_WIN_PREFIX}/Library/lib/qt6/plugins/styles")
            install(DIRECTORY "${CONDA_WIN_PREFIX}/Library/lib/qt6/plugins/styles" DESTINATION bin)
        endif()
    endif()
elseif(UNIX)
    install(TARGETS OpenCFDMesher RUNTIME DESTINATION bin)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/simuop-opencfd.desktop"
"[Desktop Entry]
Type=Application
Name=Simuop OpenCFD Mesher
Comment=FVM & FEM CAD-to-CFD Meshing Studio
Exec=OpenCFDMesher
Terminal=false
Categories=Science;Engineering;
")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/simuop-opencfd.desktop" DESTINATION share/applications)
endif()

set(CPACK_PACKAGE_NAME "SimuopOpenCFDMesher")
set(CPACK_PACKAGE_VENDOR "Simuop Labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simuop Labs — FVM & FEM OpenCFD Meshing Studio")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Simuop OpenCFD Mesher")
set(CPACK_PACKAGE_EXECUTABLES "OpenCFDMesher" "Simuop OpenCFD Mesher")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Simuop OpenCFD Mesher")
    set(CPACK_SYSTEM_NAME "macOS-${PACKAGE_ARCH}")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_SYSTEM_NAME "Windows-${PACKAGE_ARCH}")
    set(CPACK_NSIS_DISPLAY_NAME "Simuop OpenCFD Mesher (${PACKAGE_ARCH})")
    set(CPACK_NSIS_PACKAGE_NAME "Simuop OpenCFD Mesher")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
elseif(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_SYSTEM_NAME "Linux-x86_64")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Simuop Labs <https://simuoplabs.github.io>")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgl1, libegl1, libxkbcommon-x11-0, libxcb-cursor0")
endif()

set(CPACK_PACKAGE_FILE_NAME "SimuopOpenCFDMesher-${CPACK_SYSTEM_NAME}")
include(CPack)
