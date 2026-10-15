cmake_minimum_required(VERSION 3.21)
project(OpenCFDMesher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(APPLE)
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/opt/homebrew/opt/qt" "/usr/local")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "/opt/anaconda3")
elseif(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "C:/gmsh-sdk" "C:/Qt/6.8.0/msvc2022_64")
endif()

find_path(GMSH_INCLUDE_DIR gmsh.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include "C:/gmsh-sdk/include"
    REQUIRED
)
find_library(GMSH_LIBRARY NAMES gmsh libgmsh
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu "C:/gmsh-sdk/lib"
    REQUIRED
)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Gui)
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
    add_executable(OpenCFDMesher MACOSX_BUNDLE src/main.cpp)
    set_target_properties(OpenCFDMesher PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.simuoplabs.opencfdmesher"
        MACOSX_BUNDLE_BUNDLE_NAME "OpenCFDMesher"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        INSTALL_RPATH "@executable_path/../Frameworks;/opt/homebrew/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
elseif(WIN32)
    add_executable(OpenCFDMesher WIN32 src/main.cpp)
else()
    add_executable(OpenCFDMesher src/main.cpp)
endif()

target_include_directories(OpenCFDMesher PRIVATE ${GMSH_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(OpenCFDMesher PRIVATE
    ${GMSH_LIBRARY}
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Gui
    ${VTK_LIBRARIES}
)

if(WIN32)
    target_link_libraries(OpenCFDMesher PRIVATE psapi)
endif()

vtk_module_autoinit(
    TARGETS OpenCFDMesher
    MODULES ${VTK_LIBRARIES}
)

# ==============================================================================
# OS-SPECIFIC DEPENDENCY BUNDLING FOR CPACK INSTALLERS
# ==============================================================================

# 1. macOS (.dmg): Bundle all Qt, VTK, Gmsh & OpenCASCADE dylibs inside OpenCFDMesher.app
if(APPLE)
    install(TARGETS OpenCFDMesher BUNDLE DESTINATION .)

    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt
        HINTS /opt/homebrew/opt/qt/bin /usr/local/opt/qt/bin
        REQUIRED
    )

    install(CODE "
        message(STATUS \"Bundling all frameworks and dylibs into OpenCFDMesher.app...\")
        execute_process(COMMAND \"${MACDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\" -always-overwrite)
        execute_process(COMMAND codesign --force --deep -s - \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\")
    ")

# 2. Windows (.exe NSIS Installer): Bundle all transitive DLLs (Gmsh, OCCT, VTK, Qt6) + Qt plugins
elseif(WIN32)
    include(InstallRequiredSystemLibraries)

    install(TARGETS OpenCFDMesher
        RUNTIME DESTINATION bin
        RUNTIME_DEPENDENCIES
            PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" "hvsifiletrust\\.dll"
            POST_EXCLUDE_REGEXES ".*system32/.*\\.dll" ".*WinSxS/.*"
            DIRECTORIES "${CMAKE_PREFIX_PATH}/bin" "$ENV{CONDA_PREFIX}/Library/bin"
            RUNTIME DESTINATION bin
    )

    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${CMAKE_PREFIX_PATH}/bin" "$ENV{CONDA_PREFIX}/Library/bin"
        REQUIRED
    )

    install(CODE "
        message(STATUS \"Running windeployqt to bundle Qt6 plugins and OpenGL runtime...\")
        execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" --release --no-translations \"\${CMAKE_INSTALL_PREFIX}/bin/OpenCFDMesher.exe\")
    ")

# 3. Linux (.deb Package): Install binary + Desktop Menu launcher & auto-resolve APT dependencies
elseif(UNIX)
    install(TARGETS OpenCFDMesher RUNTIME DESTINATION bin)

    # Create a Linux Desktop Application entry so it appears in the Ubuntu/Debian App Menu
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/simuop-opencfd.desktop"
"[Desktop Entry]
Type=Application
Name=Simuop OpenCFD Mesher
Comment=FVM & FEM CAD-to-CFD Meshing Studio
Exec=OpenCFDMesher
Terminal=false
Categories=Science;Engineering;
")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/simuop-opencfd.desktop"
        DESTINATION share/applications
    )
endif()

# ==============================================================================
# CPACK INSTALLER GENERATORS (macOS .dmg | Windows .exe | Linux .deb)
# ==============================================================================
set(CPACK_PACKAGE_NAME "SimuopOpenCFDMesher")
set(CPACK_PACKAGE_VENDOR "Simuop Labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simuop Labs — FVM & FEM OpenCFD Meshing Studio")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Simuop Labs <https://simuoplabs.github.io>")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Simuop OpenCFD Mesher")
    set(CPACK_SYSTEM_NAME "macOS-arm64")

elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_SYSTEM_NAME "Windows-x64")
    set(CPACK_NSIS_DISPLAY_NAME "Simuop Labs OpenCFD Mesher")
    set(CPACK_NSIS_PACKAGE_NAME "Simuop Labs OpenCFD Mesher")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_PACKAGE_EXECUTABLES "OpenCFDMesher" "Simuop OpenCFD Mesher")
    set(CPACK_CREATE_DESKTOP_LINKS "OpenCFDMesher")

elseif(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_SYSTEM_NAME "Linux-x64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Simuop Labs <https://simuoplabs.github.io>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "science")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Automatically scans the binary with dpkg-shlibdeps and adds exact Qt6, VTK9, Gmsh & OCCT packages
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "gmsh")
endif()

set(CPACK_PACKAGE_FILE_NAME "SimuopOpenCFDMesher-${CPACK_SYSTEM_NAME}")
include(CPack)
