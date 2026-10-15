# Simuop Labs // OpenCFD Studio (`opencfd-mesher`)

An open-source, cross-platform **Finite Volume Method (FVM)** and **Finite Element Method (FEM)** 3D CAD preparation and meshing studio built by [Simuop Labs](https://simuoplabs.github.io/).

---

## Download Latest Official Installers

Click your operating system and processor below to download the self-contained installer directly:

| Operating System | Processor Architecture | Supported GPUs | Direct Installer Download |
| :--- | :--- | :--- | :--- |
| **Windows 10 / 11** | **x86_64 (AMD Ryzen & Intel Core/Xeon)** | NVIDIA, AMD Radeon, Intel | [**Download Windows x86_64 (.exe)**](https://github.com/simuoplabs/opencfd-mesher/releases/latest/download/SimuopOpenCFDMesher-Windows-x86_64-Intel-Ryzen.exe) |
| **Windows 11 ARM** | **ARM64 (Snapdragon / Windows on ARM)** | Qualcomm Adreno, NVIDIA, AMD | [**Download Windows ARM64 (.exe)**](https://github.com/simuoplabs/opencfd-mesher/releases/latest/download/SimuopOpenCFDMesher-Windows-arm64.exe) |
| **macOS (Apple Silicon)** | **ARM64 (M1 / M2 / M3 / M4)** | Apple Silicon GPU (Metal / OpenGL) | [**Download macOS Apple Silicon (.dmg)**](https://github.com/simuoplabs/opencfd-mesher/releases/latest/download/SimuopOpenCFDMesher-macOS-arm64.dmg) |
| **macOS (Intel)** | **x86_64 (Intel Mac)** | AMD Radeon, Intel Iris | [**Download macOS Intel (.dmg)**](https://github.com/simuoplabs/opencfd-mesher/releases/latest/download/SimuopOpenCFDMesher-macOS-x86_64.dmg) |
| **Linux (Ubuntu / Debian)** | **x86_64 (AMD Ryzen & Intel Core/Xeon)** | NVIDIA, AMD Radeon, Intel Mesa | [**Download Linux x86_64 (.deb)**](https://github.com/simuoplabs/opencfd-mesher/releases/latest/download/SimuopOpenCFDMesher-Linux-x86_64-Intel-Ryzen.deb) |

> **Linux Installation Note:** Run `sudo apt install ./SimuopOpenCFDMesher-Linux-x86_64-Intel-Ryzen.deb` to automatically fetch and install all required runtime libraries.

---

## Core Engineering Capabilities
- **Zero-Lag CAD Import & Surface Repair:** Native STEP (`.step`/`.stp`), IGES (`.iges`/`.igs`), and B-Rep (`.brep`) import with O(N) leak/free-edge detection, sliver removal, hole capping, and topology sewing.
- **Interactive Wind Tunnel & Turning/Yaw Flow:** 6-wall shaded fluid enclosure generator with auto-opening camera walls, 1-click boundary role assignment (`inlet`, `outlet`, `slip_wall`, `no_slip_wall`, `moving_ground`, `symmetry`), and cornering/sideslip flow setup.
- **Surface-Skin Paint Refinement:** Paint local mesh refinement directly onto the 3D body surface with live `[` and `]` brush thickness scaling.
- **Dual FVM & FEM Discretization Engine:**
  - **FVM (1st-Order Linear `Tet4` / `Tri3`):** Exports to **BARAM (`BaramFlow` / `BaramMesh`)**, **OpenFOAM (`gmshToFoam`)**, **Ansys Fluent/CFX**, and **SU2**.
  - **FEM (2nd-Order Quadratic `Tet10` / `Tri6`):** High-order curved Jacobian optimization with `*NSET` and `*ELSET` export for **CalculiX / Abaqus (`.inp`)**, **Elmer (`.unv`)**, and **FEniCS (`.msh`)**.
- **Out-of-Core SSD Cache (`mmap`):** Automatically streams 3D tetrahedral volume meshes to disk and reclaims OS heap RAM for massive meshes.

## License
Licensed under the **GNU General Public License v3.0 (`GPL-3.0-or-later`)**.
