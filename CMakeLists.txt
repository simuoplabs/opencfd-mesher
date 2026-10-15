cmake_minimum_required(VERSION 3.21)
project(OpenCFDMesher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" TARGET_ARCH_LOWER)
if(CMAKE_OSX_ARCHITECTURES)
    set(PACKAGE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
elseif(TARGET_ARCH_LOWER MATCHES "arm64|aarch64")
    set(PACKAGE_ARCH "arm64")
else()
    set(PACKAGE_ARCH "x86_64")
endif()

if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HB_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(PREPEND CMAKE_PREFIX_PATH "${HB_PREFIX}" "/opt/homebrew" "/usr/local")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "/opt/anaconda3")
elseif(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "C:/gmsh-sdk" "$ENV{CONDA_PREFIX}/Library")
endif()

find_path(GMSH_INCLUDE_DIR gmsh.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include "${HB_PREFIX}/include" "C:/gmsh-sdk/include" "$ENV{CONDA_PREFIX}/Library/include"
    REQUIRED
)
find_library(GMSH_LIBRARY NAMES gmsh libgmsh
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu "${HB_PREFIX}/lib" "C:/gmsh-sdk/lib" "$ENV{CONDA_PREFIX}/Library/lib"
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
        INSTALL_RPATH "@executable_path/../Frameworks;${HB_PREFIX}/lib;/opt/homebrew/lib;/usr/local/lib"
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
    target_link_libraries(OpenCFDMesher PRIVATE psapi opengl32)
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
# INSTALL & CPACK
# ==============================================================================
if(APPLE)
    install(TARGETS OpenCFDMesher BUNDLE DESTINATION .)
    execute_process(COMMAND brew --prefix qt OUTPUT_VARIABLE QT_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(MACDEPLOYQT_PATH "${QT_PREFIX}/bin/macdeployqt")

    install(CODE "
        message(STATUS \"Running macdeployqt from ${MAC_DEPLOYQT_PATH}...\")
        execute_process(COMMAND \"${MACDEPLOYQT_PATH}\" \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\" -always-overwrite)
        execute_process(COMMAND codesign --force --deep -s - \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\")
    ")
elseif(WIN32)
    include(InstallRequiredSystemLibraries)
    install(TARGETS OpenCFDMesher RUNTIME DESTINATION bin)
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
set(CPACK_PACKAGE_FILE_NAME "SimuopOpenCFDMesher-${CPACK_SYSTEM_NAME}")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Simuop OpenCFD Mesher")
    set(CPACK_SYSTEM_NAME "macOS-${PACKAGE_ARCH}")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_SYSTEM_NAME "Windows-${PACKAGE_ARCH}")
elseif(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_SYSTEM_NAME "Linux-x86_64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Simuop Labs <https://simuoplabs.github.io>")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "gmsh, libgl1, libegl1, mesa-utils")
endif()

include(CPack)
