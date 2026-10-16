cmake_minimum_required(VERSION 3.21)
project(OpenCFDMesher VERSION 1.0.0 LANGUAGES C CXX)

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
    # Ensure standard main() works seamlessly even with WIN32 subsystem
    set_target_properties(OpenCFDMesher PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
else()
    add_executable(OpenCFDMesher src/main.cpp src/os_platform.h)
    set_target_properties(OpenCFDMesher PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE
    )
endif()

target_include_directories(OpenCFDMesher PRIVATE ${GMSH_INCLUDE_DIR}${CMAKE_CURRENT_SOURCE_DIR}/src)

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

# ==============================================================================
# INSTALL & CPACK PACKAGING
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
    # Bundle all required runtime DLLs and Qt plugins from Conda into bin/
    if(DEFINED ENV{CONDA_PREFIX})
        string(REPLACE "\\" "/" CONDA_PREFIX_UNIX "$ENV{CONDA_PREFIX}")
        install(CODE "
            file(GLOB CONDA_DLLS \"${CONDA_PREFIX_UNIX}/Library/bin/*.dll\")
            fileLooking closely at the exact logs and why the previous iterations kept hitting walls, there are four distinct, underlying engineering problems across your build matrix. Fixing them properly requires addressing the root causes in both CMake and the GitHub Actions runner setup rather than just swapping package managers back and forth.

---

### Root Cause Breakdown

1. **Linux (`Qt5::Core` vs `Qt6` Collision & Conda `libstdc++` trap):**
   * **Why APT failed:** On Ubuntu 24.04, `libvtk9-qt-dev` is compiled against **Qt5**. When your `CMakeLists.txt` loads `Qt6` and then loads `VTK` with `GUISupportQt`, VTK's CMake config forcefully injects `Qt5::Widgets` and `Qt5::Core` into your target's link interface. CMake immediately halts because a single target cannot link both Qt5 and Qt6.
   * **Why Conda on Linux is dangerous for `.deb`:** While `conda-forge` has VTK 9 built against Qt6, compiling against Conda libraries links your binary to `$CONDA_PREFIX/lib/libvtk...so` and Conda's newer `libstdc++`. If you package only the executable into a `.deb`, it will crash immediately on any user's Ubuntu machine with `error while loading shared libraries: libvtkCommonCore-9.x.so: cannot open shared object file`. Furthermore, Conda's compilers look for a custom sysroot unless you use Ubuntu's native `g++` or bundle the libraries into the `.deb`.
   * **The Fix:** On Linux, we use `conda-forge` **strictly as the library provider** (`qt6-main`, `vtk`, `gmsh`), compile using the system GCC, set `INSTALL_RPATH` to `$ORIGIN/../lib`, and have CMake bundle the required shared libraries directly into the `.deb` package (`/usr/lib/opencfd-mesher`) so the `.deb` is 100% self-contained and installs cleanly on any Debian/Ubuntu distro.

2. **macOS Intel (`macos-13` hanging indefinitely):**
   * **Why it hung:** GitHub officially deprecated and decommissioned the `macos-13` runner image cohort. Jobs requesting `macos-13` sit in the queue forever waiting for a runner that no longer exists.
   * **The Fix:** Use `macos-15-intel` (or cross-compile `x86_64` from `macos-14` / `macos-15`). Using `macos-15-intel` gives you a live, native x86_64 macOS runner immediately.

3. **Windows ARM64 (`LNK4272: library machine type 'x64' conflicts with target machine type 'ARM64'`):**
   * **Why it failed:** `conda-forge` does not publish `win-arm64` builds for VTK or Gmsh—every `.lib` downloaded into `C:\Miniconda\envs\test\Library\lib` is an `x64` binary. You cannot link an `ARM64` `.obj` against `x64` `.lib` files.
   * **The Fix:** Dropping the `amd64_arm64` matrix entry and shipping the `Windows-x86_64` build is the right call. Windows 11 on ARM runs `x86_64` binaries transparently via Microsoft's Prism emulator with near-native performance.

4. **Windows x86_64 Packaging (Silent Empty ZIP bug):**
   * **Why it matters:** In your previous workflow, `windeployqt build/OpenCFDMesher.exe` deployed the Qt DLLs into `build/`, and then `cpack -G ZIP` only packaged the `install(TARGETS ...)` binary—leaving all the Qt, VTK, and Gmsh `.dll` files behind! Anyone downloading that `.zip` on Windows would get `Qt6Core.dll was not found`.
   * **The Fix:** Run `windeployqt` and copy the runtime `.dll`s directly during CMake's `install()` phase so `cpack -G ZIP` packages the `.exe` **and** every required `.dll` into a single, ready-to-run folder.

---

### Step 1: Replace `CMakeLists.txt`

Run this block in your terminal to update `CMakeLists.txt` with proper RPATH handling, Windows DLL bundling, and Linux library bundling:

```bash
cd ~/baram_mesher

cat << 'EOF' > CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(OpenCFDMesher VERSION 1.0.0 LANGUAGES CXX C)

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
    if(DEFINED ENV{CONDA_PREFIX})
        string(REPLACE "\\" "/" CONDA_PREFIX_PATH "$ENV{CONDA_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${CONDA_PREFIX_PATH}/Library")
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
    target_link_libraries(OpenCFDMesher PRIVATE Qt6::EntryPoint psapi opengl32 shell32 user32 gdi32)
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
        execute_process(COMMAND \"${MACDEPLOYQT_PATH}\" \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\" -always-overwrite)
        execute_process(COMMAND codesign --force --deep -s - \"\${CMAKE_INSTALL_PREFIX}/OpenCFDMesher.app\")
    ")
elseif(WIN32)
    include(InstallRequiredSystemLibraries)
    install(TARGETS OpenCFDMesher RUNTIME DESTINATION bin)
    if(DEFINED ENV{CONDA_PREFIX})
        string(REPLACE "\\" "/" CONDA_WIN_BIN "$ENV{CONDA_PREFIX}/Library/bin")
        file(GLOB CONDA_DLLS "${CONDA_WIN_BIN}/*.dll")
        install(FILES ${CONDA_DLLS} DESTINATION bin)
        install(DIRECTORY "$ENV{CONDA_PREFIX}/Library/lib/qt6/plugins/platforms" DESTINATION bin OPTIONAL)
        install(DIRECTORY "$ENV{CONDA_PREFIX}/Library/lib/qt6/plugins/styles" DESTINATION bin OPTIONAL)
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
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Simuop Labs [https://simuoplabs.github.io](https://simuoplabs.github.io)")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgl1, libegl1, libxkbcommon-x11-0, libxcb-cursor0")
endif()

set(CPACK_PACKAGE_FILE_NAME "SimuopOpenCFDMesher-${CPACK_SYSTEM_NAME}")
include(CPack)
