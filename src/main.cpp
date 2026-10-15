#include "os_platform.h"
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdint>
#include <gmsh.h>

// macOS Native Kernel & Virtual Memory Headers (RAM Telemetry + mmap Disk Cache)
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#if !defined(_WIN32)
#include <fcntl.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif

// Qt6 Includes
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabWidget>
#include <QPushButton>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QFrame>
#include <QSurfaceFormat>
#include <QButtonGroup>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDateTime>
#include <QScrollBar>
#include <QRadioButton>
#include <QGroupBox>
#include <QEventLoop>
#include <QTimer>

// VTK Includes
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkCamera.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkUnstructuredGrid.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkFloatArray.h>
#include <vtkIntArray.h>
#include <vtkCellData.h>
#include <vtkPointData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkPolyDataMapper.h>
#include <vtkDataSetMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkCellPicker.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkTriangle.h>
#include <vtkLine.h>
#include <vtkTetra.h>
#include <vtkPlane.h>
#include <vtkCubeSource.h>
#include <vtkAxesActor.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCaptionActor2D.h>
#include <vtkTextProperty.h>

// ============================================================================
// OUT-OF-CORE SSD MESH CACHE & MACOS RAM MANAGER (POSIX mmap + libmalloc)
// ============================================================================
#pragma pack(push, 1)
struct CachedNode {
    float x, y, z; // 12 bytes per node
};

struct CachedTet {
    uint32_t n[4]; // 16 bytes per tetrahedron (corner vertices)
};
#pragma pack(pop)

class OutOfCoreMeshCache {
public:
    std::string cacheDir;
    std::string nodesPath;
    std::string tetsPath;
    std::string mshCachePath;

    size_t numCachedNodes = 0;
    size_t numCachedTets = 0;
    double cachedDiskMB = 0.0;
    bool hasDiskCache = false;

    OutOfCoreMeshCache(const std::string& dir = "/tmp/simuop_cfd_cache") : cacheDir(dir) {
        mkdir(cacheDir.c_str(), 0755);
        nodesPath    = cacheDir + "/nodes.bin";
        tetsPath     = cacheDir + "/tets.bin";
        mshCachePath = cacheDir + "/full_volume_cache.msh";
    }

    ~OutOfCoreMeshCache() {
        clearDiskCache();
    }

    static double getCurrentRamUsageMB() {
        task_vm_info_data_t vmInfo;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count) == KERN_SUCCESS) {
            return static_cast<double>(vmInfo.phys_footprint) / (1024.0 * 1024.0);
        }
        return 0.0;
    }

    static void releaseFreeRamToMacOS() {
        malloc_zone_pressure_relief(nullptr, 0);
    }

    static double getFileSizeMB(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return static_cast<double>(st.st_size) / (1024.0 * 1024.0);
        }
        return 0.0;
    }

    // Supports both 1st-order Tet4 (type 4) and 2nd-order Quadratic Tet10 (type 11) for FVM & FEM!
    bool offload3DMeshToDiskAndFreeRAM(bool saveMshBackup = true) {
        hasDiskCache = false;
        numCachedNodes = 0;
        numCachedTets = 0;
        cachedDiskMB = 0.0;

        if (saveMshBackup) {
            gmsh::option::setNumber("Mesh.MshFileVersion", 4.1);
            gmsh::option::setNumber("Mesh.Binary", 1);
            gmsh::option::setNumber("Mesh.SaveAll", 1);
            gmsh::write(mshCachePath);
        }

        std::vector<std::size_t> nodeTags;
        std::vector<double> coords, params;
        gmsh::model::mesh::getNodes(nodeTags, coords, params);
        if (nodeTags.empty()) return false;

        std::size_t maxTag = 0;
        for (std::size_t t : nodeTags) {
            if (t > maxTag) maxTag = t;
        }

        std::vector<uint32_t> tagToDenseIdx(maxTag + 1, 0);
        {
            std::ofstream nodeOut(nodesPath, std::ios::binary | std::ios::trunc);
            if (!nodeOut) return false;

            std::vector<CachedNode> chunk;
            chunk.reserve(65536);

            for (size_t i = 0; i < nodeTags.size(); ++i) {
                tagToDenseIdx[nodeTags[i]] = static_cast<uint32_t>(i);
                chunk.push_back({
                    static_cast<float>(coords[3*i]),
                    static_cast<float>(coords[3*i+1]),
                    static_cast<float>(coords[3*i+2])
                });
                if (chunk.size() == 65536) {
                    nodeOut.write(reinterpret_cast<const char*>(chunk.data()), chunk.size() * sizeof(CachedNode));
                    chunk.clear();
                }
            }
            if (!chunk.empty()) {
                nodeOut.write(reinterpret_cast<const char*>(chunk.data()), chunk.size() * sizeof(CachedNode));
            }
            numCachedNodes = nodeTags.size();
        }

        nodeTags.clear(); nodeTags.shrink_to_fit();
        coords.clear();   coords.shrink_to_fit();
        params.clear();   params.shrink_to_fit();

        std::vector<int> eTypes3D;
        std::vector<std::vector<std::size_t>> eTags3D, eNodes3D;
        gmsh::model::mesh::getElements(eTypes3D, eTags3D, eNodes3D, 3);

        {
            std::ofstream tetOut(tetsPath, std::ios::binary | std::ios::trunc);
            if (!tetOut) return false;

            std::vector<CachedTet> tetChunk;
            tetChunk.reserve(65536);

            for (size_t t = 0; t < eTypes3D.size(); ++t) {
                // Element Type 4 = 4-node Linear Tet4 (FVM/FEM)
                // Element Type 11 = 10-node Quadratic Tet10 (FEM High-Order) -> first 4 nodes are the 4 corner vertices!
                int nodesPerElem = (eTypes3D[t] == 4) ? 4 : ((eTypes3D[t] == 11) ? 10 : 0);
                if (nodesPerElem > 0) {
                    const auto& ns = eNodes3D[t];
                    for (size_t j = 0; j < ns.size(); j += nodesPerElem) {
                        tetChunk.push_back({
                            tagToDenseIdx[ns[j]],
                            tagToDenseIdx[ns[j+1]],
                            tagToDenseIdx[ns[j+2]],
                            tagToDenseIdx[ns[j+3]]
                        });
                        numCachedTets++;
                        if (tetChunk.size() == 65536) {
                            tetOut.write(reinterpret_cast<const char*>(tetChunk.data()), tetChunk.size() * sizeof(CachedTet));
                            tetChunk.clear();
                        }
                    }
                }
            }
            if (!tetChunk.empty()) {
                tetOut.write(reinterpret_cast<const char*>(tetChunk.data()), tetChunk.size() * sizeof(CachedTet));
            }
        }

        eTypes3D.clear(); eTypes3D.shrink_to_fit();
        eTags3D.clear();  eTags3D.shrink_to_fit();
        eNodes3D.clear(); eNodes3D.shrink_to_fit();
        tagToDenseIdx.clear(); tagToDenseIdx.shrink_to_fit();

        gmsh::vectorpair volumes;
        gmsh::model::getEntities(volumes, 3);
        if (!volumes.empty()) {
            gmsh::model::mesh::clear(volumes);
        }

        releaseFreeRamToMacOS();

        cachedDiskMB = getFileSizeMB(nodesPath) + getFileSizeMB(tetsPath) + getFileSizeMB(mshCachePath);
        hasDiskCache = (numCachedTets > 0);
        return hasDiskCache;
    }

    vtkSmartPointer<vtkUnstructuredGrid> extractSectionCutFromDisk(vtkPlane* plane) {
        auto sliceGrid = vtkSmartPointer<vtkUnstructuredGrid>::New();
        if (!hasDiskCache || numCachedNodes == 0 || numCachedTets == 0 || !plane) return sliceGrid;

        int fdNodes = open(nodesPath.c_str(), O_RDONLY);
        int fdTets  = open(tetsPath.c_str(), O_RDONLY);
        if (fdNodes < 0 || fdTets < 0) {
            if (fdNodes >= 0) close(fdNodes);
            if (fdTets >= 0) close(fdTets);
            return sliceGrid;
        }

        size_t nodesBytes = numCachedNodes * sizeof(CachedNode);
        size_t tetsBytes  = numCachedTets  * sizeof(CachedTet);

        const CachedNode* mmapNodes = reinterpret_cast<const CachedNode*>(
            mmap(nullptr, nodesBytes, PROT_READ, MAP_SHARED, fdNodes, 0));
        const CachedTet* mmapTets = reinterpret_cast<const CachedTet*>(
            mmap(nullptr, tetsBytes, PROT_READ, MAP_SHARED, fdTets, 0));

        if (mmapNodes == MAP_FAILED || mmapTets == MAP_FAILED) {
            if (mmapNodes != MAP_FAILED) munmap((void*)mmapNodes, nodesBytes);
            if (mmapTets != MAP_FAILED)  munmap((void*)mmapTets, tetsBytes);
            close(fdNodes); close(fdTets);
            return sliceGrid;
        }

        madvise((void*)mmapTets, tetsBytes, MADV_SEQUENTIAL);

        double origin[3], normal[3];
        plane->GetOrigin(origin);
        plane->GetNormal(normal);

        vtkNew<vtkPoints> slicePts;
        std::unordered_map<uint32_t, vtkIdType> localPtMap;

        for (size_t i = 0; i < numCachedTets; ++i) {
            const CachedTet& tet = mmapTets[i];
            int posCount = 0, negCount = 0;

            for (int k = 0; k < 4; ++k) {
                const CachedNode& p = mmapNodes[tet.n[k]];
                double dist = (p.x - origin[0]) * normal[0] +
                              (p.y - origin[1]) * normal[1] +
                              (p.z - origin[2]) * normal[2];
                if (dist >= 0.0) posCount++;
                if (dist <= 0.0) negCount++;
            }

            if (posCount > 0 && negCount > 0) {
                vtkIdType cellPts[4];
                for (int k = 0; k < 4; ++k) {
                    uint32_t globalIdx = tet.n[k];
                    auto it = localPtMap.find(globalIdx);
                    if (it != localPtMap.end()) {
                        cellPts[k] = it->second;
                    } else {
                        const CachedNode& p = mmapNodes[globalIdx];
                        vtkIdType newId = slicePts->InsertNextPoint(p.x, p.y, p.z);
                        localPtMap[globalIdx] = newId;
                        cellPts[k] = newId;
                    }
                }
                sliceGrid->InsertNextCell(VTK_TETRA, 4, cellPts);
            }
        }

        sliceGrid->SetPoints(slicePts);

        madvise((void*)mmapTets, tetsBytes, MADV_FREE_REUSABLE);
        munmap((void*)mmapNodes, nodesBytes);
        munmap((void*)mmapTets, tetsBytes);
        close(fdNodes);
        close(fdTets);

        return sliceGrid;
    }

    void clearDiskCache() {
        unlink(nodesPath.c_str());
        unlink(tetsPath.c_str());
        unlink(mshCachePath.c_str());
        hasDiskCache = false;
        numCachedNodes = 0;
        numCachedTets = 0;
        cachedDiskMB = 0.0;
        releaseFreeRamToMacOS();
    }
};

// ============================================================================
// DATA STRUCTURES & CUSTOM INTERACTOR
// ============================================================================
struct RefinementStroke {
    double x, y, z;
    double radius;
    double targetSize;
    std::vector<vtkIdType> paintedCells;
};

struct BoxWallActorData {
    int surfaceTag = -1;
    std::string roleKey; // "-X", "+X", "-Y", "+Y", "-Z", "+Z"
    double center[3] = {0, 0, 0};
    double outwardNormal[3] = {0, 0, 1};
    vtkSmartPointer<vtkPolyData> poly;
    vtkSmartPointer<vtkPolyDataMapper> mapper;
    vtkSmartPointer<vtkActor> actor;
};

struct AnalysisProfile {
    QString discretizationMethod = "FVM"; // "FVM", "FEM", or "FVM+FEM"
    int defaultElementOrder = 1;          // 1 = Linear (Tet4/Tri3), 2 = Quadratic (Tet10/Tri6)
    QString flowType = "External";
    QString machRegime = "Sub 0.3 Mach";
    double defaultMinSize = 0.00;
    double defaultMaxSize = 1.00;
    int cpuThreads = std::max(1u, std::thread::hardware_concurrency());
    bool gpuAccel = true;
    std::string gpuVendorMode = "auto";
    int ramLimitGb = 8;
};

class MesherInteractorStyle : public vtkInteractorStyleTrackballCamera {
public:
    static MesherInteractorStyle* New() { return new MesherInteractorStyle; }
    vtkTypeMacro(MesherInteractorStyle, vtkInteractorStyleTrackballCamera);

    bool paintMode = false;
    bool isPainting = false;
    bool* isBusyPtr = nullptr;

    std::function<void(vtkActor*, vtkIdType, bool)> onCellPicked;
    std::function<void(double, double, double)> onPaintPoint;
    std::function<void(double, double, double, bool)> onBrushHover;
    std::function<void(double)> onBrushScale;
    std::function<void(double)> onFineSizeScale;
    std::function<void()> onUndoLastPaint;
    std::function<void()> onCameraOrbit;

    bool isLocked() const { return (isBusyPtr && *isBusyPtr); }

    void OnLeftButtonDown() override {
        if (isLocked() || !this->GetDefaultRenderer()) return;
        int* clickPos = this->GetInteractor()->GetEventPosition();
        vtkNew<vtkCellPicker> picker;
        picker->SetTolerance(0.0025);
        picker->Pick(clickPos[0], clickPos[1], 0, this->GetDefaultRenderer());

        vtkActor* pickedActor = picker->GetActor();
        vtkIdType cellId = picker->GetCellId();

        if (paintMode) {
            if (pickedActor && cellId >= 0 && onPaintPoint) {
                isPainting = true;
                double* pos = picker->GetPickPosition();
                if (onBrushHover) onBrushHover(pos[0], pos[1], pos[2], true);
                onPaintPoint(pos[0], pos[1], pos[2]);
                return;
            }
        } else {
            if (pickedActor && cellId >= 0 && onCellPicked) {
                bool shiftHeld = this->GetInteractor()->GetShiftKey();
                onCellPicked(pickedActor, cellId, shiftHeld);
            }
        }
        vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
    }

    void OnMouseMove() override {
        if (isLocked()) return;
        if (paintMode && this->GetDefaultRenderer()) {
            int* clickPos = this->GetInteractor()->GetEventPosition();
            vtkNew<vtkCellPicker> picker;
            picker->SetTolerance(0.0025);
            picker->Pick(clickPos[0], clickPos[1], 0, this->GetDefaultRenderer());

            if (picker->GetActor() && picker->GetCellId() >= 0) {
                double* pos = picker->GetPickPosition();
                if (onBrushHover) onBrushHover(pos[0], pos[1], pos[2], true);
                if (isPainting && onPaintPoint) {
                    onPaintPoint(pos[0], pos[1], pos[2]);
                    return;
                }
            } else {
                if (onBrushHover) onBrushHover(0, 0, 0, false);
            }
            if (isPainting) return;
        }
        vtkInteractorStyleTrackballCamera::OnMouseMove();
        if (onCameraOrbit) onCameraOrbit();
    }

    void OnLeftButtonUp() override {
        isPainting = false;
        if (isLocked()) return;
        vtkInteractorStyleTrackballCamera::OnLeftButtonUp();
        if (onCameraOrbit) onCameraOrbit();
    }

    void OnMouseWheelForward() override {
        if (isLocked()) return;
        if (paintMode && (isPainting || this->GetInteractor()->GetShiftKey())) {
            if (onBrushScale) onBrushScale(1.12);
            return;
        }
        vtkInteractorStyleTrackballCamera::OnMouseWheelForward();
        if (onCameraOrbit) onCameraOrbit();
    }

    void OnMouseWheelBackward() override {
        if (isLocked()) return;
        if (paintMode && (isPainting || this->GetInteractor()->GetShiftKey())) {
            if (onBrushScale) onBrushScale(0.89);
            return;
        }
        vtkInteractorStyleTrackballCamera::OnMouseWheelBackward();
        if (onCameraOrbit) onCameraOrbit();
    }

    void OnKeyPress() override {
        if (isLocked()) return;
        std::string key = this->GetInteractor()->GetKeySym() ? this->GetInteractor()->GetKeySym() : "";
        if (paintMode) {
            if (key == "bracketleft" || key == "minus") {
                if (onBrushScale) onBrushScale(0.85);
                return;
            } else if (key == "bracketright" || key == "equal" || key == "plus") {
                if (onBrushScale) onBrushScale(1.18);
                return;
            } else if (key == "braceleft") {
                if (onFineSizeScale) onFineSizeScale(0.85);
                return;
            } else if (key == "braceright") {
                if (onFineSizeScale) onFineSizeScale(1.18);
                return;
            } else if (key == "BackSpace" || key == "z" || key == "Z") {
                if (onUndoLastPaint) onUndoLastPaint();
                return;
            }
        }
        vtkInteractorStyleTrackballCamera::OnKeyPress();
    }
};

// ============================================================================
// PRE-FLIGHT LAUNCHER DIALOG (WITH FVM vs. FEM DISCRETIZATION SELECTOR)
// ============================================================================
class SciFiLauncherDialog : public QDialog {
public:
    AnalysisProfile profile;

    SciFiLauncherDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("SIMUOP LABS — INITIALIZATION SEQUENCE");
        resize(650, 640);
        setModal(true);

        setStyleSheet(R"(
            QDialog { background-color: #0b0f17; border: 1px solid #10b981; }
            QLabel { color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text"; font-size: 12px; }
            QLabel#TitleLabel { font-weight: bold; font-size: 15px; color: #10b981; letter-spacing: 2px; }
            QLabel#SubtitleLabel { color: #94a3b8; font-size: 11px; }
            QGroupBox {
                border: 1px solid #1e293b;
                border-radius: 0px;
                margin-top: 8px;
                font-weight: bold;
                color: #10b981;
                font-size: 11px;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }
            QRadioButton, QCheckBox { color: #f8fafc; font-size: 11px; spacing: 8px; }
            QRadioButton::indicator { width: 13px; height: 13px; border: 1px solid #10b981; background: #0f172a; border-radius: 0px; }
            QRadioButton::indicator:checked { background: #f97316; border-color: #fb923c; }
            QComboBox, QSpinBox {
                background-color: #0f172a;
                color: #f8fafc;
                border: 1px solid #1e293b;
                border-radius: 0px;
                padding: 4px;
            }
            QPushButton#LaunchBtn {
                background-color: #10b981; color: #0b0f17; border: 1px solid #34d399;
                border-radius: 0px; padding: 10px; font-weight: bold; font-size: 12px; letter-spacing: 1px;
            }
            QPushButton#LaunchBtn:hover { background-color: #f97316; color: #0b0f17; border-color: #fb923c; }
        )");

        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(20, 18, 20, 18);
        mainLayout->setSpacing(8);

        QLabel* title = new QLabel("SIMUOP LABS // FVM & FEM MULTIPHYSICS STUDIO");
        title->setObjectName("TitleLabel");
        QLabel* sub = new QLabel("SELECT NUMERICAL DISCRETIZATION (FVM / FEM), REGIME & HARDWARE");
        sub->setObjectName("SubtitleLabel");
        mainLayout->addWidget(title);
        mainLayout->addWidget(sub);

        // Group 0: Numerical Discretization Method (FVM vs FEM)
        QGroupBox* methodBox = new QGroupBox("01 // NUMERICAL DISCRETIZATION ENGINE (FVM vs. FEM)");
        QVBoxLayout* methodLayout = new QVBoxLayout(methodBox);
        rbFVM = new QRadioButton("Finite Volume Method (FVM) — Cell-Centered CFD (BaramFlow, OpenFOAM, Fluent, SU2) [1st-Order Tet4/Tri3]");
        rbFEM = new QRadioButton("Finite Element Method (FEM) — Structural / Thermal / FEM-CFD (CalculiX, Abaqus, Elmer, FEniCS) [2nd-Order Tet10/Tri6]");
        rbFSI = new QRadioButton("Coupled FVM + FEM (Fluid-Structure Interaction / Aeroelasticity — Dual Export)");
        rbFVM->setChecked(true);
        methodLayout->addWidget(rbFVM);
        methodLayout->addWidget(rbFEM);
        methodLayout->addWidget(rbFSI);
        mainLayout->addWidget(methodBox);

        // Group 1: Domain
        QGroupBox* domainBox = new QGroupBox("02 // TOPOLOGY DOMAIN");
        QVBoxLayout* domainLayout = new QVBoxLayout(domainBox);
        rbInternal = new QRadioButton("Internal Domain / Solid Component (Pipes, Channels, Structural Parts)");
        rbExternal = new QRadioButton("External Farfield Enclosure (Aerodynamics, Turning Vehicles, Wind Tunnel Box)");
        rbExternal->setChecked(true);
        domainLayout->addWidget(rbInternal);
        domainLayout->addWidget(rbExternal);
        mainLayout->addWidget(domainBox);

        // Group 2: Regime
        QGroupBox* machBox = new QGroupBox("03 // PHYSICS / MACH REGIME");
        QVBoxLayout* machLayout = new QVBoxLayout(machBox);
        rbMachLow = new QRadioButton("Sub 0.3 Mach / Static Structural / Thermal (Incompressible / Low-Speed)");
        rbMachSub = new QRadioButton("0.3 - 1.0 Mach (Subsonic / Transonic / High-Load FSI)");
        rbMachSup = new QRadioButton("1.0 - 3.0 Mach (Supersonic)");
        rbMachHyp1 = new QRadioButton("3.0 - 5.0 Mach (High Supersonic)");
        rbMachHyp2 = new QRadioButton("7.0+ Mach (Hypersonic / Aerothermal)");
        rbMachLow->setChecked(true);
        machLayout->addWidget(rbMachLow);
        machLayout->addWidget(rbMachSub);
        machLayout->addWidget(rbMachSup);
        machLayout->addWidget(rbMachHyp1);
        machLayout->addWidget(rbMachHyp2);
        mainLayout->addWidget(machBox);

        // Group 3: Hardware
        QGroupBox* hwBox = new QGroupBox("04 // HARDWARE & OUT-OF-CORE RAM ALLOCATION");
        QVBoxLayout* hwLayout = new QVBoxLayout(hwBox);

        QHBoxLayout* threadRow = new QHBoxLayout();
        threadRow->addWidget(new QLabel("CPU Compute Threads:"));
        comboThreads = new QComboBox();
        unsigned int maxCores = std::max(1u, std::thread::hardware_concurrency());
        comboThreads->addItem(QString("All Available Cores (%1 Threads)").arg(maxCores), maxCores);
        comboThreads->addItem(QString("Performance Cores Only (%1 Threads)").arg(std::max(1u, maxCores / 2)), std::max(1u, maxCores / 2));
        comboThreads->addItem("Single Core (Safe Mode)", 1);
        threadRow->addWidget(comboThreads);
        hwLayout->addLayout(threadRow);

        QHBoxLayout* cpuDetectedRow = new QHBoxLayout();
        cpuDetectedRow->addWidget(new QLabel("Detected Host CPU:"));
        QLabel* cpuBrandLbl = new QLabel(QString::fromStdString(SimuopOS::getCpuArchitectureAndBrand()));
        cpuBrandLbl->setStyleSheet("color: #34d399; font-weight: bold;");
        cpuDetectedRow->addWidget(cpuBrandLbl);
        hwLayout->addLayout(cpuDetectedRow);

        QHBoxLayout* gpuRow = new QHBoxLayout();
        gpuRow->addWidget(new QLabel("GPU Hardware Pipeline:"));
        comboGpuVendor = new QComboBox();
        comboGpuVendor->addItem("Auto-Detect Hardware GPU (NVIDIA / AMD / Intel / Apple)", "auto");
        comboGpuVendor->addItem("NVIDIA GeForce / RTX Discrete GPU (Optimus / PRIME)", "nvidia");
        comboGpuVendor->addItem("AMD Radeon / Instinct Discrete GPU (SmartShift / DRI_PRIME)", "amd");
        comboGpuVendor->addItem("Intel Arc / Iris Xe / UHD Integrated GPU", "intel");
        comboGpuVendor->addItem("CPU Software Rendering Only (No GPU Required)", "software_cpu");
        gpuRow->addWidget(comboGpuVendor);
        hwLayout->addLayout(gpuRow);

        QHBoxLayout* ramRow = new QHBoxLayout();
        ramRow->addWidget(new QLabel("RAM Safety Ceiling (Auto-Flush to SSD):"));
        spinRamLimit = new QSpinBox();
        spinRamLimit->setRange(2, 128);
        spinRamLimit->setValue(8);
        spinRamLimit->setSuffix(" GB");
        ramRow->addWidget(spinRamLimit);
        hwLayout->addLayout(ramRow);
        mainLayout->addWidget(hwBox);

        QPushButton* launchBtn = new QPushButton("INITIALIZE FVM / FEM STUDIO [ ENTER ]");
        launchBtn->setObjectName("LaunchBtn");
        connect(launchBtn, &QPushButton::clicked, this, &SciFiLauncherDialog::onLaunchClicked);
        mainLayout->addWidget(launchBtn);
    }

private:
    QRadioButton* rbFVM;
    QRadioButton* rbFEM;
    QRadioButton* rbFSI;
    QRadioButton* rbInternal;
    QRadioButton* rbExternal;
    QRadioButton* rbMachLow;
    QRadioButton* rbMachSub;
    QRadioButton* rbMachSup;
    QRadioButton* rbMachHyp1;
    QRadioButton* rbMachHyp2;
    QComboBox* comboThreads;
    QComboBox* comboGpuVendor;
    QSpinBox* spinRamLimit;

    void onLaunchClicked() {
        if (rbFVM->isChecked()) {
            profile.discretizationMethod = "FVM";
            profile.defaultElementOrder = 1; // FVM requires 1st-order linear elements
        } else if (rbFEM->isChecked()) {
            profile.discretizationMethod = "FEM";
            profile.defaultElementOrder = 2; // FEM defaults to 2nd-order quadratic Tet10/Tri6
        } else {
            profile.discretizationMethod = "FVM+FEM";
            profile.defaultElementOrder = 1;
        }

        profile.flowType = rbInternal->isChecked() ? "Internal" : "External";
        profile.defaultMinSize = 0.0;
        if (rbMachLow->isChecked()) { profile.machRegime = "Sub 0.3 Mach"; profile.defaultMaxSize = 1.0; }
        else if (rbMachSub->isChecked()) { profile.machRegime = "0.3 - 1.0 Mach"; profile.defaultMaxSize = 0.8; }
        else if (rbMachSup->isChecked()) { profile.machRegime = "1.0 - 3.0 Mach"; profile.defaultMaxSize = 0.5; }
        else if (rbMachHyp1->isChecked()) { profile.machRegime = "3.0 - 5.0 Mach"; profile.defaultMaxSize = 0.3; }
        else { profile.machRegime = "7.0+ Mach"; profile.defaultMaxSize = 0.15; }

        profile.cpuThreads = comboThreads->currentData().toInt();
        profile.gpuVendorMode = comboGpuVendor->currentData().toString().toStdString();
        profile.gpuAccel = (profile.gpuVendorMode != "software_cpu");
        SimuopOS::applyGpuVendorPreference(profile.gpuVendorMode);
        profile.ramLimitGb = spinRamLimit->value();
        accept();
    }
};

// ============================================================================
// MAIN STUDIO WINDOW (DUAL FVM & FEM ENGINE)
// ============================================================================
class DiscoveryWindow : public QMainWindow {
public:
    AnalysisProfile activeProfile;
    OutOfCoreMeshCache diskCache;

    DiscoveryWindow(const AnalysisProfile& prof) : activeProfile(prof) {
        updateWindowTitleBanner();

        resize(1260, 780);
        setMinimumSize(980, 640);

        setStyleSheet(R"(
            QMainWindow { background-color: #0b0f17; }
            QFrame#HudPanel {
                background-color: rgba(13, 19, 31, 242);
                border: 1px solid #1e293b;
                border-radius: 0px;
            }
            QTabWidget::pane {
                border: 1px solid #10b981;
                background-color: rgba(13, 19, 31, 246);
                border-radius: 0px;
            }
            QTabBar::tab {
                background: #0f172a;
                color: #94a3b8;
                padding: 6px 12px;
                border: 1px solid #1e293b;
                border-bottom: none;
                font-weight: bold;
                font-size: 10px;
                letter-spacing: 0.8px;
            }
            QTabBar::tab:hover { background: #1e293b; color: #f8fafc; }
            QTabBar::tab:selected { background: #10b981; color: #0b0f17; border-color: #10b981; }
            QLabel { color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text"; font-size: 11px; }
            QLabel#HudTitle { font-weight: bold; font-size: 11px; color: #10b981; letter-spacing: 0.8px; }
            QLabel#OrangeBadge { font-weight: bold; font-size: 11px; color: #f97316; letter-spacing: 0.8px; }
            QLabel#CommentLabel { color: #94a3b8; font-size: 10px; font-style: italic; }
            QPushButton {
                background-color: #1e293b;
                color: #f8fafc;
                border: 1px solid #334155;
                border-radius: 0px;
                padding: 4px 8px;
                font-weight: 600;
                font-size: 10px;
            }
            QPushButton:hover { background-color: #10b981; color: #0b0f17; border-color: #34d399; }
            QPushButton:checked { background-color: #f97316; color: #0b0f17; border-color: #fb923c; }
            QPushButton#PrimaryBtn { background-color: #10b981; color: #0b0f17; border: 1px solid #34d399; font-weight: bold; }
            QPushButton#PrimaryBtn:hover { background-color: #34d399; }
            QPushButton#OrangeBtn { background-color: #f97316; color: #0b0f17; border: 1px solid #fb923c; font-weight: bold; }
            QPushButton#OrangeBtn:hover { background-color: #fb923c; }
            QPushButton#DangerBtn { background-color: #7f1d1d; border: 1px solid #dc2626; color: #fca5a5; font-weight: bold; }
            QPushButton#DangerBtn:hover { background-color: #dc2626; color: #ffffff; }
            QListWidget, QLineEdit, QDoubleSpinBox, QSpinBox, QComboBox, QPlainTextEdit {
                background-color: rgba(11, 15, 23, 245);
                color: #f8fafc;
                border: 1px solid #1e293b;
                border-radius: 0px;
                padding: 3px;
                font-size: 10px;
            }
            QPlainTextEdit { font-family: Menlo, Monaco, Courier, monospace; font-size: 10px; color: #34d399; }
            QProgressBar {
                border: 1px solid #1e293b;
                border-radius: 0px;
                background-color: #0f172a;
                text-align: center;
                color: #ffffff;
                font-weight: bold;
                font-size: 10px;
                min-height: 16px;
            }
            QProgressBar::chunk {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #10b981, stop:1 #f97316);
                border-radius: 0px;
            }
            QCheckBox { color: #f8fafc; font-size: 10px; }
        )");

        vtkWidget = new QVTKOpenGLNativeWidget(this);
        setCentralWidget(vtkWidget);

        renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        vtkWidget->setRenderWindow(renderWindow);

        renderer = vtkSmartPointer<vtkRenderer>::New();
        renderer->SetBackground(0.04, 0.06, 0.09);
        renderer->SetBackground2(0.08, 0.12, 0.18);
        renderer->GradientBackgroundOn();
        renderWindow->AddRenderer(renderer);

        clipPlane = vtkSmartPointer<vtkPlane>::New();
        clipPlane->SetOrigin(0.0, 0.0, 0.0);
        clipPlane->SetNormal(0.0, 0.0, -1.0);

        interactorStyle = vtkSmartPointer<MesherInteractorStyle>::New();
        interactorStyle->isBusyPtr = &isBusy;
        interactorStyle->SetDefaultRenderer(renderer);
        interactorStyle->onCellPicked = [this](vtkActor* actor, vtkIdType cellId, bool multiSelect) {
            handleCellPicked(actor, cellId, multiSelect);
        };
        interactorStyle->onPaintPoint = [this](double x, double y, double z) {
            paintOnBodySurface(x, y, z);
        };
        interactorStyle->onBrushHover = [this](double x, double y, double z, bool visible) {
            updateSurfaceBrushHover(x, y, z, visible);
        };
        interactorStyle->onBrushScale = [this](double factor) {
            scaleBrushRadiusLive(factor);
        };
        interactorStyle->onFineSizeScale = [this](double factor) {
            double newFine = std::clamp(brushSizeSpin->value() * factor, brushSizeSpin->minimum(), brushSizeSpin->maximum());
            brushSizeSpin->setValue(newFine);
        };
        interactorStyle->onUndoLastPaint = [this]() {
            undoLastPaintStroke();
        };
        interactorStyle->onCameraOrbit = [this]() {
            updateAutoOpenBoxWalls();
        };
        renderWindow->GetInteractor()->SetInteractorStyle(interactorStyle);

        setupCoordinateAxesWidget();
        setupDebugDialog();
        setupTopTabRibbonUI();

        gmsh::initialize();
        configureGmshHardware();

        ramTimer = new QTimer(this);
        connect(ramTimer, &QTimer::timeout, this, &DiscoveryWindow::updateRamTelemetryUI);
        ramTimer->start(1000);
        updateRamTelemetryUI();

        appendDebugLog(QString("Simuop Labs Studio Initialized in [%1] Mode (Default Order = %2).")
            .arg(activeProfile.discretizationMethod).arg(activeProfile.defaultElementOrder));
    }

    ~DiscoveryWindow() override {
        diskCache.clearDiskCache();
        gmsh::finalize();
    }

private:
    bool isBusy = false;
    struct ScopedBusyLock {
        bool& flag;
        ScopedBusyLock(bool& f) : flag(f) { flag = true; }
        ~ScopedBusyLock() { flag = false; }
    };

    QTimer* ramTimer = nullptr;

    QVTKOpenGLNativeWidget* vtkWidget;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<MesherInteractorStyle> interactorStyle;
    vtkSmartPointer<vtkPlane> clipPlane;

    vtkSmartPointer<vtkAxesActor> axesActor;
    vtkSmartPointer<vtkOrientationMarkerWidget> axesWidget;

    vtkSmartPointer<vtkPolyData> innerModelPoly;
    vtkSmartPointer<vtkPolyDataMapper> innerModelMapper;
    vtkSmartPointer<vtkActor> innerModelActor;
    std::vector<int> innerCellToTag;
    std::vector<double> innerCellCentroids;
    std::unordered_map<vtkIdType, double> paintedInnerCells;
    std::unordered_set<vtkIdType> hoverInnerCells;
    std::vector<RefinementStroke> refinementStrokes;

    std::map<int, BoxWallActorData> boxWallActors;

    vtkSmartPointer<vtkActor> cadFeatureEdgeActor;
    vtkSmartPointer<vtkCubeSource> enclosurePreviewSource;
    vtkSmartPointer<vtkActor> enclosurePreviewActor;
    QSlider* brushThicknessSlider;

    vtkSmartPointer<vtkDataSetMapper> volumeClipMapper;
    vtkSmartPointer<vtkActor> volumeClipActor;

    bool isMeshMode = false;
    bool hasLoadedModel = false;
    bool hasEnclosureBox = false;
    int lastGeneratedOrder = 1;

    std::map<int, std::string> tagToPatchName;
    std::set<int> outerBoxSurfaceTags;
    std::map<std::string, int> boxWallRoleToTag;
    std::set<int> selectedTags;

    std::vector<int> detectedFreeCurveTags;
    std::vector<int> detectedSliverSurfaceTags;

    QTabWidget* topRibbonTabs;
    QComboBox* methodCombo;
    QComboBox* solverCombo;
    QComboBox* elemOrderCombo;
    QCheckBox* highOrderCurveOptCheck;
    QCheckBox* autoOpenBoxCheck;
    QCheckBox* autoOffloadSsdCheck;
    QSlider* boxOpacitySlider;

    QListWidget* problemListWidget;
    QListWidget* faceListWidget;
    QLineEdit* patchNameInput;

    QProgressBar* progressBar;
    QLabel* statusLabel;
    QLabel* commentLabel;
    QLabel* ramTelemetryLabel;
    QDialog* debugDialog;
    QPlainTextEdit* debugConsole;

    QDoubleSpinBox* healTolSpin;
    QDoubleSpinBox* sliverRatioSpin;

    QCheckBox* previewBoxCheck;
    QDoubleSpinBox* padNegX; QDoubleSpinBox* padPosX;
    QDoubleSpinBox* padNegY; QDoubleSpinBox* padPosY;
    QDoubleSpinBox* padNegZ; QDoubleSpinBox* padPosZ;

    QComboBox* wallFrontNegXCombo;
    QComboBox* wallRearPosXCombo;
    QComboBox* wallLeftNegYCombo;
    QComboBox* wallRightPosYCombo;
    QComboBox* wallFloorNegZCombo;
    QComboBox* wallRoofPosZCombo;

    QDoubleSpinBox* modelYawSpin;
    QDoubleSpinBox* modelPitchSpin;
    QDoubleSpinBox* modelRollSpin;
    QDoubleSpinBox* flowSpeedSpin;
    QDoubleSpinBox* flowYawAngleSpin;
    QDoubleSpinBox* turningRadiusSpin;

    QComboBox* algo2DCombo;
    QComboBox* algo3DCombo;
    QDoubleSpinBox* minSizeSpin;
    QDoubleSpinBox* maxSizeSpin;
    QDoubleSpinBox* curvatureSpin;
    QSpinBox* smoothingSpin;
    QCheckBox* optimizeCheck;

    QCheckBox* enableBlCheck;
    QSpinBox* blLayersSpin;
    QDoubleSpinBox* blFirstHeightSpin;
    QDoubleSpinBox* blRatioSpin;

    QPushButton* selectModeBtn;
    QPushButton* paintModeBtn;
    QDoubleSpinBox* brushRadiusSpin;
    QDoubleSpinBox* brushSizeSpin;

    QCheckBox* clipEnableCheck;
    QCheckBox* show3DTetsCheck;
    QComboBox* clipAxisCombo;
    QCheckBox* clipFlipCheck;
    QSlider* clipSlider;
    QLabel* qualitySummaryLabel;

    double modelBounds[6] = {-1, 1, -1, 1, -1, 1};
    double carOnlyBounds[6] = {-1, 1, -1, 1, -1, 1};
    bool showWireframe = false;

    void updateWindowTitleBanner() {
        setWindowTitle(QString("SIMUOP LABS // FVM & FEM STUDIO [%1 ENGINE | %2 | %3 THREADS | %4 GB RAM CAP]")
            .arg(activeProfile.discretizationMethod).arg(activeProfile.machRegime)
            .arg(activeProfile.cpuThreads).arg(activeProfile.ramLimitGb));
    }

    void setupCoordinateAxesWidget() {
        axesActor = vtkSmartPointer<vtkAxesActor>::New();
        axesActor->SetShaftTypeToCylinder();
        axesActor->SetCylinderRadius(0.05);
        axesActor->SetConeRadius(0.45);
        axesActor->SetTotalLength(1.0, 1.0, 1.0);

        axesActor->GetXAxisCaptionActor2D()->GetCaptionTextProperty()->SetColor(1.0, 0.35, 0.35);
        axesActor->GetYAxisCaptionActor2D()->GetCaptionTextProperty()->SetColor(0.20, 0.92, 0.60);
        axesActor->GetZAxisCaptionActor2D()->GetCaptionTextProperty()->SetColor(0.30, 0.70, 1.0);

        axesWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
        axesWidget->SetOrientationMarker(axesActor);
        axesWidget->SetInteractor(renderWindow->GetInteractor());
        axesWidget->SetViewport(0.60, 0.08, 0.74, 0.25);
        axesWidget->SetEnabled(1);
        axesWidget->InteractiveOff();
    }

    void setupDebugDialog() {
        debugDialog = new QDialog(this);
        debugDialog->setWindowTitle("SIMUOP LABS — KERNEL DEBUG & TELEMETRY REPORT");
        debugDialog->resize(720, 480);
        debugDialog->setStyleSheet(this->styleSheet());

        QVBoxLayout* dlgLayout = new QVBoxLayout(debugDialog);
        QLabel* title = new QLabel("KERNEL TELEMETRY STREAM (OPENCASCADE // GMSH FVM & FEM // OUT-OF-CORE SSD CACHE)");
        title->setObjectName("HudTitle");
        dlgLayout->addWidget(title);

        debugConsole = new QPlainTextEdit();
        debugConsole->setReadOnly(true);
        dlgLayout->addWidget(debugConsole);

        QHBoxLayout* btnRow = new QHBoxLayout();
        QPushButton* reportBtn = new QPushButton("GENERATE FULL DIAGNOSTIC REPORT");
        reportBtn->setObjectName("PrimaryBtn");
        connect(reportBtn, &QPushButton::clicked, this, &DiscoveryWindow::generateDiagnosticReport);

        QPushButton* clearBtn = new QPushButton("CLEAR");
        connect(clearBtn, &QPushButton::clicked, [this]() { debugConsole->clear(); });

        btnRow->addWidget(reportBtn);
        btnRow->addStretch();
        btnRow->addWidget(clearBtn);
        dlgLayout->addLayout(btnRow);
    }

    void updateRamTelemetryUI() {
        double ramMB = OutOfCoreMeshCache::getCurrentRamUsageMB();
        double limitMB = activeProfile.ramLimitGb * 1024.0;

        QString ssdInfo = diskCache.hasDiskCache
            ? QString("SSD CACHE: %1 Tets (%2 MB)").arg(diskCache.numCachedTets).arg(diskCache.cachedDiskMB, 0, 'f', 1)
            : QString("SSD CACHE: 0 MB");

        ramTelemetryLabel->setText(QString("[%1 | P%2]  RAM: %3 / %4 MB  |  %5")
            .arg(activeProfile.discretizationMethod).arg(lastGeneratedOrder)
            .arg(ramMB, 0, 'f', 0).arg(limitMB, 0, 'f', 0).arg(ssdInfo));

        if (ramMB > limitMB * 0.85) {
            ramTelemetryLabel->setStyleSheet("font-weight: bold; color: #ef4444; font-size: 10px;");
            OutOfCoreMeshCache::releaseFreeRamToMacOS();
        } else {
            ramTelemetryLabel->setStyleSheet("font-weight: bold; color: #f97316; font-size: 10px;");
        }
    }

    void appendDebugLog(const QString& msg) {
        QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
        debugConsole->appendPlainText(QString("[%1] %2").arg(timestamp, msg));
        debugConsole->verticalScrollBar()->setValue(debugConsole->verticalScrollBar()->maximum());
    }

    void setProgressState(int percent, const QString& headline, const QString& detailComment) {
        progressBar->setValue(percent);
        statusLabel->setText(headline);
        commentLabel->setText(detailComment);
        appendDebugLog(headline + " — " + detailComment);
        updateRamTelemetryUI();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    void configureGmshHardware() {
        gmsh::option::setNumber("General.Terminal", 1);
        int threads = activeProfile.cpuThreads;
        gmsh::option::setNumber("General.NumThreads", threads);
        gmsh::option::setNumber("Geometry.OCCParallel", 1);
        gmsh::option::setNumber("Mesh.MaxNumThreads1D", threads);
        gmsh::option::setNumber("Mesh.MaxNumThreads2D", threads);
        gmsh::option::setNumber("Mesh.MaxNumThreads3D", threads);

        gmsh::option::setNumber("Geometry.OCCAutoFix", 0);
        gmsh::option::setNumber("Geometry.OCCFixSmallEdges", 0);
        gmsh::option::setNumber("Geometry.OCCFixSmallFaces", 0);
        gmsh::option::setNumber("Geometry.OCCSewFaces", 0);
    }

    void resetSceneAndKernel() {
        clearPaintedRefinement();
        diskCache.clearDiskCache();

        if (innerModelActor) { renderer->RemoveActor(innerModelActor); innerModelActor = nullptr; innerModelMapper = nullptr; innerModelPoly = nullptr; }
        for (auto& kv : boxWallActors) {
            if (kv.second.actor) renderer->RemoveActor(kv.second.actor);
        }
        boxWallActors.clear();

        if (cadFeatureEdgeActor) { renderer->RemoveActor(cadFeatureEdgeActor); cadFeatureEdgeActor = nullptr; }
        if (enclosurePreviewActor) { renderer->RemoveActor(enclosurePreviewActor); enclosurePreviewActor = nullptr; enclosurePreviewSource = nullptr; }
        if (volumeClipActor) { renderer->RemoveActor(volumeClipActor); volumeClipActor = nullptr; volumeClipMapper = nullptr; }

        innerCellToTag.clear();
        innerCellCentroids.clear();
        tagToPatchName.clear();
        outerBoxSurfaceTags.clear();
        boxWallRoleToTag.clear();
        selectedTags.clear();
        detectedFreeCurveTags.clear();
        detectedSliverSurfaceTags.clear();
        faceListWidget->clear();
        problemListWidget->clear();
        hasEnclosureBox = false;

        gmsh::clear();
        configureGmshHardware();
        OutOfCoreMeshCache::releaseFreeRamToMacOS();
        updateRamTelemetryUI();
        renderWindow->Render();
    }

    QComboBox* createBoundaryRoleCombo(const QString& defaultRole) {
        QComboBox* c = new QComboBox();
        c->addItems({
            "inlet", "outlet", "no_slip_wall", "slip_wall", "moving_ground", "symmetry", "farfield",
            "fixed_support", "pressure_load", "force_load", "fsi_interface", "thermal_wall"
        });
        c->setCurrentText(defaultRole);
        connect(c, &QComboBox::currentTextChanged, this, &DiscoveryWindow::apply6WallDropdownsToBox);
        return c;
    }

    void setupTopTabRibbonUI() {
        QVBoxLayout* mainOverlay = new QVBoxLayout(vtkWidget);
        mainOverlay->setContentsMargins(10, 10, 10, 10);
        mainOverlay->setSpacing(6);

        topRibbonTabs = new QTabWidget();
        topRibbonTabs->setFixedHeight(102);

        // ---------------- TAB 1: 01 // IMPORT, METHOD (FVM/FEM) & VIEW ----------------
        QWidget* tabImport = new QWidget();
        QGridLayout* t1 = new QGridLayout(tabImport);
        t1->setContentsMargins(10, 4, 10, 4);
        t1->setSpacing(6);

        QPushButton* openBtn = new QPushButton("1. IMPORT CAD (STEP / IGES / BREP)");
        openBtn->setObjectName("PrimaryBtn");
        connect(openBtn, &QPushButton::clicked, this, &DiscoveryWindow::onOpenCAD);
        t1->addWidget(openBtn, 0, 0);

        t1->addWidget(new QLabel("METHOD:"), 0, 1);
        methodCombo = new QComboBox();
        methodCombo->addItem("FVM — Finite Volume (CFD)", "FVM");
        methodCombo->addItem("FEM — Finite Element (FEA / FSI)", "FEM");
        methodCombo->addItem("Coupled FVM + FEM (Multiphysics)", "FVM+FEM");
        methodCombo->setCurrentIndex(activeProfile.discretizationMethod == "FEM" ? 1 : (activeProfile.discretizationMethod == "FVM+FEM" ? 2 : 0));
        connect(methodCombo, &QComboBox::currentIndexChanged, this, &DiscoveryWindow::onDiscretizationMethodChanged);
        t1->addWidget(methodCombo, 0, 2);

        t1->addWidget(new QLabel("SOLVER:"), 0, 3);
        solverCombo = new QComboBox();
        // Both FVM and FEM Solvers!
        solverCombo->addItem("[FVM] BARAM (BaramFlow — Gmsh v2.2 .msh)", "baram_flow");
        solverCombo->addItem("[FVM] BARAM (BaramMesh — Multi-Solid .stl)", "baram_mesh");
        solverCombo->addItem("[FVM] OpenFOAM Native (gmshToFoam .msh)", "openfoam");
        solverCombo->addItem("[FVM] Ansys Fluent / CFX (.unv / .msh)", "ansys_fvm");
        solverCombo->addItem("[FVM] SU2 Multiphysics (.su2)", "su2");
        solverCombo->addItem("[FEM] CalculiX / Abaqus FEA (.inp with NSET/ELSET)", "calculix_inp");
        solverCombo->addItem("[FEM] Elmer FEM / Ideas Universal (.unv)", "elmer_unv");
        solverCombo->addItem("[FEM] FEniCS / Firedrake / Deal.II (Gmsh v4.1 .msh)", "fenics_msh4");
        solverCombo->addItem("[FEM] Ansys Mechanical / ParaView (.vtk)", "fem_vtk");
        if (activeProfile.discretizationMethod == "FEM") solverCombo->setCurrentIndex(5);
        t1->addWidget(solverCombo, 0, 4);

        QPushButton* wireframeBtn = new QPushButton("WIREFRAME");
        connect(wireframeBtn, &QPushButton::clicked, this, &DiscoveryWindow::toggleEdges);
        t1->addWidget(wireframeBtn, 0, 5);

        autoOpenBoxCheck = new QCheckBox("AUTO-OPEN BOX WALLS (HIDE FRONT WALLS BLOCKING CAMERA)");
        autoOpenBoxCheck->setChecked(true);
        connect(autoOpenBoxCheck, &QCheckBox::toggled, this, &DiscoveryWindow::updateAutoOpenBoxWalls);
        t1->addWidget(autoOpenBoxCheck, 1, 0, 1, 3);

        t1->addWidget(new QLabel("BOX OPACITY:"), 1, 3);
        boxOpacitySlider = new QSlider(Qt::Horizontal);
        boxOpacitySlider->setRange(10, 100);
        boxOpacitySlider->setValue(45);
        connect(boxOpacitySlider, &QSlider::valueChanged, [this](int val) {
            for (auto& kv : boxWallActors) {
                if (kv.second.actor) kv.second.actor->GetProperty()->SetOpacity(val / 100.0);
            }
            renderWindow->Render();
        });
        t1->addWidget(boxOpacitySlider, 1, 4);

        QPushButton* debugBtn = new QPushButton("DEBUG LOGS");
        connect(debugBtn, &QPushButton::clicked, [this]() {
            generateDiagnosticReport();
            debugDialog->show();
            debugDialog->raise();
        });
        t1->addWidget(debugBtn, 1, 5);

        topRibbonTabs->addTab(tabImport, "01 // IMPORT & FVM/FEM MODE");

        // ---------------- TAB 2: 02 // SCAN & REPAIR CAD ----------------
        QWidget* tabRepair = new QWidget();
        QGridLayout* t2 = new QGridLayout(tabRepair);
        t2->setContentsMargins(10, 4, 10, 4);
        t2->setSpacing(6);

        QPushButton* scanCadBtn = new QPushButton("SCAN CAD FOR LEAKS & SLIVERS");
        scanCadBtn->setObjectName("OrangeBtn");
        connect(scanCadBtn, &QPushButton::clicked, this, &DiscoveryWindow::onScanCADProblems);
        t2->addWidget(scanCadBtn, 0, 0);

        t2->addWidget(new QLabel("SEW TOL:"), 0, 1);
        healTolSpin = new QDoubleSpinBox();
        healTolSpin->setDecimals(5);
        healTolSpin->setRange(1e-6, 10.0);
        healTolSpin->setValue(1e-3);
        t2->addWidget(healTolSpin, 0, 2);

        QPushButton* healBtn = new QPushButton("AUTO-SEW & HEAL GAPS");
        healBtn->setObjectName("PrimaryBtn");
        connect(healBtn, &QPushButton::clicked, this, &DiscoveryWindow::onHealCAD);
        t2->addWidget(healBtn, 0, 3);

        QPushButton* capHolesBtn = new QPushButton("CAP / PATCH OPEN HOLES (MAKE SOLID FOR FEM/FVM)");
        connect(capHolesBtn, &QPushButton::clicked, this, &DiscoveryWindow::onCapHoles);
        t2->addWidget(capHolesBtn, 0, 4);

        t2->addWidget(new QLabel("SLIVER %:"), 1, 0);
        sliverRatioSpin = new QDoubleSpinBox();
        sliverRatioSpin->setRange(0.01, 5.0);
        sliverRatioSpin->setDecimals(2);
        sliverRatioSpin->setValue(0.25);
        sliverRatioSpin->setSuffix("%");
        t2->addWidget(sliverRatioSpin, 1, 1);

        QPushButton* removeSliversBtn = new QPushButton("REMOVE SLIVER FACES");
        connect(removeSliversBtn, &QPushButton::clicked, this, &DiscoveryWindow::onRemoveSliverFaces);
        t2->addWidget(removeSliversBtn, 1, 2);

        QPushButton* deleteFaceBtn = new QPushButton("DELETE SELECTED FACE(S)");
        deleteFaceBtn->setObjectName("DangerBtn");
        connect(deleteFaceBtn, &QPushButton::clicked, this, &DiscoveryWindow::onDeleteSelectedFaces);
        t2->addWidget(deleteFaceBtn, 1, 3);

        QPushButton* fuseBtn = new QPushButton("FUSE OVERLAPPING VOLUMES (CONFORMAL FEM/FVM)");
        connect(fuseBtn, &QPushButton::clicked, this, &DiscoveryWindow::onFragmentAllVolumes);
        t2->addWidget(fuseBtn, 1, 4);

        topRibbonTabs->addTab(tabRepair, "02 // SCAN & REPAIR CAD");

        // ---------------- TAB 3: 03 // FLUID BOX & WALLS ----------------
        QWidget* tabDomain = new QWidget();
        QGridLayout* t3 = new QGridLayout(tabDomain);
        t3->setContentsMargins(8, 3, 8, 3);
        t3->setSpacing(4);

        previewBoxCheck = new QCheckBox("BOX PREVIEW");
        connect(previewBoxCheck, &QCheckBox::toggled, this, &DiscoveryWindow::updateEnclosurePreview);
        t3->addWidget(previewBoxCheck, 0, 0);

        auto makePadSpin = [this](double val, const QString& prefix) {
            QDoubleSpinBox* s = new QDoubleSpinBox();
            s->setRange(0.0, 100000.0);
            s->setDecimals(1);
            s->setValue(val);
            s->setPrefix(prefix);
            connect(s, &QDoubleSpinBox::valueChanged, this, &DiscoveryWindow::updateEnclosurePreview);
            return s;
        };
        padNegX = makePadSpin(2.0, "-X:"); padPosX = makePadSpin(5.0, "+X:");
        padNegY = makePadSpin(2.0, "-Y:"); padPosY = makePadSpin(2.0, "+Y:");
        padNegZ = makePadSpin(0.1, "-Z:"); padPosZ = makePadSpin(2.5, "+Z:");

        t3->addWidget(padNegX, 0, 1); t3->addWidget(padPosX, 0, 2);
        t3->addWidget(padNegY, 0, 3); t3->addWidget(padPosY, 0, 4);
        t3->addWidget(padNegZ, 0, 5); t3->addWidget(padPosZ, 0, 6);

        QPushButton* createBoxBtn = new QPushButton("CREATE SHADED FLUID BOX");
        createBoxBtn->setObjectName("PrimaryBtn");
        connect(createBoxBtn, &QPushButton::clicked, this, &DiscoveryWindow::onCreateFluidEnclosure);
        t3->addWidget(createBoxBtn, 0, 7, 1, 2);

        wallFrontNegXCombo = createBoundaryRoleCombo("inlet");
        wallRearPosXCombo  = createBoundaryRoleCombo("outlet");
        wallLeftNegYCombo  = createBoundaryRoleCombo("slip_wall");
        wallRightPosYCombo = createBoundaryRoleCombo("slip_wall");
        wallFloorNegZCombo = createBoundaryRoleCombo("moving_ground");
        wallRoofPosZCombo  = createBoundaryRoleCombo("slip_wall");

        t3->addWidget(new QLabel("Front(-X):"), 1, 0); t3->addWidget(wallFrontNegXCombo, 1, 1);
        t3->addWidget(new QLabel("Rear(+X):"), 1, 2);  t3->addWidget(wallRearPosXCombo, 1, 3);
        t3->addWidget(new QLabel("Left(-Y):"), 1, 4);  t3->addWidget(wallLeftNegYCombo, 1, 5);
        t3->addWidget(new QLabel("Right(+Y):"), 1, 6); t3->addWidget(wallRightPosYCombo, 1, 7);
        t3->addWidget(new QLabel("Floor(-Z):"), 1, 8); t3->addWidget(wallFloorNegZCombo, 1, 9);
        t3->addWidget(new QLabel("Roof(+Z):"), 0, 9);  t3->addWidget(wallRoofPosZCombo, 0, 10);

        topRibbonTabs->addTab(tabDomain, "03 // FLUID BOX & WALLS");

        // ---------------- TAB 4: 04 // TURNING & YAW FLOW ----------------
        QWidget* tabTurning = new QWidget();
        QGridLayout* tTurn = new QGridLayout(tabTurning);
        tTurn->setContentsMargins(10, 4, 10, 4);
        tTurn->setSpacing(6);

        tTurn->addWidget(new QLabel("MODEL ATTITUDE:"), 0, 0);
        modelYawSpin = new QDoubleSpinBox(); modelYawSpin->setRange(-180, 180); modelYawSpin->setValue(0.0); modelYawSpin->setPrefix("Yaw(Z): "); modelYawSpin->setSuffix("°");
        modelPitchSpin = new QDoubleSpinBox(); modelPitchSpin->setRange(-90, 90); modelPitchSpin->setValue(0.0); modelPitchSpin->setPrefix("Pitch(Y): "); modelPitchSpin->setSuffix("°");
        modelRollSpin = new QDoubleSpinBox(); modelRollSpin->setRange(-90, 90); modelRollSpin->setValue(0.0); modelRollSpin->setPrefix("Roll(X): "); modelRollSpin->setSuffix("°");
        tTurn->addWidget(modelYawSpin, 0, 1);
        tTurn->addWidget(modelPitchSpin, 0, 2);
        tTurn->addWidget(modelRollSpin, 0, 3);

        QPushButton* rotModelBtn = new QPushButton("ROTATE CAD MODEL IN TUNNEL");
        rotModelBtn->setObjectName("PrimaryBtn");
        connect(rotModelBtn, &QPushButton::clicked, this, &DiscoveryWindow::onRotateCADModel);
        tTurn->addWidget(rotModelBtn, 0, 4);

        tTurn->addWidget(new QLabel("TURNING AIRFLOW:"), 1, 0);
        flowSpeedSpin = new QDoubleSpinBox(); flowSpeedSpin->setRange(0.1, 2000.0); flowSpeedSpin->setValue(30.0); flowSpeedSpin->setPrefix("U: "); flowSpeedSpin->setSuffix(" m/s");
        flowYawAngleSpin = new QDoubleSpinBox(); flowYawAngleSpin->setRange(-90.0, 90.0); flowYawAngleSpin->setValue(12.0); flowYawAngleSpin->setPrefix("Sideslip β: "); flowYawAngleSpin->setSuffix("°");
        turningRadiusSpin = new QDoubleSpinBox(); turningRadiusSpin->setRange(0.0, 10000.0); turningRadiusSpin->setValue(45.0); turningRadiusSpin->setPrefix("Turn R: "); turningRadiusSpin->setSuffix(" m");

        tTurn->addWidget(flowSpeedSpin, 1, 1);
        tTurn->addWidget(flowYawAngleSpin, 1, 2);
        tTurn->addWidget(turningRadiusSpin, 1, 3);

        QPushButton* autoYawWallsBtn = new QPushButton("AUTO-SET BOX WALLS FOR TURNING / YAW");
        autoYawWallsBtn->setObjectName("OrangeBtn");
        connect(autoYawWallsBtn, &QPushButton::clicked, this, &DiscoveryWindow::onAutoAssignWallsForTurningFlow);
        tTurn->addWidget(autoYawWallsBtn, 1, 4);

        topRibbonTabs->addTab(tabTurning, "04 // TURNING & YAW FLOW");

        // ---------------- TAB 5: 05 // MESH, ELEMENT ORDER (FVM/FEM) & PAINT ----------------
        QWidget* tabMesh = new QWidget();
        QGridLayout* t4 = new QGridLayout(tabMesh);
        t4->setContentsMargins(8, 4, 8, 4);
        t4->setSpacing(5);

        algo2DCombo = new QComboBox();
        algo2DCombo->addItem("2D: Frontal-Delaunay", 6);
        algo2DCombo->addItem("2D: Delaunay (Fast)", 5);
        algo2DCombo->addItem("2D: MeshAdapt", 1);
        algo2DCombo->addItem("2D: BAMG Aniso", 7);
        t4->addWidget(algo2DCombo, 0, 0);

        algo3DCombo = new QComboBox();
        algo3DCombo->addItem("3D: Delaunay 3D", 1);
        algo3DCombo->addItem("3D: HXT Parallel 3D", 10);
        algo3DCombo->addItem("3D: Frontal 3D", 4);
        t4->addWidget(algo3DCombo, 0, 1);

        // Element Order Selector: 1st-Order Linear (FVM) vs 2nd-Order Quadratic (FEM Tet10/Tri6)!
        elemOrderCombo = new QComboBox();
        elemOrderCombo->addItem("Order 1: Linear (Tet4/Tri3 — FVM)", 1);
        elemOrderCombo->addItem("Order 2: Quadratic (Tet10/Tri6 — FEM)", 2);
        elemOrderCombo->setCurrentIndex(activeProfile.defaultElementOrder == 2 ? 1 : 0);
        t4->addWidget(elemOrderCombo, 0, 2);

        minSizeSpin = new QDoubleSpinBox(); minSizeSpin->setRange(0.0, 10000.0); minSizeSpin->setDecimals(4); minSizeSpin->setValue(0.0); minSizeSpin->setPrefix("Min: ");
        maxSizeSpin = new QDoubleSpinBox(); maxSizeSpin->setRange(0.001, 100000.0); maxSizeSpin->setDecimals(2); maxSizeSpin->setValue(activeProfile.defaultMaxSize); maxSizeSpin->setPrefix("Max: ");
        curvatureSpin = new QDoubleSpinBox(); curvatureSpin->setRange(4.0, 200.0); curvatureSpin->setValue(28.0); curvatureSpin->setPrefix("Curv: ");
        t4->addWidget(minSizeSpin, 0, 3);
        t4->addWidget(maxSizeSpin, 0, 4);
        t4->addWidget(curvatureSpin, 0, 5);

        QPushButton* gen2DBtn = new QPushButton("MESH 2D");
        connect(gen2DBtn, &QPushButton::clicked, [this]() { generateCFDViewMesh(2); });
        t4->addWidget(gen2DBtn, 0, 6);

        QPushButton* gen3DBtn = new QPushButton("GENERATE 3D MESH");
        gen3DBtn->setObjectName("PrimaryBtn");
        connect(gen3DBtn, &QPushButton::clicked, [this]() { generateCFDViewMesh(3); });
        t4->addWidget(gen3DBtn, 0, 7);

        enableBlCheck = new QCheckBox("WAKE / BL BOX");
        enableBlCheck->setChecked(activeProfile.discretizationMethod != "FEM");
        t4->addWidget(enableBlCheck, 1, 0);

        highOrderCurveOptCheck = new QCheckBox("FEM CURVED JACOBIAN OPT");
        highOrderCurveOptCheck->setChecked(true);
        highOrderCurveOptCheck->setToolTip("Optimizes 2nd-order quadratic Tet10 mid-edge nodes on curved CAD surfaces so Jacobians stay positive");
        t4->addWidget(highOrderCurveOptCheck, 1, 1);

        blLayersSpin = new QSpinBox(); blLayersSpin->setRange(1, 50); blLayersSpin->setValue(8); blLayersSpin->setPrefix("Layers: ");
        blFirstHeightSpin = new QDoubleSpinBox(); blFirstHeightSpin->setRange(1e-5, 100.0); blFirstHeightSpin->setDecimals(4); blFirstHeightSpin->setValue(0.01);
        blRatioSpin = new QDoubleSpinBox(); blRatioSpin->setRange(1.05, 2.0); blRatioSpin->setDecimals(2); blRatioSpin->setValue(1.20);
        t4->addWidget(blLayersSpin, 1, 2);

        selectModeBtn = new QPushButton("CURSOR: SELECT"); selectModeBtn->setCheckable(true); selectModeBtn->setChecked(true);
        paintModeBtn = new QPushButton("PAINT ON BODY"); paintModeBtn->setCheckable(true);

        connect(selectModeBtn, &QPushButton::clicked, [this]() { setPaintModeActive(false); });
        connect(paintModeBtn, &QPushButton::clicked, [this](bool checked) { setPaintModeActive(checked); });
        t4->addWidget(selectModeBtn, 1, 3);
        t4->addWidget(paintModeBtn, 1, 4);

        brushRadiusSpin = new QDoubleSpinBox(); brushRadiusSpin->setRange(0.001, 5000.0); brushRadiusSpin->setDecimals(3); brushRadiusSpin->setValue(1.0); brushRadiusSpin->setPrefix("R: ");
        brushThicknessSlider = new QSlider(Qt::Horizontal);
        brushThicknessSlider->setRange(1, 200);
        brushThicknessSlider->setValue(50);
        connect(brushThicknessSlider, &QSlider::valueChanged, [this](int sliderVal) {
            double dx = std::max(1e-3, carOnlyBounds[1] - carOnlyBounds[0]);
            double dy = std::max(1e-3, carOnlyBounds[3] - carOnlyBounds[2]);
            double dz = std::max(1e-3, carOnlyBounds[5] - carOnlyBounds[4]);
            double diag = std::hypot(dx, std::hypot(dy, dz));
            brushRadiusSpin->setValue(std::max(0.001, diag * 0.0025 * sliderVal));
        });

        brushSizeSpin = new QDoubleSpinBox(); brushSizeSpin->setRange(0.0001, 1000.0); brushSizeSpin->setDecimals(3); brushSizeSpin->setValue(0.05); brushSizeSpin->setPrefix("Fine: ");
        t4->addWidget(brushRadiusSpin, 1, 5);
        t4->addWidget(brushThicknessSlider, 1, 6);
        t4->addWidget(brushSizeSpin, 1, 7);

        topRibbonTabs->addTab(tabMesh, "05 // MESH, ORDER (FVM/FEM) & PAINT");

        // ---------------- TAB 6: 06 // QUALITY, SSD CACHE & EXPORT ----------------
        QWidget* tabQuality = new QWidget();
        QHBoxLayout* t5Layout = new QHBoxLayout(tabQuality);
        t5Layout->setContentsMargins(10, 4, 10, 4);

        QPushButton* scanMeshBtn = new QPushButton("SCAN FVM/FEM QUALITY");
        scanMeshBtn->setObjectName("OrangeBtn");
        connect(scanMeshBtn, &QPushButton::clicked, this, &DiscoveryWindow::onScanMeshQuality);
        t5Layout->addWidget(scanMeshBtn);

        smoothingSpin = new QSpinBox(); smoothingSpin->setRange(1, 100); smoothingSpin->setValue(10); smoothingSpin->setPrefix("Smooth: ");
        t5Layout->addWidget(smoothingSpin);

        optimizeCheck = new QCheckBox("3D OPTIMIZER"); optimizeCheck->setChecked(true);
        t5Layout->addWidget(optimizeCheck);

        autoOffloadSsdCheck = new QCheckBox("AUTO-CACHE 3D VOLUME TO SSD & FREE RAM");
        autoOffloadSsdCheck->setChecked(true);
        t5Layout->addWidget(autoOffloadSsdCheck);

        QPushButton* purgeRamBtn = new QPushButton("PURGE RAM ➔ SSD NOW");
        connect(purgeRamBtn, &QPushButton::clicked, this, &DiscoveryWindow::onManualPurgeRamToSsd);
        t5Layout->addWidget(purgeRamBtn);

        QPushButton* clearPaintBtn = new QPushButton("CLEAR PAINT");
        connect(clearPaintBtn, &QPushButton::clicked, this, &DiscoveryWindow::clearPaintedRefinement);
        t5Layout->addWidget(clearPaintBtn);

        qualitySummaryLabel = new QLabel("QUALITY: UNSCANNED");
        qualitySummaryLabel->setObjectName("HudTitle");
        t5Layout->addWidget(qualitySummaryLabel);

        t5Layout->addStretch();

        QPushButton* exportBtn = new QPushButton("EXPORT FVM / FEM MESH");
        exportBtn->setObjectName("PrimaryBtn");
        connect(exportBtn, &QPushButton::clicked, this, &DiscoveryWindow::exportForSelectedSolver);
        t5Layout->addWidget(exportBtn);

        topRibbonTabs->addTab(tabQuality, "06 // QUALITY, SSD CACHE & EXPORT");

        mainOverlay->addWidget(topRibbonTabs);

        // ================= MIDDLE WORKSPACE + DUAL FVM/FEM SIDEBAR =================
        QHBoxLayout* midLayout = new QHBoxLayout();
        midLayout->addStretch();

        QFrame* rightHud = new QFrame();
        rightHud->setObjectName("HudPanel");
        rightHud->setFixedWidth(315);
        QVBoxLayout* rightLayout = new QVBoxLayout(rightHud);
        rightLayout->setContentsMargins(10, 8, 10, 8);
        rightLayout->setSpacing(4);

        QLabel* scanTitle = new QLabel("PROBLEM SCANNER & FVM/FEM DIAGNOSTICS");
        scanTitle->setObjectName("OrangeBadge");
        rightLayout->addWidget(scanTitle);

        problemListWidget = new QListWidget();
        problemListWidget->setFixedHeight(78);
        connect(problemListWidget, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
            int surfTag = item->data(Qt::UserRole).toInt();
            if (surfTag > 0) selectSurfaceTag(surfTag, false);
        });
        rightLayout->addWidget(problemListWidget);

        QLabel* quickWallTitle = new QLabel("1-CLICK SELECT WIND TUNNEL WALL:");
        quickWallTitle->setObjectName("HudTitle");
        rightLayout->addWidget(quickWallTitle);

        QGridLayout* wallPickGrid = new QGridLayout();
        wallPickGrid->setSpacing(3);
        const std::vector<std::pair<QString, std::string>> wallBtns = {
            {"Front (-X)", "-X"}, {"Rear (+X)", "+X"}, {"Left (-Y)", "-Y"},
            {"Right (+Y)", "+Y"}, {"Floor (-Z)", "-Z"}, {"Roof (+Z)", "+Z"}
        };
        for (size_t i = 0; i < wallBtns.size(); ++i) {
            QPushButton* wb = new QPushButton(wallBtns[i].first);
            std::string roleKey = wallBtns[i].second;
            connect(wb, &QPushButton::clicked, [this, roleKey]() {
                if (boxWallRoleToTag.count(roleKey)) {
                    selectSurfaceTag(boxWallRoleToTag[roleKey], false);
                } else {
                    setProgressState(0, "Create Fluid Box First", "Go to Tab '03 // FLUID BOX & WALLS' and click 'CREATE SHADED FLUID BOX'.");
                }
            });
            wallPickGrid->addWidget(wb, i / 3, i % 3);
        }
        rightLayout->addLayout(wallPickGrid);

        // Dual FVM & FEM Boundary / Load Role Presets!
        QLabel* patchTitle = new QLabel("ASSIGN FVM PATCH OR FEM LOAD/BC SET:");
        patchTitle->setObjectName("OrangeBadge");
        rightLayout->addWidget(patchTitle);

        QGridLayout* roleBtnGrid = new QGridLayout();
        roleBtnGrid->setSpacing(3);
        const std::vector<std::pair<QString, QString>> rolePresets = {
            {"[FVM] INLET", "inlet"}, {"[FVM] OUTLET", "outlet"}, {"[FVM] SLIP", "slip_wall"},
            {"[FVM] NO-SLIP", "no_slip_wall"}, {"[FVM] MOV GND", "moving_ground"}, {"SYMMETRY", "symmetry"},
            {"[FEM] FIXED", "fixed_support"}, {"[FEM] PRESS", "pressure_load"}, {"[FSI] INTERFACE", "fsi_interface"}
        };
        for (size_t i = 0; i < rolePresets.size(); ++i) {
            QPushButton* rb = new QPushButton(rolePresets[i].first);
            QString roleName = rolePresets[i].second;
            connect(rb, &QPushButton::clicked, [this, roleName]() {
                patchNameInput->setText(roleName);
                assignPatchToSelected();
            });
            roleBtnGrid->addWidget(rb, i / 3, i % 3);
        }
        rightLayout->addLayout(roleBtnGrid);

        faceListWidget = new QListWidget();
        connect(faceListWidget, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
            int tag = item->data(Qt::UserRole).toInt();
            selectSurfaceTag(tag, false);
        });
        rightLayout->addWidget(faceListWidget);

        QHBoxLayout* customAssignRow = new QHBoxLayout();
        patchNameInput = new QLineEdit("inlet");
        QPushButton* assignBtn = new QPushButton("ASSIGN CUSTOM");
        assignBtn->setObjectName("PrimaryBtn");
        connect(assignBtn, &QPushButton::clicked, this, &DiscoveryWindow::assignPatchToSelected);
        customAssignRow->addWidget(patchNameInput);
        customAssignRow->addWidget(assignBtn);
        rightLayout->addLayout(customAssignRow);

        // Section Cut Plane (Out-of-Core mmap Slicer)
        QLabel* clipTitle = new QLabel("3D SECTION CUT PLANE (MMAP SSD SLICER)");
        clipTitle->setObjectName("HudTitle");
        rightLayout->addWidget(clipTitle);

        QHBoxLayout* clipTopRow = new QHBoxLayout();
        clipEnableCheck = new QCheckBox("Slice Model");
        connect(clipEnableCheck, &QCheckBox::toggled, this, &DiscoveryWindow::updateClippingPlane);
        clipTopRow->addWidget(clipEnableCheck);

        show3DTetsCheck = new QCheckBox("Stream 3D Tets from SSD");
        show3DTetsCheck->setChecked(true);
        connect(show3DTetsCheck, &QCheckBox::toggled, this, &DiscoveryWindow::updateClippingPlane);
        clipTopRow->addWidget(show3DTetsCheck);
        rightLayout->addLayout(clipTopRow);

        QHBoxLayout* clipAxisRow = new QHBoxLayout();
        clipAxisCombo = new QComboBox();
        clipAxisCombo->addItems({"X Axis", "Y Axis", "Z Axis"});
        clipAxisCombo->setCurrentIndex(1);
        connect(clipAxisCombo, &QComboBox::currentIndexChanged, this, &DiscoveryWindow::updateClippingPlane);
        clipAxisRow->addWidget(clipAxisCombo);

        clipFlipCheck = new QCheckBox("Flip");
        connect(clipFlipCheck, &QCheckBox::toggled, this, &DiscoveryWindow::updateClippingPlane);
        clipAxisRow->addWidget(clipFlipCheck);

        clipSlider = new QSlider(Qt::Horizontal);
        clipSlider->setRange(0, 100);
        clipSlider->setValue(50);
        connect(clipSlider, &QSlider::valueChanged, this, &DiscoveryWindow::updateClippingPlane);
        clipAxisRow->addWidget(clipSlider);
        rightLayout->addLayout(clipAxisRow);

        midLayout->addWidget(rightHud);
        mainOverlay->addLayout(midLayout);

        // ================= BOTTOM PROGRESS BAR & LIVE RAM / SSD CACHE TELEMETRY HUD =================
        QFrame* bottomHud = new QFrame();
        bottomHud->setObjectName("HudPanel");
        bottomHud->setFixedHeight(58);
        QVBoxLayout* bottomLayout = new QVBoxLayout(bottomHud);
        bottomLayout->setContentsMargins(12, 4, 12, 4);
        bottomLayout->setSpacing(2);

        QHBoxLayout* progRow = new QHBoxLayout();
        statusLabel = new QLabel("READY — SIMUOP LABS FVM & FEM STUDIO");
        statusLabel->setStyleSheet("font-weight: bold; color: #10b981;");

        ramTelemetryLabel = new QLabel("RAM: 0 MB  |  SSD CACHE: 0 MB");
        ramTelemetryLabel->setObjectName("OrangeBadge");

        progressBar = new QProgressBar();
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setFixedWidth(280);

        progRow->addWidget(statusLabel);
        progRow->addStretch();
        progRow->addWidget(ramTelemetryLabel);
        progRow->addSpacing(12);
        progRow->addWidget(progressBar);
        bottomLayout->addLayout(progRow);

        commentLabel = new QLabel("Dual FVM (1st-Order Cell-Centered) & FEM (2nd-Order Quadratic Tet10/Tri6 + NSET/ELSET) Engine Ready.");
        commentLabel->setObjectName("CommentLabel");
        bottomLayout->addWidget(commentLabel);

        mainOverlay->addWidget(bottomHud);
    }

    // Automatically adapts Element Order, Wake Box, and Solver selection when switching between FVM and FEM!
    void onDiscretizationMethodChanged() {
        activeProfile.discretizationMethod = methodCombo->currentData().toString();
        updateWindowTitleBanner();

        if (activeProfile.discretizationMethod == "FVM") {
            elemOrderCombo->setCurrentIndex(0); // Order 1 Linear (Tet4/Tri3) for FVM
            enableBlCheck->setChecked(true);
            if (solverCombo->currentIndex() >= 5) solverCombo->setCurrentIndex(0);
            setProgressState(100, "Switched to FVM Mode (Finite Volume Method)",
                "Set Element Order = 1 (Linear Tet4/Tri3) for cell-centered flux conservation (BaramFlow, OpenFOAM, Fluent, SU2).");
        } else if (activeProfile.discretizationMethod == "FEM") {
            elemOrderCombo->setCurrentIndex(1); // Order 2 Quadratic (Tet10/Tri6) for FEM
            enableBlCheck->setChecked(false);
            if (solverCombo->currentIndex() < 5) solverCombo->setCurrentIndex(5); // CalculiX/Abaqus .inp
            setProgressState(100, "Switched to FEM Mode (Finite Element Method)",
                "Set Element Order = 2 (Quadratic Tet10/Tri6) with High-Order Curved Jacobian Optimization and NSET/ELSET export.");
        } else {
            setProgressState(100, "Switched to Coupled FVM + FEM Mode (FSI)",
                "Ready to export both FVM fluid meshes and FEM structural meshes with shared 'fsi_interface' patches.");
        }
    }

    void onManualPurgeRamToSsd() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);

        double ramBefore = OutOfCoreMeshCache::getCurrentRamUsageMB();
        if (isMeshMode && !diskCache.hasDiskCache) {
            diskCache.offload3DMeshToDiskAndFreeRAM(true);
        } else {
            OutOfCoreMeshCache::releaseFreeRamToMacOS();
        }
        double ramAfter = OutOfCoreMeshCache::getCurrentRamUsageMB();
        updateRamTelemetryUI();

        setProgressState(100,
            QString("Purged Heap & Cached to SSD (RAM: %1 MB ➔ %2 MB)").arg(ramBefore, 0, 'f', 0).arg(ramAfter, 0, 'f', 0),
            QString("SSD Cache holds %1 tetrahedra (%2 MB on disk).").arg(diskCache.numCachedTets).arg(diskCache.cachedDiskMB, 0, 'f', 1));
    }

    // ================= SURFACE-SKIN PAINTING & MEMORY PERSISTENCE =================
    void setPaintModeActive(bool enable) {
        if (enable && interactorStyle->paintMode) {
            enable = false;
        }
        interactorStyle->paintMode = enable;
        paintModeBtn->setChecked(enable);
        selectModeBtn->setChecked(!enable);

        if (!enable) {
            hoverInnerCells.clear();
            setProgressState(100,
                QString("Paint Hidden — %1 Painted Surface Zones Remembered in Memory").arg(refinementStrokes.size()),
                "Orange paint overlay hidden for clean viewing. Your painted zones remain saved for FVM/FEM meshing!");
        } else {
            vtkWidget->setFocus();
            setProgressState(100,
                QString("Surface Paint Mode Active (%1 Saved Zones Shown)").arg(refinementStrokes.size()),
                "Drag on the body to paint refinement directly on the surface! Press '[' or ']' to resize brush, or click 'PAINT ON BODY' again to hide.");
        }
        updateSurfaceCellColors();
        renderWindow->Render();
    }

    void updateSurfaceBrushHover(double x, double y, double z, bool visible) {
        hoverInnerCells.clear();
        if (visible && interactorStyle->paintMode && !innerCellCentroids.empty()) {
            double r2 = brushRadiusSpin->value() * brushRadiusSpin->value();
            size_t numCells = innerCellToTag.size();
            for (size_t i = 0; i < numCells; ++i) {
                double dx = innerCellCentroids[3*i]     - x;
                double dy = innerCellCentroids[3*i + 1] - y;
                double dz = innerCellCentroids[3*i + 2] - z;
                if (dx*dx + dy*dy + dz*dz <= r2) {
                    hoverInnerCells.insert(static_cast<vtkIdType>(i));
                }
            }
        }
        updateSurfaceCellColors();
        renderWindow->Render();
    }

    void paintOnBodySurface(double x, double y, double z) {
        if (innerCellCentroids.empty()) return;
        double r = brushRadiusSpin->value();
        double r2 = r * r;
        double fineSize = brushSizeSpin->value();

        std::vector<vtkIdType> newlyPainted;
        size_t numCells = innerCellToTag.size();
        for (size_t i = 0; i < numCells; ++i) {
            double dx = innerCellCentroids[3*i]     - x;
            double dy = innerCellCentroids[3*i + 1] - y;
            double dz = innerCellCentroids[3*i + 2] - z;
            if (dx*dx + dy*dy + dz*dz <= r2) {
                vtkIdType cid = static_cast<vtkIdType>(i);
                paintedInnerCells[cid] = fineSize;
                newlyPainted.push_back(cid);
            }
        }

        if (newlyPainted.empty()) return;

        bool tooClose = false;
        for (const auto& s : refinementStrokes) {
            double d = std::hypot(x - s.x, std::hypot(y - s.y, z - s.z));
            if (d < r * 0.30) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) {
            refinementStrokes.push_back({x, y, z, r, fineSize, newlyPainted});
        }

        statusLabel->setText(QString("Painted Body Surface (%1 zones remembered in memory)").arg(refinementStrokes.size()));
        updateSurfaceCellColors();
        renderWindow->Render();
    }

    void scaleBrushRadiusLive(double factor) {
        double currentR = brushRadiusSpin->value();
        double newR = std::clamp(currentR * factor, brushRadiusSpin->minimum(), brushRadiusSpin->maximum());
        brushRadiusSpin->setValue(newR);
        statusLabel->setText(QString("Brush Thickness (Radius): %1").arg(newR, 0, 'f', 3));
    }

    void undoLastPaintStroke() {
        if (refinementStrokes.empty()) return;
        refinementStrokes.pop_back();
        paintedInnerCells.clear();
        for (const auto& s : refinementStrokes) {
            for (vtkIdType cid : s.paintedCells) {
                paintedInnerCells[cid] = s.targetSize;
            }
        }
        statusLabel->setText(QString("Undid Paint Stroke (%1 zones in memory)").arg(refinementStrokes.size()));
        updateSurfaceCellColors();
        renderWindow->Render();
    }

    void clearPaintedRefinement() {
        refinementStrokes.clear();
        paintedInnerCells.clear();
        hoverInnerCells.clear();
        updateSurfaceCellColors();
        renderWindow->Render();
        statusLabel->setText("Cleared all remembered paint refinement zones.");
    }

    // ================= 1. IMPORT CAD WITH HIGH-RES PER-FACE B-REP SHADING =================
    void onOpenCAD() {
        if (isBusy) return;
        QString file = QFileDialog::getOpenFileName(this, "Import CAD Model", "", "CAD Files (*.step *.stp *.iges *.igs *.brep)");
        if (file.isEmpty()) return;

        ScopedBusyLock lock(isBusy);
        auto tStart = std::chrono::high_resolution_clock::now();
        setProgressState(5, "Resetting Scene & Clearing SSD Cache...", "Reclaiming RAM and clearing previous OpenCASCADE state.");
        resetSceneAndKernel();
        gmsh::model::add("user_cad");

        try {
            setProgressState(20, "Reading CAD File...", QString("OpenCASCADE is parsing '%1'...").arg(file));
            gmsh::vectorpair dimTags;
            gmsh::model::occ::importShapes(file.toStdString(), dimTags);

            setProgressState(50, "Synchronizing B-Rep Topology...", "Transferring analytical NURBS surfaces to model database...");
            gmsh::model::occ::synchronize();

            gmsh::vectorpair surfaces;
            gmsh::model::getEntities(surfaces, 2);
            if (surfaces.empty()) {
                setProgressState(0, "Import Warning", "No 2D surfaces found in the selected file.");
                return;
            }

            hasLoadedModel = true;
            isMeshMode = false;

            gmsh::model::getBoundingBox(-1, -1,
                modelBounds[0], modelBounds[2], modelBounds[4],
                modelBounds[1], modelBounds[3], modelBounds[5]);
            std::copy(std::begin(modelBounds), std::end(modelBounds), std::begin(carOnlyBounds));

            double dx = std::max(1e-3, modelBounds[1] - modelBounds[0]);
            double dy = std::max(1e-3, modelBounds[3] - modelBounds[2]);
            double dz = std::max(1e-3, modelBounds[5] - modelBounds[4]);
            double diag = std::hypot(dx, std::hypot(dy, dz));

            padNegX->blockSignals(true); padPosX->blockSignals(true);
            padNegY->blockSignals(true); padPosY->blockSignals(true);
            padNegZ->blockSignals(true); padPosZ->blockSignals(true);
            padNegX->setValue(dx * 1.2); padPosX->setValue(dx * 2.8);
            padNegY->setValue(dy * 1.5); padPosY->setValue(dy * 1.5);
            padNegZ->setValue(dz * 0.05); padPosZ->setValue(dz * 1.8);
            padNegX->blockSignals(false); padPosX->blockSignals(false);
            padNegY->blockSignals(false); padPosY->blockSignals(false);
            padNegZ->blockSignals(false); padPosZ->blockSignals(false);

            maxSizeSpin->setValue(diag * 0.05);
            minSizeSpin->setValue(0.0);
            blFirstHeightSpin->setValue(diag * 0.003);
            brushRadiusSpin->setValue(diag * 0.08);
            brushSizeSpin->setValue(diag * 0.008);
            turningRadiusSpin->setValue(dx * 8.0);

            for (const auto& s : surfaces) {
                tagToPatchName[s.second] = (activeProfile.discretizationMethod == "FEM") ? "solid_boundary" : "obstacle";
            }

            setProgressState(75, "Building High-Precision B-Rep Shaded Surface...",
                "Generating curvature-adapted CAD display with isolated per-face normals...");
            generateHighPrecisionCADDisplay();

            renderer->ResetCamera();
            updateAutoOpenBoxWalls();
            renderWindow->Render();

            auto tEnd = std::chrono::high_resolution_clock::now();
            double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();

            setProgressState(100,
                QString("CAD Loaded in %1 s").arg(elapsedSec, 0, 'f', 2),
                QString("Loaded %1 CAD surfaces in [%2] mode.").arg(surfaces.size()).arg(activeProfile.discretizationMethod));

        } catch (const std::exception& e) {
            setProgressState(0, "CAD Import Error", QString::fromStdString(e.what()));
        }
    }

    void generateHighPrecisionCADDisplay() {
        if (!hasLoadedModel) return;
        isMeshMode = false;
        showWireframe = false;
        lastGeneratedOrder = 1;

        gmsh::model::mesh::clear();
        gmsh::model::getBoundingBox(-1, -1,
            modelBounds[0], modelBounds[2], modelBounds[4],
            modelBounds[1], modelBounds[3], modelBounds[5]);
        double diag = std::hypot(modelBounds[1] - modelBounds[0],
                      std::hypot(modelBounds[3] - modelBounds[2], modelBounds[5] - modelBounds[4]));
        if (diag < 1e-6) diag = 10.0;

        gmsh::vectorpair surfaces;
        gmsh::model::getEntities(surfaces, 2);

        gmsh::option::setNumber("Mesh.ElementOrder", 1);
        gmsh::option::setNumber("Mesh.MeshSizeMin", 0.0);
        gmsh::option::setNumber("Mesh.MeshSizeMax", diag * 0.045);
        gmsh::option::setNumber("Mesh.MeshSizeFromPoints", 1);
        gmsh::option::setNumber("Mesh.MeshSizeFromCurvature", (surfaces.size() > 1500) ? 16 : 30);
        gmsh::option::setNumber("Mesh.Algorithm", 6);

        try {
            gmsh::model::mesh::generate(2);
        } catch (...) {
            gmsh::option::setNumber("Mesh.Algorithm", 1);
            try { gmsh::model::mesh::generate(2); } catch (...) {}
        }

        buildSceneActorsFromCurrentMesh(false);
        buildSharpCADFeatureCurvesFast();
        OutOfCoreMeshCache::releaseFreeRamToMacOS();
        updateRamTelemetryUI();
    }

    void getColorForPatchName(const std::string& patch, int tag, unsigned char rgb[3]) {
        if (selectedTags.count(tag)) {
            rgb[0] = 249; rgb[1] = 115; rgb[2] = 22; // Simuop Glowing Orange when selected
            return;
        }
        if (patch == "inlet" || patch == "pressure_load") {
            rgb[0] = 14; rgb[1] = 165; rgb[2] = 233;  // Cyan-Blue (Inlet / Pressure Load)
        } else if (patch == "outlet" || patch == "force_load") {
            rgb[0] = 239; rgb[1] = 68; rgb[2] = 68;   // Crimson Red (Outlet / Force Load)
        } else if (patch == "slip_wall" || patch == "fixed_support") {
            rgb[0] = 16; rgb[1] = 185; rgb[2] = 129;  // Emerald Green (Slip Wall / Fixed Support)
        } else if (patch == "moving_ground" || patch == "fsi_interface") {
            rgb[0] = 234; rgb[1] = 179; rgb[2] = 8;   // Amber Gold (Moving Ground / FSI Interface)
        } else if (patch == "no_slip_wall" || patch == "walls" || patch == "thermal_wall") {
            rgb[0] = 100; rgb[1] = 116; rgb[2] = 139; // Slate
        } else if (patch == "symmetry") {
            rgb[0] = 168; rgb[1] = 85; rgb[2] = 247;  // Purple Symmetry
        } else {
            rgb[0] = 212; rgb[1] = 222; rgb[2] = 234; // Crisp Metallic CAD Silver
        }
    }

    // Supports both 1st-Order Tri3 (type 2) and 2nd-Order Quadratic Tri6 (type 9) surface elements!
    size_t buildSceneActorsFromCurrentMesh(bool showMeshEdges) {
        if (innerModelActor) { renderer->RemoveActor(innerModelActor); innerModelActor = nullptr; }
        for (auto& kv : boxWallActors) {
            if (kv.second.actor) renderer->RemoveActor(kv.second.actor);
        }
        boxWallActors.clear();
        innerCellToTag.clear();
        innerCellCentroids.clear();
        hoverInnerCells.clear();

        gmsh::model::getBoundingBox(-1, -1,
            modelBounds[0], modelBounds[2], modelBounds[4],
            modelBounds[1], modelBounds[3], modelBounds[5]);

        std::vector<std::size_t> allNodeTags;
        std::vector<double> allCoords, allParams;
        gmsh::model::mesh::getNodes(allNodeTags, allCoords, allParams);

        std::unordered_map<std::size_t, std::array<double, 3>> nodeCoordMap;
        nodeCoordMap.reserve(allNodeTags.size());
        for (size_t i = 0; i < allNodeTags.size(); ++i) {
            nodeCoordMap[allNodeTags[i]] = {allCoords[3*i], allCoords[3*i+1], allCoords[3*i+2]};
        }
        allNodeTags.clear(); allNodeTags.shrink_to_fit();
        allCoords.clear();   allCoords.shrink_to_fit();
        allParams.clear();   allParams.shrink_to_fit();

        vtkNew<vtkPoints> innerPoints;
        vtkNew<vtkCellArray> innerTriangles;

        gmsh::vectorpair surfaces;
        gmsh::model::getEntities(surfaces, 2);

        double boxCenter[3] = {
            0.5 * (modelBounds[0] + modelBounds[1]),
            0.5 * (modelBounds[2] + modelBounds[3]),
            0.5 * (modelBounds[4] + modelBounds[5])
        };

        size_t totalTriangles = 0;

        for (const auto& surf : surfaces) {
            int tag = surf.second;
            if (tagToPatchName.find(tag) == tagToPatchName.end()) {
                tagToPatchName[tag] = "obstacle";
            }
            bool isOuterWall = (outerBoxSurfaceTags.count(tag) > 0);

            std::vector<int> elemTypes;
            std::vector<std::vector<std::size_t>> elemTags, elemNodeTags;
            gmsh::model::mesh::getElements(elemTypes, elemTags, elemNodeTags, 2, tag);

            if (isOuterWall) {
                vtkNew<vtkPoints> wallPts;
                vtkNew<vtkCellArray> wallTris;
                std::unordered_map<std::size_t, vtkIdType> wallNodeMap;

                for (size_t t = 0; t < elemTypes.size(); ++t) {
                    // Type 2 = 3-node Linear Triangle (FVM/FEM), Type 9 = 6-node Quadratic Triangle (FEM Order 2)
                    int stride = (elemTypes[t] == 2) ? 3 : ((elemTypes[t] == 9) ? 6 : 0);
                    if (stride > 0) {
                        const auto& nTags = elemNodeTags[t];
                        for (size_t j = 0; j < nTags.size(); j += stride) {
                            vtkIdType pts[3];
                            bool ok = true;
                            for (int k = 0; k < 3; ++k) {
                                auto it = wallNodeMap.find(nTags[j+k]);
                                if (it != wallNodeMap.end()) {
                                    pts[k] = it->second;
                                } else {
                                    auto cit = nodeCoordMap.find(nTags[j+k]);
                                    if (cit == nodeCoordMap.end()) { ok = false; break; }
                                    pts[k] = wallPts->InsertNextPoint(cit->second[0], cit->second[1], cit->second[2]);
                                    wallNodeMap[nTags[j+k]] = pts[k];
                                }
                            }
                            if (ok) {
                                wallTris->InsertNextCell(3, pts);
                                totalTriangles++;
                            }
                        }
                    }
                }

                BoxWallActorData wData;
                wData.surfaceTag = tag;
                double sx0, sy0, sz0, sx1, sy1, sz1;
                gmsh::model::getBoundingBox(2, tag, sx0, sy0, sz0, sx1, sy1, sz1);
                wData.center[0] = 0.5 * (sx0 + sx1);
                wData.center[1] = 0.5 * (sy0 + sy1);
                wData.center[2] = 0.5 * (sz0 + sz1);

                double dx = wData.center[0] - boxCenter[0];
                double dy = wData.center[1] - boxCenter[1];
                double dz = wData.center[2] - boxCenter[2];
                for (const auto& rw : boxWallRoleToTag) {
                    if (rw.second == tag) wData.roleKey = rw.first;
                }
                if (wData.roleKey == "-X")      { wData.outwardNormal[0] = -1; wData.outwardNormal[1] = 0; wData.outwardNormal[2] = 0; }
                else if (wData.roleKey == "+X") { wData.outwardNormal[0] =  1; wData.outwardNormal[1] = 0; wData.outwardNormal[2] = 0; }
                else if (wData.roleKey == "-Y") { wData.outwardNormal[0] =  0; wData.outwardNormal[1] = -1; wData.outwardNormal[2] = 0; }
                else if (wData.roleKey == "+Y") { wData.outwardNormal[0] =  0; wData.outwardNormal[1] =  1; wData.outwardNormal[2] = 0; }
                else if (wData.roleKey == "-Z") { wData.outwardNormal[0] =  0; wData.outwardNormal[1] = 0; wData.outwardNormal[2] = -1; }
                else if (wData.roleKey == "+Z") { wData.outwardNormal[0] =  0; wData.outwardNormal[1] = 0; wData.outwardNormal[2] =  1; }
                else {
                    double len = std::hypot(dx, std::hypot(dy, dz));
                    if (len > 1e-6) { wData.outwardNormal[0] = dx/len; wData.outwardNormal[1] = dy/len; wData.outwardNormal[2] = dz/len; }
                }

                wData.poly = vtkSmartPointer<vtkPolyData>::New();
                wData.poly->SetPoints(wallPts);
                wData.poly->SetPolys(wallTris);

                wData.mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
                wData.mapper->SetInputData(wData.poly);

                wData.actor = vtkSmartPointer<vtkActor>::New();
                wData.actor->SetMapper(wData.mapper);
                wData.actor->GetProperty()->SetOpacity(boxOpacitySlider->value() / 100.0);
                wData.actor->GetProperty()->SetEdgeVisibility(1);
                wData.actor->GetProperty()->SetEdgeColor(0.08, 0.20, 0.28);
                wData.actor->GetProperty()->SetLineWidth(showMeshEdges ? 1.2 : 1.0);

                renderer->AddActor(wData.actor);
                boxWallActors[tag] = wData;
            } else {
                std::unordered_map<std::size_t, vtkIdType> faceLocalNodeMap;
                for (size_t t = 0; t < elemTypes.size(); ++t) {
                    int stride = (elemTypes[t] == 2) ? 3 : ((elemTypes[t] == 9) ? 6 : 0);
                    if (stride > 0) {
                        const auto& nTags = elemNodeTags[t];
                        for (size_t j = 0; j < nTags.size(); j += stride) {
                            vtkIdType pts[3];
                            double triCoords[3][3];
                            bool ok = true;
                            for (int k = 0; k < 3; ++k) {
                                auto cit = nodeCoordMap.find(nTags[j+k]);
                                if (cit == nodeCoordMap.end()) { ok = false; break; }
                                triCoords[k][0] = cit->second[0];
                                triCoords[k][1] = cit->second[1];
                                triCoords[k][2] = cit->second[2];

                                auto it = faceLocalNodeMap.find(nTags[j+k]);
                                if (it != faceLocalNodeMap.end()) {
                                    pts[k] = it->second;
                                } else {
                                    pts[k] = innerPoints->InsertNextPoint(triCoords[k][0], triCoords[k][1], triCoords[k][2]);
                                    faceLocalNodeMap[nTags[j+k]] = pts[k];
                                }
                            }
                            if (ok) {
                                innerTriangles->InsertNextCell(3, pts);
                                innerCellToTag.push_back(tag);
                                innerCellCentroids.push_back((triCoords[0][0] + triCoords[1][0] + triCoords[2][0]) / 3.0);
                                innerCellCentroids.push_back((triCoords[0][1] + triCoords[1][1] + triCoords[2][1]) / 3.0);
                                innerCellCentroids.push_back((triCoords[0][2] + triCoords[1][2] + triCoords[2][2]) / 3.0);
                                totalTriangles++;
                            }
                        }
                    }
                }
            }
        }
        nodeCoordMap.clear();

        paintedInnerCells.clear();
        for (auto& stroke : refinementStrokes) {
            stroke.paintedCells.clear();
            double r2 = stroke.radius * stroke.radius;
            for (size_t i = 0; i < innerCellToTag.size(); ++i) {
                double dx = innerCellCentroids[3*i]     - stroke.x;
                double dy = innerCellCentroids[3*i + 1] - stroke.y;
                double dz = innerCellCentroids[3*i + 2] - stroke.z;
                if (dx*dx + dy*dy + dz*dz <= r2) {
                    vtkIdType cid = static_cast<vtkIdType>(i);
                    paintedInnerCells[cid] = stroke.targetSize;
                    stroke.paintedCells.push_back(cid);
                }
            }
        }

        vtkNew<vtkPolyData> rawInnerPoly;
        rawInnerPoly->SetPoints(innerPoints);
        rawInnerPoly->SetPolys(innerTriangles);

        if (!showMeshEdges) {
            vtkNew<vtkPolyDataNormals> normalGen;
            normalGen->SetInputData(rawInnerPoly);
            normalGen->ComputePointNormalsOn();
            normalGen->ComputeCellNormalsOff();
            normalGen->SplittingOff();
            normalGen->ConsistencyOn();
            normalGen->Update();
            innerModelPoly = normalGen->GetOutput();
        } else {
            innerModelPoly = rawInnerPoly;
        }

        innerModelMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        innerModelMapper->SetInputData(innerModelPoly);
        innerModelMapper->SetScalarModeToUseCellData();
        innerModelMapper->SelectColorArray("Colors");
        innerModelMapper->SetColorModeToDirectScalars();

        innerModelActor = vtkSmartPointer<vtkActor>::New();
        innerModelActor->SetMapper(innerModelMapper);
        if (!showMeshEdges) {
            innerModelActor->GetProperty()->SetInterpolationToPhong();
            innerModelActor->GetProperty()->SetSpecular(0.32);
            innerModelActor->GetProperty()->SetSpecularPower(26.0);
        } else {
            innerModelActor->GetProperty()->SetInterpolationToFlat();
        }
        innerModelActor->GetProperty()->SetOpacity(1.0);
        innerModelActor->GetProperty()->SetEdgeVisibility(showMeshEdges ? 1 : 0);
        innerModelActor->GetProperty()->SetEdgeColor(0.04, 0.08, 0.14);
        innerModelActor->GetProperty()->SetLineWidth(1.1);
        renderer->AddActor(innerModelActor);

        updateSurfaceCellColors();
        updateAutoOpenBoxWalls();
        updateClippingPlane();
        refreshFaceListUI();
        renderWindow->Render();
        return totalTriangles;
    }

    void updateAutoOpenBoxWalls() {
        if (boxWallActors.empty() || !renderer || !renderer->GetActiveCamera()) return;
        double camPos[3];
        renderer->GetActiveCamera()->GetPosition(camPos);

        bool autoOpen = autoOpenBoxCheck->isChecked();
        for (auto& kv : boxWallActors) {
            BoxWallActorData& w = kv.second;
            if (!w.actor) continue;

            if (!autoOpen) {
                w.actor->SetVisibility(1);
                w.actor->SetPickable(1);
                continue;
            }

            double vCam[3] = {
                camPos[0] - w.center[0],
                camPos[1] - w.center[1],
                camPos[2] - w.center[2]
            };
            double dot = vCam[0]*w.outwardNormal[0] + vCam[1]*w.outwardNormal[1] + vCam[2]*w.outwardNormal[2];

            bool isBlockingCamera = (dot > 0.0);
            w.actor->SetVisibility(isBlockingCamera ? 0 : 1);
            w.actor->SetPickable(isBlockingCamera ? 0 : 1);
        }
    }

    void buildSharpCADFeatureCurvesFast() {
        if (cadFeatureEdgeActor) { renderer->RemoveActor(cadFeatureEdgeActor); cadFeatureEdgeActor = nullptr; }

        std::vector<std::size_t> allNodeTags;
        std::vector<double> allCoords, allParams;
        gmsh::model::mesh::getNodes(allNodeTags, allCoords, allParams);
        if (allNodeTags.empty()) return;

        vtkNew<vtkPoints> curvePts;
        curvePts->SetNumberOfPoints(allNodeTags.size());
        std::unordered_map<std::size_t, vtkIdType> nodeMap;
        nodeMap.reserve(allNodeTags.size());
        for (size_t i = 0; i < allNodeTags.size(); ++i) {
            curvePts->SetPoint(i, allCoords[3*i], allCoords[3*i+1], allCoords[3*i+2]);
            nodeMap[allNodeTags[i]] = static_cast<vtkIdType>(i);
        }

        std::vector<int> eTypes1D;
        std::vector<std::vector<std::size_t>> eTags1D, eNodes1D;
        gmsh::model::mesh::getElements(eTypes1D, eTags1D, eNodes1D, 1, -1);

        vtkNew<vtkCellArray> edgeLines;
        for (size_t t = 0; t < eTypes1D.size(); ++t) {
            if (eTypes1D[t] == 1) {
                const auto& ns = eNodes1D[t];
                for (size_t j = 0; j < ns.size(); j += 2) {
                    auto it0 = nodeMap.find(ns[j]);
                    auto it1 = nodeMap.find(ns[j+1]);
                    if (it0 != nodeMap.end() && it1 != nodeMap.end()) {
                        vtkIdType seg[2] = {it0->second, it1->second};
                        edgeLines->InsertNextCell(2, seg);
                    }
                }
            }
        }

        vtkNew<vtkPolyData> curvePoly;
        curvePoly->SetPoints(curvePts);
        curvePoly->SetLines(edgeLines);

        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(curvePoly);
        if (clipEnableCheck->isChecked()) mapper->AddClippingPlane(clipPlane);

        cadFeatureEdgeActor = vtkSmartPointer<vtkActor>::New();
        cadFeatureEdgeActor->SetMapper(mapper);
        cadFeatureEdgeActor->GetProperty()->SetColor(0.05, 0.12, 0.20);
        cadFeatureEdgeActor->GetProperty()->SetLineWidth(1.4);
        cadFeatureEdgeActor->SetPickable(false);
        renderer->AddActor(cadFeatureEdgeActor);
    }

    void updateSurfaceCellColors() {
        if (innerModelPoly) {
            vtkNew<vtkUnsignedCharArray> innerColors;
            innerColors->SetNumberOfComponents(3);
            innerColors->SetName("Colors");
            innerColors->SetNumberOfTuples(innerCellToTag.size());

            bool showPaintOnSurface = interactorStyle->paintMode;

            for (size_t i = 0; i < innerCellToTag.size(); ++i) {
                vtkIdType cid = static_cast<vtkIdType>(i);
                int tag = innerCellToTag[i];
                unsigned char c[3];

                if (showPaintOnSurface && hoverInnerCells.count(cid)) {
                    c[0] = 16; c[1] = 235; c[2] = 150;
                } else if (showPaintOnSurface && paintedInnerCells.count(cid)) {
                    c[0] = 249; c[1] = 115; c[2] = 22;
                } else {
                    getColorForPatchName(tagToPatchName[tag], tag, c);
                }
                innerColors->SetTypedTuple(i, c);
            }
            innerModelPoly->GetCellData()->SetScalars(innerColors);
            innerModelPoly->Modified();
        }

        for (auto& kv : boxWallActors) {
            int tag = kv.first;
            BoxWallActorData& w = kv.second;
            if (!w.actor) continue;
            unsigned char c[3];
            getColorForPatchName(tagToPatchName[tag], tag, c);
            w.actor->GetProperty()->SetColor(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
        }
    }

    // ================= TAB 3: FOOLPROOF 6-WALL WIND TUNNEL BOX =================
    void updateEnclosurePreview() {
        if (!hasLoadedModel) return;

        if (!enclosurePreviewSource) {
            enclosurePreviewSource = vtkSmartPointer<vtkCubeSource>::New();
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputConnection(enclosurePreviewSource->GetOutputPort());

            enclosurePreviewActor = vtkSmartPointer<vtkActor>::New();
            enclosurePreviewActor->SetMapper(mapper);
            enclosurePreviewActor->GetProperty()->SetColor(0.06, 0.73, 0.50);
            enclosurePreviewActor->GetProperty()->SetOpacity(0.22);
            enclosurePreviewActor->GetProperty()->SetEdgeVisibility(1);
            enclosurePreviewActor->GetProperty()->SetEdgeColor(0.20, 0.85, 0.55);
            enclosurePreviewActor->GetProperty()->SetLineWidth(1.5);
            enclosurePreviewActor->SetPickable(false);
            renderer->AddActor(enclosurePreviewActor);
        }

        double xMin = carOnlyBounds[0] - padNegX->value();
        double xMax = carOnlyBounds[1] + padPosX->value();
        double yMin = carOnlyBounds[2] - padNegY->value();
        double yMax = carOnlyBounds[3] + padPosY->value();
        double zMin = carOnlyBounds[4] - padNegZ->value();
        double zMax = carOnlyBounds[5] + padPosZ->value();

        enclosurePreviewSource->SetBounds(xMin, xMax, yMin, yMax, zMin, zMax);
        enclosurePreviewActor->SetVisibility(previewBoxCheck->isChecked() ? 1 : 0);
        renderWindow->Render();
    }

    void onCreateFluidEnclosure() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);

        try {
            setProgressState(20, "Building Wind Tunnel Domain...", "Creating 6-wall fluid enclosure around model...");

            double xMin = carOnlyBounds[0] - padNegX->value();
            double xMax = carOnlyBounds[1] + padPosX->value();
            double yMin = carOnlyBounds[2] - padNegY->value();
            double yMax = carOnlyBounds[3] + padPosY->value();
            double zMin = carOnlyBounds[4] - padNegZ->value();
            double zMax = carOnlyBounds[5] + padPosZ->value();

            gmsh::vectorpair carSurfacesBeforeBox;
            gmsh::model::getEntities(carSurfacesBeforeBox, 2);
            std::set<int> preBoxSurfaceSet;
            for (const auto& s : carSurfacesBeforeBox) preBoxSurfaceSet.insert(s.second);

            gmsh::vectorpair modelVols;
            gmsh::model::getEntities(modelVols, 3);

            int boxTag = gmsh::model::occ::addBox(xMin, yMin, zMin, xMax - xMin, yMax - yMin, zMax - zMin);

            bool booleanCutSucceeded = false;
            if (!modelVols.empty() && carSurfacesBeforeBox.size() <= 250) {
                setProgressState(45, "Running 3D Boolean Subtraction...", "Subtracting solid model from wind tunnel box...");
                try {
                    gmsh::vectorpair outDimTags;
                    std::vector<gmsh::vectorpair> outMap;
                    gmsh::model::occ::cut({{3, boxTag}}, modelVols, outDimTags, outMap);
                    gmsh::model::occ::synchronize();
                    booleanCutSucceeded = true;
                } catch (...) {}
            }

            if (!booleanCutSucceeded) {
                gmsh::model::occ::synchronize();
                std::vector<int> innerSurfTags;
                for (const auto& s : carSurfacesBeforeBox) innerSurfTags.push_back(s.second);
                try {
                    gmsh::model::mesh::embed(2, innerSurfTags, 3, boxTag);
                } catch (...) {}
            }

            outerBoxSurfaceTags.clear();
            boxWallRoleToTag.clear();

            gmsh::vectorpair allSurfaces;
            gmsh::model::getEntities(allSurfaces, 2);

            struct CandidateWall { int tag; double mx, my, mz; };
            std::vector<CandidateWall> newWalls;
            for (const auto& s : allSurfaces) {
                int tag = s.second;
                if (!preBoxSurfaceSet.count(tag)) {
                    double sx0, sy0, sz0, sx1, sy1, sz1;
                    gmsh::model::getBoundingBox(2, tag, sx0, sy0, sz0, sx1, sy1, sz1);
                    newWalls.push_back({tag, 0.5*(sx0+sx1), 0.5*(sy0+sy1), 0.5*(sz0+sz1)});
                } else {
                    tagToPatchName[tag] = "obstacle";
                }
            }

            if (newWalls.size() >= 6) {
                auto minXIt = std::min_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.mx < b.mx; });
                auto maxXIt = std::max_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.mx < b.mx; });
                auto minYIt = std::min_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.my < b.my; });
                auto maxYIt = std::max_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.my < b.my; });
                auto minZIt = std::min_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.mz < b.mz; });
                auto maxZIt = std::max_element(newWalls.begin(), newWalls.end(), [](const auto& a, const auto& b){ return a.mz < b.mz; });

                boxWallRoleToTag["-X"] = minXIt->tag;
                boxWallRoleToTag["+X"] = maxXIt->tag;
                boxWallRoleToTag["-Y"] = minYIt->tag;
                boxWallRoleToTag["+Y"] = maxYIt->tag;
                boxWallRoleToTag["-Z"] = minZIt->tag;
                boxWallRoleToTag["+Z"] = maxZIt->tag;

                for (const auto& rw : boxWallRoleToTag) {
                    outerBoxSurfaceTags.insert(rw.second);
                }
            }

            hasEnclosureBox = true;
            previewBoxCheck->setChecked(false);
            apply6WallDropdownsToBox();

            generateHighPrecisionCADDisplay();
            renderer->ResetCamera();
            updateAutoOpenBoxWalls();
            renderWindow->Render();

            setProgressState(100, "6-Wall Shaded Fluid Box Created!",
                "Front walls auto-open so you can see inside! Click any visible wall in 3D or use the 1-Click Wall Buttons on the right.");
        } catch (const std::exception& e) {
            setProgressState(0, "Enclosure Error", QString::fromStdString(e.what()));
        }
    }

    void apply6WallDropdownsToBox() {
        if (!hasEnclosureBox) return;
        if (boxWallRoleToTag.count("-X")) tagToPatchName[boxWallRoleToTag["-X"]] = wallFrontNegXCombo->currentText().toStdString();
        if (boxWallRoleToTag.count("+X")) tagToPatchName[boxWallRoleToTag["+X"]] = wallRearPosXCombo->currentText().toStdString();
        if (boxWallRoleToTag.count("-Y")) tagToPatchName[boxWallRoleToTag["-Y"]] = wallLeftNegYCombo->currentText().toStdString();
        if (boxWallRoleToTag.count("+Y")) tagToPatchName[boxWallRoleToTag["+Y"]] = wallRightPosYCombo->currentText().toStdString();
        if (boxWallRoleToTag.count("-Z")) tagToPatchName[boxWallRoleToTag["-Z"]] = wallFloorNegZCombo->currentText().toStdString();
        if (boxWallRoleToTag.count("+Z")) tagToPatchName[boxWallRoleToTag["+Z"]] = wallRoofPosZCombo->currentText().toStdString();

        updateSurfaceCellColors();
        refreshFaceListUI();
        renderWindow->Render();
    }

    // ================= TAB 5: DUAL FVM (ORDER 1) & FEM (ORDER 2 QUADRATIC) MESHING =================
    void generateCFDViewMesh(int targetDim) {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);

        if (interactorStyle->paintMode) {
            interactorStyle->paintMode = false;
            paintModeBtn->setChecked(false);
            selectModeBtn->setChecked(true);
            hoverInnerCells.clear();
        }

        auto tStart = std::chrono::high_resolution_clock::now();
        diskCache.clearDiskCache();

        int targetOrder = elemOrderCombo->currentData().toInt();
        setProgressState(10,
            QString("Configuring %1 Meshing Pipeline (Order %2)...").arg(activeProfile.discretizationMethod).arg(targetOrder),
            QString("Applying %1 remembered surface-paint zones and sizing parameters...").arg(refinementStrokes.size()));

        gmsh::model::mesh::clear();
        gmsh::option::setNumber("Mesh.ElementOrder", 1); // Always generate linear base mesh first, then elevate to Order 2 if FEM!
        gmsh::option::setNumber("Mesh.Algorithm", algo2DCombo->currentData().toInt());
        gmsh::option::setNumber("Mesh.Algorithm3D", algo3DCombo->currentData().toInt());
        gmsh::option::setNumber("Mesh.MeshSizeMin", minSizeSpin->value());
        gmsh::option::setNumber("Mesh.MeshSizeMax", maxSizeSpin->value());
        gmsh::option::setNumber("Mesh.MeshSizeFromCurvature", curvatureSpin->value());
        gmsh::option::setNumber("Mesh.Smoothing", smoothingSpin->value());

        std::vector<int> existingFields;
        gmsh::model::mesh::field::list(existingFields);
        for (int f : existingFields) gmsh::model::mesh::field::remove(f);

        std::vector<double> activeFields;

        if (enableBlCheck->isChecked() && (carOnlyBounds[1] > carOnlyBounds[0])) {
            double pad = blFirstHeightSpin->value() * blLayersSpin->value() * blRatioSpin->value() * 2.0;
            int boxField = gmsh::model::mesh::field::add("Box");
            gmsh::model::mesh::field::setNumber(boxField, "VIn", std::max(minSizeSpin->value(), blFirstHeightSpin->value() * 2.5));
            gmsh::model::mesh::field::setNumber(boxField, "VOut", maxSizeSpin->value());
            gmsh::model::mesh::field::setNumber(boxField, "XMin", carOnlyBounds[0] - pad);
            gmsh::model::mesh::field::setNumber(boxField, "XMax", carOnlyBounds[1] + pad * 2.5);
            gmsh::model::mesh::field::setNumber(boxField, "YMin", carOnlyBounds[2] - pad);
            gmsh::model::mesh::field::setNumber(boxField, "YMax", carOnlyBounds[3] + pad);
            gmsh::model::mesh::field::setNumber(boxField, "ZMin", carOnlyBounds[4] - pad);
            gmsh::model::mesh::field::setNumber(boxField, "ZMax", carOnlyBounds[5] + pad);
            gmsh::model::mesh::field::setNumber(boxField, "Thickness", pad * 2.0);
            activeFields.push_back(boxField);
        }

        for (const auto& stroke : refinementStrokes) {
            int fTag = gmsh::model::mesh::field::add("Ball");
            gmsh::model::mesh::field::setNumber(fTag, "XCenter", stroke.x);
            gmsh::model::mesh::field::setNumber(fTag, "YCenter", stroke.y);
            gmsh::model::mesh::field::setNumber(fTag, "ZCenter", stroke.z);
            gmsh::model::mesh::field::setNumber(fTag, "Radius", stroke.radius);
            gmsh::model::mesh::field::setNumber(fTag, "VIn", stroke.targetSize);
            gmsh::model::mesh::field::setNumber(fTag, "VOut", maxSizeSpin->value());
            gmsh::model::mesh::field::setNumber(fTag, "Thickness", stroke.radius * 0.5);
            activeFields.push_back(fTag);
        }

        if (!activeFields.empty()) {
            int minField = gmsh::model::mesh::field::add("Min");
            gmsh::model::mesh::field::setNumbers(minField, "FieldsList", activeFields);
            gmsh::model::mesh::field::setAsBackgroundMesh(minField);
        }

        try {
            setProgressState(25, "Step 1/4: Discretizing 1D CAD Curves...", "Placing nodes on CAD edges...");
            gmsh::model::mesh::generate(1);

            setProgressState(50, "Step 2/4: Generating 2D Surface Mesh...",
                QString("Triangulating surfaces with %1 painted refinement zones...").arg(refinementStrokes.size()));
            try {
                gmsh::model::mesh::generate(2);
            } catch (...) {
                gmsh::option::setNumber("Mesh.Algorithm", 1);
                gmsh::model::mesh::generate(2);
            }

            if (targetDim == 3) {
                gmsh::vectorpair vols;
                gmsh::model::getEntities(vols, 3);
                if (!vols.empty()) {
                    setProgressState(70, "Step 3/4: Generating 3D Volumetric Tetrahedra...", "Filling 3D volume...");
                    try {
                        gmsh::model::mesh::generate(3);
                    } catch (...) {
                        gmsh::option::setNumber("Mesh.Algorithm3D", 1);
                        try { gmsh::model::mesh::generate(3); } catch (...) {}
                    }
                    if (optimizeCheck->isChecked()) {
                        setProgressState(80, "Smoothing Linear Base Mesh...", "Running thread-safe 3D vertex relocation...");
                        try { gmsh::model::mesh::optimize("Relocate3D"); } catch (...) {}
                    }
                }
            }

            // FEM Quadratic Order-2 Elevation (Tet10 / Tri6) + High-Order Curved Jacobian Optimization!
            if (targetOrder == 2) {
                setProgressState(86, "Step 4/4: Elevating to 2nd-Order Quadratic FEM Elements (Tet10 / Tri6)...",
                    "Inserting mid-edge nodes on NURBS surfaces and optimizing curved Jacobians...");
                gmsh::model::mesh::setOrder(2);
                if (highOrderCurveOptCheck->isChecked()) {
                    try {
                        gmsh::model::mesh::optimize("HighOrderFastCurving");
                    } catch (...) {}
                }
            }
            lastGeneratedOrder = targetOrder;
        } catch (const std::exception& e) {
            appendDebugLog(QString("Meshing notice: ") + e.what());
        }

        isMeshMode = true;
        showWireframe = true;
        size_t totalTris = buildSceneActorsFromCurrentMesh(true);

        size_t totalTets = 0;
        double ramPeakMB = OutOfCoreMeshCache::getCurrentRamUsageMB();
        if (targetDim == 3 && autoOffloadSsdCheck->isChecked()) {
            setProgressState(94, "Offloading 3D Volume Mesh to SSD Cache & Freeing RAM...",
                "Streaming nodes.bin & tets.bin to disk and calling malloc_zone_pressure_relief()...");
            diskCache.offload3DMeshToDiskAndFreeRAM(true);
            totalTets = diskCache.numCachedTets;
        }
        double ramAfterFlushMB = OutOfCoreMeshCache::getCurrentRamUsageMB();

        syncPhysicalGroupsToGmsh();
        updateClippingPlane();
        onScanMeshQuality();

        auto tEnd = std::chrono::high_resolution_clock::now();
        double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();

        QString elemTypeStr = (lastGeneratedOrder == 2) ? "Quadratic (Tri6 / Tet10 — FEM)" : "Linear (Tri3 / Tet4 — FVM)";
        setProgressState(100,
            QString("%1 Mesh Ready (%2 s) — %3 Tris | %4 Tets").arg(elemTypeStr).arg(elapsedSec, 0, 'f', 2).arg(totalTris).arg(totalTets),
            QString("Offloaded to SSD (%1 MB). RAM dropped from %2 MB ➔ %3 MB!")
                .arg(diskCache.cachedDiskMB, 0, 'f', 1).arg(ramPeakMB, 0, 'f', 0).arg(ramAfterFlushMB, 0, 'f', 0));
    }

    // ================= TAB 4: TURNING / CORNERING & YAW AERODYNAMICS =================
    void onRotateCADModel() {
        if (!hasLoadedModel || isBusy) return;
        if (hasEnclosureBox) {
            setProgressState(0, "Rotate Before Creating Box", "Rotate the CAD model's Yaw/Pitch/Roll BEFORE creating the fluid box.");
            return;
        }
        ScopedBusyLock lock(isBusy);

        double yawRad   = modelYawSpin->value() * M_PI / 180.0;
        double pitchRad = modelPitchSpin->value() * M_PI / 180.0;
        double rollRad  = modelRollSpin->value() * M_PI / 180.0;

        double cx = 0.5 * (carOnlyBounds[0] + carOnlyBounds[1]);
        double cy = 0.5 * (carOnlyBounds[2] + carOnlyBounds[3]);
        double cz = 0.5 * (carOnlyBounds[4] + carOnlyBounds[5]);

        try {
            gmsh::vectorpair entities;
            gmsh::model::getEntities(entities, 3);
            if (entities.empty()) gmsh::model::getEntities(entities, 2);

            if (std::abs(yawRad) > 1e-6)   gmsh::model::occ::rotate(entities, cx, cy, cz, 0, 0, 1, yawRad);
            if (std::abs(pitchRad) > 1e-6) gmsh::model::occ::rotate(entities, cx, cy, cz, 0, 1, 0, pitchRad);
            if (std::abs(rollRad) > 1e-6)  gmsh::model::occ::rotate(entities, cx, cy, cz, 1, 0, 0, rollRad);

            gmsh::model::occ::synchronize();
            gmsh::model::getBoundingBox(-1, -1,
                carOnlyBounds[0], carOnlyBounds[2], carOnlyBounds[4],
                carOnlyBounds[1], carOnlyBounds[3], carOnlyBounds[5]);

            generateHighPrecisionCADDisplay();
            updateEnclosurePreview();
            setProgressState(100, "Model Attitude Rotated", "CAD geometry rotated in 3D space.");
        } catch (const std::exception& e) {
            setProgressState(0, "Rotation Error", QString::fromStdString(e.what()));
        }
    }

    void onAutoAssignWallsForTurningFlow() {
        double beta = flowYawAngleSpin->value();
        double Rturn = turningRadiusSpin->value();

        wallFrontNegXCombo->setCurrentText("inlet");
        wallRearPosXCombo->setCurrentText("outlet");
        wallFloorNegZCombo->setCurrentText("moving_ground");
        wallRoofPosZCombo->setCurrentText("slip_wall");

        if (beta > 1.0 || Rturn > 0.5) {
            wallLeftNegYCombo->setCurrentText("inlet");
            wallRightPosYCombo->setCurrentText("outlet");
        } else if (beta < -1.0) {
            wallRightPosYCombo->setCurrentText("inlet");
            wallLeftNegYCombo->setCurrentText("outlet");
        } else {
            wallLeftNegYCombo->setCurrentText("slip_wall");
            wallRightPosYCombo->setCurrentText("slip_wall");
        }
        apply6WallDropdownsToBox();
        setProgressState(100, "Box Walls Configured for Turning / Yaw Flow",
            "Front & upstream side wall set to 'inlet', rear & downstream side wall set to 'outlet', floor set to 'moving_ground'.");
    }

    // ================= TAB 2: SAFE O(N) SCANNER & REPAIR =================
    void onScanCADProblems() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);

        problemListWidget->clear();
        detectedFreeCurveTags.clear();
        detectedSliverSurfaceTags.clear();

        gmsh::vectorpair surfaces, volumes;
        gmsh::model::getEntities(surfaces, 2);
        gmsh::model::getEntities(volumes, 3);

        if (volumes.empty()) {
            QListWidgetItem* item = new QListWidgetItem("[SHELL WARNING] 0 Solid Volumes (Surface Assembly)");
            item->setForeground(QColor("#f97316"));
            problemListWidget->addItem(item);
        } else {
            QListWidgetItem* item = new QListWidgetItem(QString("[OK] %1 Closed 3D Solid Volume(s)").arg(volumes.size()));
            item->setForeground(QColor("#10b981"));
            problemListWidget->addItem(item);
        }

        std::unordered_map<int, int> curveIncidenceCount;
        for (const auto& s : surfaces) {
            gmsh::vectorpair bnd;
            try {
                gmsh::model::getBoundary({s}, bnd, false, false, false);
                std::set<int> uniqueInFace;
                for (const auto& c : bnd) uniqueInFace.insert(std::abs(c.second));
                for (int cTag : uniqueInFace) curveIncidenceCount[cTag]++;
            } catch (...) {}
        }

        for (const auto& kv : curveIncidenceCount) {
            if (kv.second == 1) detectedFreeCurveTags.push_back(kv.first);
        }

        double globalDiag = std::hypot(modelBounds[1] - modelBounds[0],
                            std::hypot(modelBounds[3] - modelBounds[2], modelBounds[5] - modelBounds[4]));
        double sliverThreshold = globalDiag * (sliverRatioSpin->value() / 100.0);

        for (const auto& s : surfaces) {
            int sTag = s.second;
            double x0, y0, z0, x1, y1, z1;
            try {
                gmsh::model::getBoundingBox(2, sTag, x0, y0, z0, x1, y1, z1);
                double faceDiag = std::hypot(x1 - x0, std::hypot(y1 - y0, z1 - z0));
                if (faceDiag < sliverThreshold) detectedSliverSurfaceTags.push_back(sTag);
            } catch (...) {}
        }

        if (detectedFreeCurveTags.empty()) {
            QListWidgetItem* item = new QListWidgetItem("[OK] Watertight Topology (0 Leak Edges)");
            item->setForeground(QColor("#10b981"));
            problemListWidget->addItem(item);
        } else {
            QListWidgetItem* item = new QListWidgetItem(
                QString("[LEAK] %1 Open Edges Detected").arg(detectedFreeCurveTags.size()));
            item->setForeground(QColor("#ef4444"));
            problemListWidget->addItem(item);
        }

        if (detectedSliverSurfaceTags.empty()) {
            QListWidgetItem* item = new QListWidgetItem("[OK] 0 Sliver Faces Detected");
            item->setForeground(QColor("#10b981"));
            problemListWidget->addItem(item);
        } else {
            int shown = 0;
            for (int sTag : detectedSliverSurfaceTags) {
                if (++shown > 80) break;
                QListWidgetItem* item = new QListWidgetItem(QString("[SLIVER] Face #%1 — Click to Highlight").arg(sTag));
                item->setForeground(QColor("#fb923c"));
                item->setData(Qt::UserRole, sTag);
                problemListWidget->addItem(item);
            }
        }

        setProgressState(100,
            QString("Scan Complete: %1 Leaks | %2 Slivers | %3 Volumes")
                .arg(detectedFreeCurveTags.size()).arg(detectedSliverSurfaceTags.size()).arg(volumes.size()),
            "Use 'AUTO-SEW & HEAL GAPS', 'CAP / PATCH OPEN HOLES', or 'REMOVE SLIVER FACES' in Tab 02.");
    }

    void onRemoveSliverFaces() {
        if (!hasLoadedModel || isBusy) return;
        if (detectedSliverSurfaceTags.empty()) onScanCADProblems();
        if (detectedSliverSurfaceTags.empty()) return;

        ScopedBusyLock lock(isBusy);
        try {
            gmsh::vectorpair toRemove;
            for (int sTag : detectedSliverSurfaceTags) {
                toRemove.push_back({2, sTag});
                tagToPatchName.erase(sTag);
            }
            gmsh::model::occ::remove(toRemove, false);
            gmsh::model::occ::synchronize();
            generateHighPrecisionCADDisplay();
            setProgressState(100, "Sliver Faces Removed", QString("Cleaned %1 sliver faces.").arg(toRemove.size()));
        } catch (const std::exception& e) {
            setProgressState(0, "Sliver Removal Notice", QString::fromStdString(e.what()));
        }
    }

    void onHealCAD() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);
        try {
            double tol = healTolSpin->value();
            gmsh::vectorpair outShapes;
            gmsh::model::occ::healShapes(outShapes, {}, tol, true, true, true, true, true);
            gmsh::model::occ::removeAllDuplicates();
            gmsh::model::occ::synchronize();
            generateHighPrecisionCADDisplay();
            setProgressState(100, "CAD Healing Complete", QString("Healed & sewed CAD topology (Tol = %1).").arg(tol));
        } catch (const std::exception& e) {
            setProgressState(0, "Heal Error", QString::fromStdString(e.what()));
        }
    }

    void onCapHoles() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);
        try {
            gmsh::vectorpair outShapes;
            gmsh::model::occ::healShapes(outShapes, {}, healTolSpin->value(), true, true, true, true, true);
            gmsh::model::occ::synchronize();

            gmsh::vectorpair volumes;
            gmsh::model::getEntities(volumes, 3);
            if (volumes.empty()) {
                gmsh::vectorpair surfaces;
                gmsh::model::getEntities(surfaces, 2);
                std::vector<int> sTags;
                for (const auto& s : surfaces) sTags.push_back(s.second);
                if (!sTags.empty() && sTags.size() < 2000) {
                    try {
                        int sl = gmsh::model::occ::addSurfaceLoop(sTags);
                        gmsh::model::occ::addVolume({sl});
                        gmsh::model::occ::synchronize();
                    } catch (...) {}
                }
            }
            generateHighPrecisionCADDisplay();
            gmsh::model::getEntities(volumes, 3);
            setProgressState(100, "Cap & Solidify Complete", QString("Model now contains %1 solid volume(s).").arg(volumes.size()));
        } catch (const std::exception& e) {
            setProgressState(0, "Cap Holes Notice", QString::fromStdString(e.what()));
        }
    }

    void onDeleteSelectedFaces() {
        if (!hasLoadedModel || selectedTags.empty() || isBusy) return;
        ScopedBusyLock lock(isBusy);
        try {
            gmsh::vectorpair toRemove;
            for (int tag : selectedTags) {
                toRemove.push_back({2, tag});
                tagToPatchName.erase(tag);
                outerBoxSurfaceTags.erase(tag);
            }
            gmsh::model::occ::remove(toRemove, false);
            gmsh::model::occ::synchronize();
            selectedTags.clear();
            generateHighPrecisionCADDisplay();
            setProgressState(100, "Faces Deleted", QString("Removed %1 face(s).").arg(toRemove.size()));
        } catch (const std::exception& e) {
            setProgressState(0, "Delete Error", QString::fromStdString(e.what()));
        }
    }

    void onFragmentAllVolumes() {
        if (!hasLoadedModel || isBusy) return;
        ScopedBusyLock lock(isBusy);
        try {
            gmsh::vectorpair vols;
            gmsh::model::getEntities(vols, 3);
            if (vols.size() >= 2) {
                gmsh::vectorpair outDimTags;
                std::vector<gmsh::vectorpair> outMap;
                gmsh::vectorpair first = {vols[0]};
                gmsh::vectorpair rest(vols.begin() + 1, vols.end());
                gmsh::model::occ::fragment(first, rest, outDimTags, outMap);
                gmsh::model::occ::removeAllDuplicates();
                gmsh::model::occ::synchronize();
            }
            generateHighPrecisionCADDisplay();
            setProgressState(100, "Boolean Fuse Complete", "Conformal CAD topology synchronized.");
        } catch (const std::exception& e) {
            setProgressState(0, "Fragment Error", QString::fromStdString(e.what()));
        }
    }

    // Computes both FVM Skewness and FEM Aspect Ratio / Scaled Jacobian Quality!
    void onScanMeshQuality() {
        if (!innerModelPoly) return;
        problemListWidget->clear();

        double minQ = 1.0, sumQ = 0.0, maxAR = 1.0;
        size_t badCells = 0, totalCells = 0;

        auto scanPoly = [&](vtkPolyData* poly) {
            if (!poly || !poly->GetPoints()) return;
            vtkPoints* pts = poly->GetPoints();
            vtkCellArray* polys = poly->GetPolys();
            vtkIdType npts;
            const vtkIdType* ptIds;
            polys->InitTraversal();
            while (polys->GetNextCell(npts, ptIds)) {
                if (npts == 3) {
                    double p0[3], p1[3], p2[3];
                    pts->GetPoint(ptIds[0], p0);
                    pts->GetPoint(ptIds[1], p1);
                    pts->GetPoint(ptIds[2], p2);

                    double a = std::hypot(p1[0]-p0[0], std::hypot(p1[1]-p0[1], p1[2]-p0[2]));
                    double b = std::hypot(p2[0]-p1[0], std::hypot(p2[1]-p1[1], p2[2]-p1[2]));
                    double c = std::hypot(p0[0]-p2[0], std::hypot(p0[1]-p2[1], p0[2]-p2[2]));
                    double s = 0.5 * (a + b + c);
                    double area = std::sqrt(std::max(0.0, s * (s - a) * (s - b) * (s - c)));
                    double denom = a*a + b*b + c*c;
                    double q = (denom > 1e-12) ? (6.92820323 * area / denom) : 0.0;

                    double maxEdge = std::max({a, b, c});
                    double minAlt  = (maxEdge > 1e-12) ? (2.0 * area / maxEdge) : 1e-12;
                    double ar      = (minAlt > 1e-12) ? (maxEdge / minAlt) : 999.0;

                    minQ = std::min(minQ, q);
                    maxAR = std::max(maxAR, ar);
                    sumQ += q;
                    if (q < 0.25) badCells++;
                    totalCells++;
                }
            }
        };

        scanPoly(innerModelPoly);
        for (const auto& kv : boxWallActors) scanPoly(kv.second.poly);

        double avgQ = (totalCells > 0) ? (sumQ / totalCells) : 0.0;
        double maxSkewness = 1.0 - minQ;

        qualitySummaryLabel->setText(QString("[%1 P%2] MIN Q:%3 | MAX SKEW:%4 | MAX AR:%5")
            .arg(activeProfile.discretizationMethod).arg(lastGeneratedOrder)
            .arg(minQ, 0, 'f', 2).arg(maxSkewness, 0, 'f', 2).arg(maxAR, 0, 'f', 1));

        problemListWidget->addItem(new QListWidgetItem(
            QString("[%1 P%2] Shape Quality Min=%3 | Avg=%4").arg(activeProfile.discretizationMethod).arg(lastGeneratedOrder).arg(minQ, 0, 'f', 3).arg(avgQ, 0, 'f', 3)));
        problemListWidget->addItem(new QListWidgetItem(
            QString("[FVM SKEWNESS] Max Skewness = %1 (%2 cells > 0.75)").arg(maxSkewness, 0, 'f', 3).arg(badCells)));
        problemListWidget->addItem(new QListWidgetItem(
            QString("[FEM JACOBIAN / AR] Max Aspect Ratio = %1").arg(maxAR, 0, 'f', 2)));
    }

    void syncPhysicalGroupsToGmsh() {
        gmsh::model::removePhysicalGroups();
        std::map<std::string, std::vector<int>> patchGroups;
        for (const auto& kv : tagToPatchName) {
            patchGroups[kv.second].push_back(kv.first);
        }

        int groupTag = 1;
        for (const auto& kv : patchGroups) {
            int pg = gmsh::model::addPhysicalGroup(2, kv.second, groupTag++);
            gmsh::model::setPhysicalName(2, pg, kv.first);
        }

        gmsh::vectorpair volumes;
        gmsh::model::getEntities(volumes, 3);
        if (!volumes.empty()) {
            std::vector<int> volTags;
            for (const auto& v : volumes) volTags.push_back(v.second);
            int vpg = gmsh::model::addPhysicalGroup(3, volTags, 100);
            std::string volName = (activeProfile.discretizationMethod == "FEM") ? "solid_domain" : "fluid";
            gmsh::model::setPhysicalName(3, vpg, volName);
        }
    }

    void updateClippingPlane() {
        int axis = clipAxisCombo->currentIndex();
        double sign = clipFlipCheck->isChecked() ? 1.0 : -1.0;
        double t = clipSlider->value() / 100.0;

        double cx = 0.5 * (modelBounds[0] + modelBounds[1]);
        double cy = 0.5 * (modelBounds[2] + modelBounds[3]);
        double cz = 0.5 * (modelBounds[4] + modelBounds[5]);

        if (axis == 0) {
            cx = modelBounds[0] + t * (modelBounds[1] - modelBounds[0]);
            clipPlane->SetNormal(sign, 0.0, 0.0);
        } else if (axis == 1) {
            cy = modelBounds[2] + t * (modelBounds[3] - modelBounds[2]);
            clipPlane->SetNormal(0.0, sign, 0.0);
        } else {
            cz = modelBounds[4] + t * (modelBounds[5] - modelBounds[4]);
            clipPlane->SetNormal(0.0, 0.0, sign);
        }
        clipPlane->SetOrigin(cx, cy, cz);

        bool clipOn = clipEnableCheck->isChecked();
        if (innerModelMapper) {
            innerModelMapper->RemoveAllClippingPlanes();
            if (clipOn) innerModelMapper->AddClippingPlane(clipPlane);
        }
        for (auto& kv : boxWallActors) {
            if (kv.second.mapper) {
                kv.second.mapper->RemoveAllClippingPlanes();
                if (clipOn) kv.second.mapper->AddClippingPlane(clipPlane);
            }
        }
        if (cadFeatureEdgeActor && cadFeatureEdgeActor->GetMapper()) {
            cadFeatureEdgeActor->GetMapper()->RemoveAllClippingPlanes();
            if (clipOn) cadFeatureEdgeActor->GetMapper()->AddClippingPlane(clipPlane);
        }

        bool showTets = clipOn && isMeshMode && show3DTetsCheck->isChecked() && diskCache.hasDiskCache;
        if (showTets) {
            auto sliceGrid = diskCache.extractSectionCutFromDisk(clipPlane);
            if (!volumeClipActor) {
                volumeClipMapper = vtkSmartPointer<vtkDataSetMapper>::New();
                volumeClipActor = vtkSmartPointer<vtkActor>::New();
                volumeClipActor->SetMapper(volumeClipMapper);
                volumeClipActor->GetProperty()->SetColor(0.16, 0.70, 0.95);
                volumeClipActor->GetProperty()->SetEdgeVisibility(1);
                volumeClipActor->GetProperty()->SetEdgeColor(0.04, 0.08, 0.12);
                volumeClipActor->SetPickable(false);
                renderer->AddActor(volumeClipActor);
            }
            volumeClipMapper->SetInputData(sliceGrid);
            volumeClipActor->SetVisibility(1);
        } else if (volumeClipActor) {
            volumeClipActor->SetVisibility(0);
        }

        renderWindow->Render();
    }

    void handleCellPicked(vtkActor* pickedActor, vtkIdType cellId, bool multiSelect) {
        if (!pickedActor || cellId < 0) return;

        for (const auto& kv : boxWallActors) {
            if (kv.second.actor == pickedActor) {
                selectSurfaceTag(kv.first, multiSelect);
                return;
            }
        }

        if (pickedActor == innerModelActor && static_cast<size_t>(cellId) < innerCellToTag.size()) {
            selectSurfaceTag(innerCellToTag[cellId], multiSelect);
        }
    }

    void selectSurfaceTag(int clickedTag, bool multiSelect) {
        if (!multiSelect) selectedTags.clear();
        if (selectedTags.count(clickedTag)) selectedTags.erase(clickedTag);
        else selectedTags.insert(clickedTag);

        updateSurfaceCellColors();
        QString roleNote = outerBoxSurfaceTags.count(clickedTag) ? "Wind Tunnel Box Wall" : "Inner Model Surface";
        statusLabel->setText(QString("Selected %1 #%2 (Current Role: '%3') — Click any FVM/FEM Role Button on the right!")
            .arg(roleNote).arg(clickedTag).arg(QString::fromStdString(tagToPatchName[clickedTag])));
        renderWindow->Render();
    }

    void assignPatchToSelected() {
        if (selectedTags.empty()) {
            setProgressState(0, "No Wall or Surface Selected",
                "Click a wall in the 3D view (or click one of the 6 Wall Buttons on the right) before assigning a role!");
            return;
        }
        std::string newPatch = patchNameInput->text().trimmed().toStdString();
        if (newPatch.empty()) return;

        for (int tag : selectedTags) {
            tagToPatchName[tag] = newPatch;
            for (const auto& rw : boxWallRoleToTag) {
                if (rw.second == tag) {
                    QString qRole = QString::fromStdString(newPatch);
                    if (rw.first == "-X") { wallFrontNegXCombo->blockSignals(true); wallFrontNegXCombo->setCurrentText(qRole); wallFrontNegXCombo->blockSignals(false); }
                    if (rw.first == "+X") { wallRearPosXCombo->blockSignals(true);  wallRearPosXCombo->setCurrentText(qRole);  wallRearPosXCombo->blockSignals(false); }
                    if (rw.first == "-Y") { wallLeftNegYCombo->blockSignals(true);  wallLeftNegYCombo->setCurrentText(qRole);  wallLeftNegYCombo->blockSignals(false); }
                    if (rw.first == "+Y") { wallRightPosYCombo->blockSignals(true); wallRightPosYCombo->setCurrentText(qRole); wallRightPosYCombo->blockSignals(false); }
                    if (rw.first == "-Z") { wallFloorNegZCombo->blockSignals(true); wallFloorNegZCombo->setCurrentText(qRole); wallFloorNegZCombo->blockSignals(false); }
                    if (rw.first == "+Z") { wallRoofPosZCombo->blockSignals(true);  wallRoofPosZCombo->setCurrentText(qRole);  wallRoofPosZCombo->blockSignals(false); }
                }
            }
        }
        syncPhysicalGroupsToGmsh();
        updateSurfaceCellColors();
        refreshFaceListUI();
        renderWindow->Render();
        setProgressState(100, QString("Assigned '%1' to %2 Surface(s)").arg(QString::fromStdString(newPatch)).arg(selectedTags.size()),
            "FVM/FEM boundary role and 3D color updated.");
    }

    void refreshFaceListUI() {
        faceListWidget->clear();
        int count = 0;
        for (int boxTag : outerBoxSurfaceTags) {
            QString wallLabel = "Box Wall";
            for (const auto& rw : boxWallRoleToTag) {
                if (rw.second == boxTag) wallLabel = QString("Box %1").arg(QString::fromStdString(rw.first));
            }
            QString label = QString("[%1] Face #%2 ➔ %3")
                .arg(wallLabel).arg(boxTag).arg(QString::fromStdString(tagToPatchName[boxTag]));
            QListWidgetItem* item = new QListWidgetItem(label, faceListWidget);
            item->setForeground(QColor("#34d399"));
            item->setData(Qt::UserRole, boxTag);
        }

        for (const auto& kv : tagToPatchName) {
            int tag = kv.first;
            if (outerBoxSurfaceTags.count(tag)) continue;
            if (++count > 400) {
                new QListWidgetItem(QString("... + %1 more inner faces").arg(tagToPatchName.size() - 400), faceListWidget);
                break;
            }
            QString label = QString("[Inner] Face #%1 ➔ %2").arg(tag).arg(QString::fromStdString(kv.second));
            QListWidgetItem* item = new QListWidgetItem(label, faceListWidget);
            item->setData(Qt::UserRole, tag);
        }
    }

    void toggleEdges() {
        showWireframe = !showWireframe;
        if (innerModelActor) innerModelActor->GetProperty()->SetEdgeVisibility(showWireframe ? 1 : 0);
        renderWindow->Render();
    }

    void generateDiagnosticReport() {
        appendDebugLog("================ SIMUOP LABS FVM & FEM DIAGNOSTIC REPORT ================");
        appendDebugLog(QString("Host CPU Architecture: %1").arg(QString::fromStdString(SimuopOS::getCpuArchitectureAndBrand())));
        appendDebugLog(QString("GPU Pipeline Mode: %1").arg(QString::fromStdString(activeProfile.gpuVendorMode)));
        appendDebugLog(QString("Discretization Engine: %1 | Active Element Order: P%2")
            .arg(activeProfile.discretizationMethod).arg(lastGeneratedOrder));
        appendDebugLog(QString("Live Process RAM: %1 MB / %2 MB Ceiling")
            .arg(OutOfCoreMeshCache::getCurrentRamUsageMB(), 0, 'f', 1).arg(activeProfile.ramLimitGb * 1024));
        appendDebugLog(QString("Out-of-Core SSD Cache: %1 Nodes | %2 Tets (%3 MB on disk)")
            .arg(diskCache.numCachedNodes).arg(diskCache.numCachedTets).arg(diskCache.cachedDiskMB, 0, 'f', 2));
        if (!hasLoadedModel) return;
        gmsh::vectorpair pts, curves, surfs, vols;
        gmsh::model::getEntities(pts, 0);
        gmsh::model::getEntities(curves, 1);
        gmsh::model::getEntities(surfs, 2);
        gmsh::model::getEntities(vols, 3);
        appendDebugLog(QString("B-Rep Topology: %1 Vertices | %2 Curves | %3 Surfaces | %4 Volumes")
            .arg(pts.size()).arg(curves.size()).arg(surfs.size()).arg(vols.size()));
        appendDebugLog("=========================================================================");
    }

    // Exports both FVM formats (.msh v2.2, .su2, .stl) and FEM formats (.inp with NSET/ELSET, .unv, .msh v4.1, .vtk)!
    void exportForSelectedSolver() {
        if (!hasLoadedModel || isBusy) return;

        QString solverKey = solverCombo->currentData().toString();
        if (!isMeshMode) {
            generateCFDViewMesh(solverKey == "baram_mesh" ? 2 : 3);
        }

        QString defaultName = "cfd_fvm_mesh.msh";
        QString filter = "Gmsh v2.2 ASCII (*.msh);;Abaqus / CalculiX FEM (*.inp);;I-DEAS Universal FVM/FEM (*.unv);;Gmsh v4.1 FEM (*.msh);;SU2 ASCII (*.su2);;VTK Legacy (*.vtk);;Multi-Solid ASCII STL (*.stl)";

        if (solverKey == "baram_mesh") {
            defaultName = "baram_surfaces.stl";
            filter = "Multi-Solid ASCII STL (*.stl)";
        } else if (solverKey == "ansys_fvm" || solverKey == "elmer_unv") {
            defaultName = (solverKey == "elmer_unv") ? "elmer_fem_mesh.unv" : "ansys_fvm_mesh.unv";
            filter = "I-DEAS Universal ASCII (*.unv);;Gmsh v2.2 ASCII (*.msh)";
        } else if (solverKey == "su2") {
            defaultName = "su2_fvm_mesh.su2";
            filter = "SU2 Native ASCII (*.su2)";
        } else if (solverKey == "calculix_inp") {
            defaultName = "calculix_abaqus_fem.inp";
            filter = "Abaqus / CalculiX FEM Input (*.inp)";
        } else if (solverKey == "fenics_msh4") {
            defaultName = "fenics_fem_mesh.msh";
            filter = "Gmsh v4.1 ASCII Mesh (*.msh)";
        } else if (solverKey == "fem_vtk") {
            defaultName = "fem_mesh.vtk";
            filter = "VTK Legacy ASCII Mesh (*.vtk)";
        }

        QString savePath = QFileDialog::getSaveFileName(this, "Export FVM / FEM Mesh", defaultName, filter);
        if (savePath.isEmpty()) return;

        ScopedBusyLock lock(isBusy);

        try {
            if (diskCache.hasDiskCache && !savePath.endsWith(".stl", Qt::CaseInsensitive)) {
                setProgressState(40, "Streaming 3D Volume from SSD Cache for Export...",
                    "Reloading cached 3D elements from disk and applying FVM/FEM Physical Groups...");
                gmsh::model::mesh::clear();
                gmsh::merge(diskCache.mshCachePath);
            }

            syncPhysicalGroupsToGmsh();

            gmsh::option::setNumber("Mesh.Binary", 0);
            gmsh::option::setNumber("Mesh.SaveAll", 0);

            if (savePath.endsWith(".inp", Qt::CaseInsensitive)) {
                // FEM Abaqus / CalculiX export: write both Node Sets (*NSET) and Element Sets (*ELSET)!
                gmsh::option::setNumber("Mesh.SaveGroupsOfNodes", 1);
                gmsh::option::setNumber("Mesh.SaveGroupsOfElements", 1);
            } else if (savePath.endsWith(".unv", Qt::CaseInsensitive)) {
                gmsh::option::setNumber("Mesh.SaveGroupsOfNodes", 1);
            } else if (savePath.endsWith(".msh", Qt::CaseInsensitive)) {
                gmsh::option::setNumber("Mesh.MshFileVersion", (solverKey == "fenics_msh4") ? 4.1 : 2.2);
            } else if (savePath.endsWith(".stl", Qt::CaseInsensitive)) {
                gmsh::option::setNumber("Mesh.StlOneSolidPerSurface", 2);
            }

            setProgressState(75, "Writing Solver ASCII File...", savePath);
            gmsh::write(savePath.toStdString());

            if (diskCache.hasDiskCache && autoOffloadSsdCheck->isChecked()) {
                gmsh::vectorpair volumes;
                gmsh::model::getEntities(volumes, 3);
                if (!volumes.empty()) gmsh::model::mesh::clear(volumes);
                OutOfCoreMeshCache::releaseFreeRamToMacOS();
            }

            double U = flowSpeedSpin->value();
            double betaRad = flowYawAngleSpin->value() * M_PI / 180.0;
            double Rturn = turningRadiusSpin->value();
            double ux = U * std::cos(betaRad);
            double uy = U * std::sin(betaRad);
            double omegaZ = (Rturn > 1e-3) ? (U / Rturn) : 0.0;
            double cx = 0.5 * (carOnlyBounds[0] + carOnlyBounds[1]);
            double cy = 0.5 * (carOnlyBounds[2] + carOnlyBounds[3]) + Rturn;

            QString condFile = savePath + "_solver_setup.txt";
            std::ofstream out(condFile.toStdString());
            out << "# SIMUOP LABS — FVM & FEM SOLVER SETUP SUMMARY\n";
            out << "Discretization_Method: " << activeProfile.discretizationMethod.toStdString() << "\n";
            out << "Element_Polynomial_Order: " << lastGeneratedOrder << " ("
                << (lastGeneratedOrder == 2 ? "Quadratic Tet10/Tri6" : "Linear Tet4/Tri3") << ")\n";
            out << "Freestream_Speed_m_s: " << U << "\n";
            out << "Sideslip_Yaw_Angle_deg: " << flowYawAngleSpin->value() << "\n";
            out << "Inlet_Velocity_Vector_Ux_Uy_Uz: (" << ux << " " << uy << " 0.0)\n";
            out << "Cornering_Turning_Radius_m: " << Rturn << "\n";
            out << "MRF_Rotation_Omega_Z_rad_s: " << omegaZ << "\n";
            out << "MRF_Turning_Center_XYZ: (" << cx << " " << cy << " 0.0)\n";
            out.close();

            setProgressState(100,
                QString("Exported %1 (Order %2) Mesh + Solver Setup").arg(activeProfile.discretizationMethod).arg(lastGeneratedOrder),
                QString("Saved '%1' and '%2'.").arg(savePath).arg(condFile));
        } catch (const std::exception& e) {
            setProgressState(0, "Export Error", QString::fromStdString(e.what()));
        }
    }
};

int main(int argc, char** argv) {
    qputenv("QSG_RHI_BACKEND", "opengl");
    qputenv("QT_VULKAN_LIB", "/opt/homebrew/lib/libMoltenVK.dylib");
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    SciFiLauncherDialog launcher;
    if (launcher.exec() != QDialog::Accepted) {
        return 0;
    }

    DiscoveryWindow window(launcher.profile);
    window.show();

    return app.exec();
}
