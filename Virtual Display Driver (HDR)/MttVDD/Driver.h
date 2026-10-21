#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <atomic>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <sstream>

#include "Trace.h"

// Utility function declarations
std::vector<std::string> split(std::string& input, char delimiter);
std::string WStringToString(const std::wstring& wstr);

// Phase 5: Final Integration function declarations
NTSTATUS ValidateEdidIntegration();
NTSTATUS PerformanceMonitor();
NTSTATUS CreateFallbackConfiguration();
NTSTATUS ValidateAndSanitizeConfiguration();
NTSTATUS RunComprehensiveDiagnostics();
NTSTATUS InitializePhase5Integration();

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;

            // Set when the device is lost: a TDR, a driver reset, an eGPU unplugged.
            // A lost device never comes back, so the cache must not hand it out
            // again -- that is what made the recovery loop churn instead of
            // recovering. Written by the swap-chain thread, read under the cache
            // mutex, so it is atomic.
            std::atomic<bool> bLost{ false };
            void MarkLost() { bLost.store(true); }
            bool IsLost() const { return bLost.load(); }
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

        public:
            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        /// <summary>
        /// Custom comparator for LUID to be used in std::map
        /// </summary>
        struct LuidComparator
        {
            bool operator()(const LUID& a, const LUID& b) const
            {
                if (a.HighPart != b.HighPart)
                    return a.HighPart < b.HighPart;
                return a.LowPart < b.LowPart;
            }
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

            void CreateMonitor(unsigned int index);

            void AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain(IDDCX_MONITOR Monitor);

        protected:

            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            // The monitors this context created, one per connector index. The driver used
            // to keep only the last one in a single member, so the others were
            // unaddressable and no departure could ever be reported for them. The second
            // member was dead: it was never written and never read.
            std::vector<IDDCX_MONITOR> m_Monitors;
            std::map<IDDCX_MONITOR, std::unique_ptr<SwapChainProcessor>> m_ProcessingThreads;

            // Guards m_ProcessingThreads, the map of swap-chain processors, one per monitor.
            std::mutex m_ProcessingThreadsMutex;

        public:
            // The EDID the driver hands the monitor. maincalc() publishes it on one thread
            // while the IddCx callbacks read it on another, so the swap and the read are
            // both under s_KnownMonitorEdidMutex.
            static std::vector<BYTE> s_KnownMonitorEdid;
            static std::mutex s_KnownMonitorEdidMutex;

        private:
            static std::map<LUID, std::shared_ptr<Direct3DDevice>, LuidComparator> s_DeviceCache;
            static std::mutex s_DeviceCacheMutex;
            static std::shared_ptr<Direct3DDevice> GetOrCreateDevice(LUID RenderAdapter);
            static void CleanupExpiredDevices();
        };
    }
}
