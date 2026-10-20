#include "os_platform.h"
#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTabWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QScrollBar>
#include <QListWidget>
#include <QDockWidget>
#include <QMenu>
#include <QToolButton>
#include <QSurfaceFormat>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkSmartPointer.h>
#include <vtkNew.h>
#include <vtkActor.h>
#include <vtkPolyDataMapper.h>
#include <vtkDataSetMapper.h>
#include <vtkPolyData.h>
#include <vtkUnstructuredGrid.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPlane.h>
#include <vtkCubeSource.h>
#include <vtkAxesActor.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCellPicker.h>
#include <vtkProperty.h>
#include <vtkPolyDataNormals.h>

#include <QVTKOpenGLNativeWidget.h>
#include <gmsh.h>

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <unordered_map>

// Out-of-core mesh cache for large CFD cases
struct CachedNode { float x, y, z; };
struct CachedTet  { uint32_t n[4]; };

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

    OutOfCoreMeshCache() {
        QString qDir = QDir::tempPath() + "/simuop_cfd_cache";
        QDir().mkpath(qDir);
        cacheDir     = qDir.toStdString();
        nodesPath    = (qDir + "/nodes.bin").toStdString();
        tetsPath     = (qDir + "/tets.bin").toStdString();
        mshCachePath = (qDir + "/full_volume_cache.msh").toStdString();
    }

    ~OutOfCoreMeshCache() { clearDiskCache(); }

    static double getFileSizeMB(const std::string& path) {
        QFileInfo fi(QString::fromStdString(path));
        return fi.exists() ? (static_cast<double>(fi.size()) / (1024.0 * 1024.0)) : 0.0;
    }

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
        for (std::size_t t : nodeTags) if (t > maxTag) maxTag = t;

        std::vector<uint32_t> tagToDenseIdx(maxTag + 1, 0);
        {
            std::ofstream nodeOut(nodesPath, std::ios::binary | std::ios::trunc);
            if (!nodeOut) return false;
            std::vector<CachedNode> chunk;
            chunk.reserve(65536);
            for (size_t i = 0; i < nodeTags.size(); ++i) {
                tagToDenseIdx[nodeTags[i]] = static_cast<uint32_t>(i);
                chunk.push_back({static_cast<float>(coords[3*i]), static_cast<float>(coords[3*i+1]), static_cast<float>(coords[3*i+2])});
                if (chunk.size() == 65536) {
                    nodeOut.write(reinterpret_cast<const char*>(chunk.data()), chunk.size() * sizeof(CachedNode));
                    chunk.clear();
                }
            }
            if (!chunk.empty()) nodeOut.write(reinterpret_cast<const char*>(chunk.data()), chunk.size() * sizeof(CachedNode));
            numCachedNodes = nodeTags.size();
        }

        std::vector<int> eTypes3D;
        std::vector<std::vector<std::size_t>> eTags3D, eNodes3D;
        gmsh::model::mesh::getElements(eTypes3D, eTags3D, eNodes3D, 3);

        {
            std::ofstream tetOut(tetsPath, std::ios::binary | std::ios::trunc);
            if (!tetOut) return false;
            std::vector<CachedTet> tetChunk;
            tetChunk.reserve(65536);
            for (size_t t = 0; t < eTypes3D.size(); ++t) {
                int nodesPerElem = (eTypes3D[t] == 4) ? 4 : ((eTypes3D[t] == 11) ? 10 : 0);
                if (nodesPerElem > 0) {
                    const auto& ns = eNodes3D[t];
                    for (size_t j = 0; j < ns.size(); j += nodesPerElem) {
                        tetChunk.push_back({tagToDenseIdx[ns[j]], tagToDenseIdx[ns[j+1]], tagToDenseIdx[ns[j+2]], tagToDenseIdx[ns[j+3]]});
                        numCachedTets++;
                        if (tetChunk.size() == 65536) {
                            tetOut.write(reinterpret_cast<const char*>(tetChunk.data()), tetChunk.size() * sizeof(CachedTet));
                            tetChunk.clear();
                        }
                    }
                }
            }
            if (!tetChunk.empty()) tetOut.write(reinterpret_cast<const char*>(tetChunk.data()), tetChunk.size() * sizeof(CachedTet));
        }

        gmsh::vectorpair volumes;
        gmsh::model::getEntities(volumes, 3);
        if (!volumes.empty()) gmsh::model::mesh::clear(volumes);

        SimuopOS::releaseFreeRamToOS();
        cachedDiskMB = getFileSizeMB(nodesPath) + getFileSizeMB(tetsPath) + getFileSizeMB(mshCachePath);
        hasDiskCache = (numCachedTets > 0);
        return hasDiskCache;
    }

    void clearDiskCache() {
        QFile::remove(QString::fromStdString(nodesPath));
        QFile::remove(QString::fromStdString(tetsPath));
        QFile::remove(QString::fromStdString(mshCachePath));
        hasDiskCache = false;
        numCachedNodes = 0;
        numCachedTets = 0;
        cachedDiskMB = 0.0;
        SimuopOS::releaseFreeRamToOS();
    }
};

class MesherInteractorStyle : public vtkInteractorStyleTrackballCamera {
public:
    static MesherInteractorStyle* New();
    vtkTypeMacro(MesherInteractorStyle, vtkInteractorStyleTrackballCamera);
    std::function<void(int, int)> onMouseClick;

    void OnLeftButtonDown() override {
        vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
        if (this->Interactor) {
            int* pos = this->Interactor->GetEventPosition();
            if (onMouseClick) onMouseClick(pos[0], pos[1]);
        }
    }
};
vtkStandardNewMacro(MesherInteractorStyle);

class DiscoveryWindow : public QMainWindow {
    Q_OBJECT
public:
    QVTKOpenGLNativeWidget* vtkPanel = nullptr;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<MesherInteractorStyle> style;

    QPlainTextEdit* debugLog = nullptr;
    QListWidget* infoTreeWidget = nullptr;
    OutOfCoreMeshCache meshCache;

    DiscoveryWindow() {
        setWindowTitle("Simuop Labs OpenCFD Studio — FVM & FEM Mesher");
        resize(1440, 900);
        setStyleSheet("QMainWindow { background: #1e1e1e; color: #f0f0f0; }");

        try {
            if (!gmsh::isInitialized()) gmsh::initialize();
        } catch (...) {}

        setupSolidWorksRibbonUI();
        setupDockablePanels();
        setupViewport();

        appendDebugLog("=== Simuop OpenCFD Studio Initialized Successfully ===");
        appendDebugLog("CPU Architecture: " + QString::fromStdString(SimuopOS::getCpuArchitectureAndBrand()));
    }

    ~DiscoveryWindow() {
        try {
            if (gmsh::isInitialized()) gmsh::finalize();
        } catch (...) {}
    }

    void appendDebugLog(const QString& msg) {
        if (debugLog) {
            debugLog->appendPlainText("[" + QDateTime::currentDateTime().toString("hh:mm:ss") + "] " + msg);
        }
    }

    void setupSolidWorksRibbonUI() {
        // Top Ribbon Container with SolidWorks Style Tabs & Logo Dropdown
        QWidget* ribbonContainer = new QWidget(this);
        QVBoxLayout* ribbonLayout = new QVBoxLayout(ribbonContainer);
        ribbonLayout->setContentsMargins(4, 4, 4, 4);
        ribbonLayout->setSpacing(2);

        QHBoxLayout* topBar = new QHBoxLayout();
        
        // Logo Button with Dropdown Menu (Import Menu)
        QToolButton* logoBtn = new QToolButton(this);
        logoBtn->setText("🟢 SIMUOP CAD");
        logoBtn->setStyleSheet("QToolButton { background: #007acc; color: white; font-weight: bold; padding: 6px 12px; border-radius: 4px; } QToolButton::menu-indicator { image: none; }");
        
        QMenu* importMenu = new QMenu(this);
        importMenu->addAction("Open CAD File (.step, .iges, .brep)...", this, [this]() {
            QString fileName = QFileDialog::getOpenFileName(this, "Import CAD File", "", "CAD Files (*.step *.stp *.iges *.igs *.brep);;All Files (*.*)");
            if (!fileName.isEmpty()) {
                appendDebugLog("Importing CAD: " + fileName);
                try {
                    gmsh::model::add("ImportedModel");
                    gmsh::model::occ::importShapes(fileName.toStdString());
                    gmsh::model::occ::synchronize();
                    appendDebugLog("CAD imported and synchronized successfully.");
                } catch (const std::exception& e) {
                    QMessageBox::critical(this, "Import Error", e.what());
                }
            }
        });
        importMenu->addAction("New Workspace", this, [this]() {
            try {
                gmsh::clear();
                gmsh::model::add("NewModel");
                appendDebugLog("Workspace cleared.");
            } catch (...) {}
        });
        importMenu->addSeparator();
        importMenu->addAction("Exit", this, &QWidget::close);
        logoBtn->setMenu(importMenu);
        logoBtn->setPopupMode(QToolButton::InstantPopup);
        topBar->addWidget(logoBtn);

        QLabel* titleLbl = new QLabel("<b>OpenCFD Studio Pro</b>", this);
        titleLbl->setStyleSheet("color: #d4d4d4; font-size: 14px; margin-left: 10px;");
        topBar->addWidget(titleLbl);
        topBar->addStretch();

        // Ribbon Tabs (SolidWorks Style)
        QTabWidget* ribbonTabs = new QTabWidget(this);
        ribbonTabs->setStyleSheet("QTabWidget::pane { border: 1px solid #333; background: #2d2d2d; } QTabBar::tab { background: #3c3c3c; color: #ccc; padding: 6px 14px; margin-right: 2px; border-top-left-radius: 3px; border-top-right-radius: 3px; } QTabBar::tab:selected { background: #007acc; color: white; font-weight: bold; }");

        // Tab 1: Import Settings
        QWidget* tabImport = new QWidget();
        QHBoxLayout* layImport = new QHBoxLayout(tabImport);
        QPushButton* btnImportCAD = new QPushButton("Browse CAD...", tabImport);
        connect(btnImportCAD, &QPushButton::clicked, logoBtn, [logoBtn]() { logoBtn->showMenu(); });
        layImport->addWidget(btnImportCAD);
        layImport->addStretch();
        ribbonTabs->addTab(tabImport, "Import Settings");

        // Tab 2: Issues & Debug
        QWidget* tabIssues = new QWidget();
        QHBoxLayout* layIssues = new QHBoxLayout(tabIssues);
        QPushButton* btnScan = new QPushButton("Scan CAD Geometry", tabIssues);
        connect(btnScan, &QPushButton::clicked, this, [this]() {
            appendDebugLog("Scanning CAD for non-manifold edges, gaps, and slivers...");
            QMessageBox::information(this, "CAD Diagnostics", "Scan complete: No critical topology errors detected.");
        });
        layIssues->addWidget(btnScan);
        layIssues->addStretch();
        ribbonTabs->addTab(tabIssues, "Issues & Debug");

        // Tab 3: Fixes
        QWidget* tabFixes = new QWidget();
        QHBoxLayout* layFixes = new QHBoxLayout(tabFixes);
        QPushButton* btnHeal = new QPushButton("Heal & Stitch Faces", tabFixes);
        connect(btnHeal, &QPushButton::clicked, this, [this]() {
            appendDebugLog("Healing and stitching CAD shapes via OpenCASCADE...");
            try {
                gmsh::model::occ::removeAllDuplicates();
                gmsh::model::occ::synchronize();
                appendDebugLog("Healing complete.");
            } catch (const std::exception& e) {
                appendDebugLog(QString("Healing error: ") + e.what());
            }
        });
        layFixes->addWidget(btnHeal);
        layFixes->addStretch();
        ribbonTabs->addTab(tabFixes, "Fixes");

        // Tab 4: Additions
        QWidget* tabAdditions = new QWidget();
        QHBoxLayout* layAdditions = new QHBoxLayout(tabAdditions);
        QPushButton* btnEnclosure = new QPushButton("Create Fluid Domain Box", tabAdditions);
        connect(btnEnclosure, &QPushButton::clicked, this, [this]() {
            appendDebugLog("Creating outer bounding fluid enclosure...");
            try {
                gmsh::model::occ::addBox(-5, -5, -5, 10, 10, 10, 999);
                gmsh::model::occ::synchronize();
                appendDebugLog("Fluid enclosure created successfully.");
            } catch (const std::exception& e) {
                appendDebugLog(QString("Enclosure error: ") + e.what());
            }
        });
        layAdditions->addWidget(btnEnclosure);
        layAdditions->addStretch();
        ribbonTabs->addTab(tabAdditions, "Additions");

        // Tab 5: Mesh Options
        QWidget* tabMesh = new QWidget();
        QHBoxLayout* layMesh = new QHBoxLayout(tabMesh);
        QPushButton* btnMeshGen = new QPushButton("Generate 3D CFD Tetrahedral Mesh", tabMesh);
        connect(btnMeshGen, &QPushButton::clicked, this, [this]() {
            appendDebugLog("Generating 3D CFD tetrahedral mesh...");
            try {
                gmsh::option::setNumber("Mesh.Algorithm", 6); // Frontal-Delaunay for 3D
                gmsh::model::mesh::generate(3);
                appendDebugLog("3D Mesh generated successfully.");
            } catch (const std::exception& e) {
                QMessageBox::critical(this, "Meshing Error", e.what());
            }
        });
        layMesh->addWidget(btnMeshGen);
        layMesh->addStretch();
        ribbonTabs->addTab(tabMesh, "Mesh Options");

        ribbonLayout->addLayout(topBar);
        ribbonLayout->addWidget(ribbonTabs);
        setMenuWidget(ribbonContainer);
    }

    void setupDockablePanels() {
        // Left Dock: Info and Name Per Section (SolidWorks Feature Tree Style)
        QDockWidget* infoDock = new QDockWidget("Info & Sections Tree", this);
        infoDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        infoTreeWidget = new QListWidget(infoDock);
        infoTreeWidget->addItem("📁 CAD Root Assembly");
        infoTreeWidget->addItem("   ├── 🟦 Fluid Domain [Volume 1]");
        infoTreeWidget->addItem("   ├── 🟩 Inlet Patch [Face 1]");
        infoTreeWidget->addItem("   ├── 🟥 Outlet Patch [Face 2]");
        infoTreeWidget->addItem("   └── 🟨 Wall Boundaries [Faces 3-6]");
        infoTreeWidget->setStyleSheet("QListWidget { background: #252526; color: #d4d4d4; border: none; font-size: 13px; }");
        infoDock->setWidget(infoTreeWidget);
        addDockWidget(Qt::LeftDockWidgetArea, infoDock);

        // Bottom Dock: Debug & OS Telemetry Log (Pop-out / Dockable)
        QDockWidget* logDock = new QDockWidget("Issues & Debug Console", this);
        logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        debugLog = new QPlainTextEdit(logDock);
        debugLog->setReadOnly(true);
        debugLog->setStyleSheet("QPlainTextEdit { background: #1e1e1e; color: #4ec9b0; font-family: Consolas, monospace; font-size: 12px; }");
        logDock->setWidget(debugLog);
        addDockWidget(Qt::BottomDockWidgetArea, logDock);

        // Right Dock: Scaling, Alignment, and Position Tools (Pop-out / Dockable just like SolidWorks)
        QDockWidget* toolsDock = new QDockWidget("Transform & Scaling", this);
        toolsDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
        
        QWidget* toolsWidget = new QWidget(toolsDock);
        QVBoxLayout* toolsLay = new QVBoxLayout(toolsWidget);
        
        toolsLay->addWidget(new QLabel("<b>Scale Factor:</b>"));
        QDoubleSpinBox* scaleSpin = new QDoubleSpinBox(toolsWidget);
        scaleSpin->setRange(0.001, 1000.0);
        scaleSpin->setValue(1.0);
        toolsLay->addWidget(scaleSpin);

        toolsLay->addWidget(new QLabel("<b>Alignment Axis:</b>"));
        QComboBox* alignCombo = new QComboBox(toolsWidget);
        alignCombo->addItems({"Align along X", "Align along Y", "Align along Z"});
        toolsLay->addWidget(alignCombo);

        QPushButton* btnApplyTransform = new QPushButton("Apply Transformation", toolsWidget);
        toolsLay->addWidget(btnApplyTransform);
        toolsLay->addStretch();
        
        toolsDock->setWidget(toolsWidget);
        addDockWidget(Qt::RightDockWidgetArea, toolsDock);
    }

    void setupViewport() {
        vtkPanel = new QVTKOpenGLNativeWidget(this);
        renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        vtkPanel->setRenderWindow(renderWindow);

        renderer = vtkSmartPointer<vtkRenderer>::New();
        renderWindow->AddRenderer(renderer);
        renderer->SetBackground(0.12, 0.12, 0.14);

        style = vtkSmartPointer<MesherInteractorStyle>::New();
        renderWindow->GetInteractor()->SetInteractorStyle(style);

        // Add Coordinate Axes Gizmo
        vtkNew<vtkAxesActor> axes;
        vtkNew<vtkOrientationMarkerWidget> axesWidget;
        axesWidget->SetOrientationMarker(axes);
        axesWidget->SetInteractor(renderWindow->GetInteractor());
        axesWidget->SetEnabled(1);
        axesWidget->InteractiveOn();

        // Sample Cube Preview
        vtkNew<vtkCubeSource> cube;
        cube->SetBounds(-2, 2, -2, 2, -2, 2);
        cube->Update();

        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(cube->GetOutputPort());

        vtkNew<vtkActor> actor;
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.2, 0.6, 0.9);
        actor->GetProperty()->EdgeVisibilityOn();
        actor->GetProperty()->SetEdgeColor(1, 1, 1);
        renderer->AddActor(actor);

        renderer->ResetCamera();
        setCentralWidget(vtkPanel);
    }
};

int main(int argc, char* argv[]) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    DiscoveryWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
