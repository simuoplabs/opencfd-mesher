#pragma once
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstdint>
#include <string>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(__APPLE__)
    #if !defined(_WIN32)
#include <sys/mman.h>
#endif
    #include <sys/stat.h>
    #include <sys/sysctl.h>
    #if !defined(_WIN32)
#include <fcntl.h>
#endif
    #if !defined(_WIN32)
#include <unistd.h>
#endif
#elif defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
    #include <malloc.h>
    #include <sys/stat.h>
    #if defined(_M_X64) || defined(__x86_64__)
        #include <intrin.h>
    #endif
#elif defined(__linux__)
    #include <malloc.h>
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
    #include <fstream>
    #if defined(__x86_64__) || defined(__i386__)
        #include <cpuid.h>
    #endif
#endif

// ============================================================================
// MULTI-VENDOR GPU DRIVER HOOKS (NVIDIA OPTIMUS & AMD RADEON SMARTSHIFT)
// Forces Windows & Linux dual-GPU systems to select the dedicated NVIDIA/AMD GPU
// instead of throttling on low-power integrated graphics.
// ============================================================================
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#elif defined(__linux__)
extern "C" {
    __attribute__((visibility("default"))) unsigned long NvOptimusEnablement = 0x00000001;
    __attribute__((visibility("default"))) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace SimuopOS {

    // 1. Detect Exact CPU Architecture & Vendor (AMD Ryzen, Intel Core/Xeon, or ARM64)
    inline std::string getCpuArchitectureAndBrand() {
        std::string arch =
        #if defined(__aarch64__) || defined(_M_ARM64)
            "ARM64";
        #elif defined(__x86_64__) || defined(_M_X64)
            "x86_64 (Intel / AMD Ryzen)";
        #else
            "Generic-64";
        #endif

        #if defined(__APPLE__)
            char brand[128] = {0};
            size_t size = sizeof(brand);
            if (sysctlbyname("machdep.cpu.brand_string", &brand, &size, nullptr, 0) == 0 && brand[0] != '\0') {
                return std::string(brand) + " [" + arch + "]";
            }
        #elif (defined(_WIN32) || defined(__linux__)) && (defined(__x86_64__) || defined(_M_X64))
            char brand[0x40] = {0};
            #if defined(_WIN32)
                int cpuInfo[4] = {-1};
                __cpuid(cpuInfo, 0x80000000);
                unsigned int nExIds = cpuInfo[0];
                if (nExIds >= 0x80000004) {
                    __cpuid((int*)(brand),      0x80000002);
                    __cpuid((int*)(brand + 16), 0x80000003);
                    __cpuid((int*)(brand + 32), 0x80000004);
                    return std::string(brand) + " [" + arch + "]";
                }
            #elif defined(__linux__)
                std::ifstream cpuinfo("/proc/cpuinfo");
                std::string line;
                while (std::getline(cpuinfo, line)) {
                    if (line.find("model name") != std::string::npos) {
                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            return line.substr(colon + 2) + " [" + arch + "]";
                        }
                    }
                }
            #endif
        #endif
        return arch;
    }

    // 2. Configure Environment for Target GPU Vendor (NVIDIA / AMD / Intel / Software Fallback)
    inline void applyGpuVendorPreference(const std::string& gpuMode) {
        if (gpuMode == "software_cpu") {
            #if defined(_WIN32)
                _putenv_s("QT_OPENGL", "software");
                _putenv_s("GALLIUM_DRIVER", "llvmpipe");
            #else
                setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
                setenv("GALLIUM_DRIVER", "llvmpipe", 1);
            #endif
        } else if (gpuMode == "nvidia") {
            #if defined(__linux__)
                // Enables NVIDIA PRIME Render Offload on Linux laptops (Intel/AMD iGPU + NVIDIA dGPU)
                setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
                setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
            #endif
        } else if (gpuMode == "amd") {
            #if defined(__linux__)
                // Enables AMD Radeon discrete GPU offload via DRI_PRIME=1 on Linux
                setenv("DRI_PRIME", "1", 1);
            #endif
        } else if (gpuMode == "intel") {
            #if defined(__linux__)
                setenv("DRI_PRIME", "0", 1);
            #endif
        }
    }

    // 3. Cross-Platform Physical RAM Footprint (MB)
    inline double getCurrentRamUsageMB() {
    #if defined(__APPLE__)
        task_vm_info_data_t vmInfo;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count) == KERN_SUCCESS) {
            return static_cast<double>(vmInfo.phys_footprint) / (1024.0 * 1024.0);
        }
        return 0.0;
    #elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }
        return 0.0;
    #elif defined(__linux__)
        std::ifstream statm("/proc/self/statm");
        long totalPages = 0, rssPages = 0;
        if (statm >> totalPages >> rssPages) {
            long pageSizeKB = sysconf(_SC_PAGESIZE) / 1024;
            return static_cast<double>(rssPages * pageSizeKB) / 1024.0;
        }
        return 0.0;
    #else
        return 0.0;
    #endif
    }

    // 4. Cross-Platform Heap Reclamation
    inline void releaseFreeRamToOS() {
    #if defined(__APPLE__)
        malloc_zone_pressure_relief(nullptr, 0);
    #elif defined(_WIN32)
        _heapmin();
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    #elif defined(__linux__)
        malloc_trim(0);
    #endif
    }

    // 5. Cross-Platform Read-Only Memory-Mapped File (POSIX mmap / Win32 MapViewOfFile)
    class MappedFileReader {
    public:
        const void* data = nullptr;
        size_t sizeBytes = 0;

    #if defined(_WIN32)
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap  = nullptr;
    #else
        int fd = -1;
    #endif

        bool openAndMap(const std::string& path, size_t expectedBytes) {
            sizeBytes = expectedBytes;
            if (sizeBytes == 0) return false;

        #if defined(_WIN32)
            hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return false;
            hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!hMap) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false; }
            data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeBytes);
            return (data != nullptr);
        #else
            fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) return false;
            void* ptr = ::mmap(nullptr, sizeBytes, PROT_READ, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) { ::close(fd); fd = -1; return false; }
            ::madvise(ptr, sizeBytes, MADV_SEQUENTIAL);
            data = ptr;
            return true;
        #endif
        }

        void closeAndUnmap() {
        #if defined(_WIN32)
            if (data) { UnmapViewOfFile(data); data = nullptr; }
            if (hMap) { CloseHandle(hMap); hMap = nullptr; }
            if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
        #else
            if (data) {
                #if defined(__APPLE__)
                ::madvise((void*)data, sizeBytes, MADV_FREE_REUSABLE);
                #elif defined(__linux__)
                ::madvise((void*)data, sizeBytes, MADV_DONTNEED);
                #endif
                ::munmap((void*)data, sizeBytes);
                data = nullptr;
            }
            if (fd >= 0) { ::close(fd); fd = -1; }
        #endif
            sizeBytes = 0;
        }

        ~MappedFileReader() { closeAndUnmap(); }
    };

} // namespace SimuopOS
