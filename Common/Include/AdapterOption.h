#pragma once

#include <wrl/client.h> 	// For ComPtr
#include <IddCx.h>       	// For IDDCX_ADAPTER, IddCxAdapterSetRenderAdapter
#include <dxgi1_5.h>   	// For IDXGIAdapter, IDXGIAdapter1, IDXGIFactory1, DXGI_ADAPTER_DESC1
#include <algorithm>    	// For sort
#include <setupapi.h>
#include <devguid.h>
#include <devpropdef.h>
#include <devpkey.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// The driver's logger and name converter, defined in Driver.cpp. Declared here so
// the header can report what it resolved and what it substituted for it.
void vddlog(const char* type, const char* message);
std::string WStringToString(const std::wstring& wstr);

using namespace std;
using namespace Microsoft::WRL;

// DEVPKEY_Device_Luid: {60b193cb-5276-4d0f-96fc-f173ab17af69}, 2
// Define it ourselves to avoid SDK/WDK header differences where DEVPKEY_Device_Luid may not be declared.
static const DEVPROPKEY DEVPKEY_Device_Luid_Custom = {
    { 0x60b193cb, 0x5276, 0x4d0f, { 0x96, 0xfc, 0xf1, 0x73, 0xab, 0xad, 0x3e, 0xc6 } },
    2
};

// Structure to vector gpus
struct GPUInfo {
    wstring name;                // GPU name
    ComPtr<IDXGIAdapter> adapter;// COM pointer to the adapter
    DXGI_ADAPTER_DESC1 desc;     // Adapter description (with the software flag)
};

// Sort function for GPUs by dedicated video memory
inline bool CompareGPUs(const GPUInfo& a, const GPUInfo& b) {
    return a.desc.DedicatedVideoMemory > b.desc.DedicatedVideoMemory;
}

// Get a enumerate list of available GPUs
inline vector<GPUInfo> getAvailableGPUs() {
    vector<GPUInfo> gpus; // Vector to hold all GPU's information

    ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return gpus;
    }

    // Enumerate all adapters (GPUs)
    for (UINT i = 0;; i++) {
        // EnumAdapters1 and GetDesc1, not the plain ones: the software flag is
        // only on DXGI_ADAPTER_DESC1.
        ComPtr<IDXGIAdapter1> adapter1;
        if (!SUCCEEDED(factory->EnumAdapters1(i, &adapter1))) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc1;

        if (!SUCCEEDED(adapter1->GetDesc1(&desc1))) {
            continue;
        }

        // The software rasteriser is not an adapter the OS presents on, and a
        // D3D11 device CAN be created on it -- so it used to be enumerable,
        // selectable by name, and the swap chain could end up on it. Measured on
        // this machine: the list held "Microsoft Basic Render Driver" with
        // DXGI_ADAPTER_FLAG_SOFTWARE set, and a device was created on it.
        if ((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        // Add the adapter information to the list
        const DXGI_ADAPTER_DESC1 desc = desc1;
        ComPtr<IDXGIAdapter> adapter = adapter1;
        GPUInfo info{ desc.Description, adapter, desc };
        gpus.push_back(info);
    }

    return gpus;
}

// The name of the adapter with this LUID, or an empty string when there is none.
inline wstring nameOfAdapter(LUID luid) {
    for (const auto& gpu : getAvailableGPUs()) {
        if (gpu.desc.AdapterLuid.LowPart == luid.LowPart &&
            gpu.desc.AdapterLuid.HighPart == luid.HighPart) {
            return gpu.name;
        }
    }
    return L"";
}

// Resolve an adapter LUID from a PCI bus number by enumerating display devices (SetupAPI).
// Returns nullopt if no match is found or if the system doesn't expose the LUID property.
inline std::optional<LUID> ResolveAdapterLuidFromPciBus(uint32_t targetBusIndex) {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    SP_DEVINFO_DATA devData = {};
    devData.cbSize = sizeof(devData);

    std::optional<LUID> result = std::nullopt;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); ++i) {
        DWORD currentBus = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(
                devInfo,
                &devData,
                SPDRP_BUSNUMBER,
                nullptr,
                reinterpret_cast<PBYTE>(&currentBus),
                sizeof(currentBus),
                nullptr)) {
            continue;
        }

        if (static_cast<uint32_t>(currentBus) != targetBusIndex) {
            continue;
        }

        // DEVPKEY_Device_Luid is exposed as a UINT64 on Windows; convert into LUID.
        DEVPROPTYPE propType = 0;
        ULONG propSize = 0;
        ULONGLONG luid64 = 0;

        if (!SetupDiGetDevicePropertyW(
                devInfo,
                &devData,
                &DEVPKEY_Device_Luid_Custom,
                &propType,
                reinterpret_cast<PBYTE>(&luid64),
                sizeof(luid64),
                &propSize,
                0)) {
            continue;
        }

        if (propType != DEVPROP_TYPE_UINT64 || propSize != sizeof(luid64)) {
            continue;
        }

        LUID luid{};
        luid.LowPart = static_cast<DWORD>(luid64 & 0xFFFFFFFFull);
        luid.HighPart = static_cast<LONG>((luid64 >> 32) & 0xFFFFFFFFull);
        result = luid;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

class AdapterOption {
public:
    bool hasTargetAdapter{}; // Indicates if a target adapter is selected
    LUID adapterLuid{};      // Adapter's unique identifier (LUID)
    wstring target_name{};   // Target adapter name

    // Select the best GPU based on dedicated video memory. The LUID is what
    // identifies it: matching by name threw the ranking away, because two cards
    // with the same name resolved to whichever one DXGI enumerated first.
    bool selectBestAdapter() {
        auto gpus = getAvailableGPUs();
        if (gpus.empty()) {
            return false; // Error check for headless / vm
        }

        // Sort GPUs by dedicated video memory in descending order
        sort(gpus.begin(), gpus.end(), CompareGPUs);
        const GPUInfo& bestGPU = gpus.front(); // Get the GPU with the most memory

        adapterLuid = bestGPU.desc.AdapterLuid;
        target_name = bestGPU.name;
        hasTargetAdapter = true;
        return true;
    }

    // The name of the best GPU, for the log line only.
    wstring selectBestGPU() {
        auto gpus = getAvailableGPUs();
        if (gpus.empty()) {
            return L""; // Error check for headless / vm
        }

        sort(gpus.begin(), gpus.end(), CompareGPUs);
        return gpus.front().name;
    }

    // Load friendlyname from a file OR select the best GPU 
    void load(const wchar_t* path) {
        ifstream ifs{ path };

        if (!ifs.is_open()) {
            target_name = selectBestGPU();
        }
        else {
            string line;
            getline(ifs, line);
            // A file saved with a UTF-8 BOM, or with a trailing space, carried a name
            // that no adapter has: the configured adapter was discarded silently and a
            // different one was used. Strip both before the name is compared.
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
            while (!line.empty() &&
                (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
                line.pop_back();
            }
            // Decode, do not widen. assign(line.begin(), line.end()) copies one
            // byte per wchar_t, so a UTF-8 GPU name became a string of
            // individual code units that never matched the DXGI name, and
            // findAndSetAdapter fell back to selectBestGPU() -- the user's
            // explicit choice was dropped with no error.
            if (!line.empty()) {
                int needed = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
                if (needed > 0) {
                    wstring wide(needed, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), wide.data(), needed);
                    target_name = wide;
                }
            }
        }

        // Find and set the adapter based on the target name
        if (!findAndSetAdapter(target_name)) {
            // The configured name matched no adapter, and the old code swapped in
            // another one silently: the operator got a render adapter they had not
            // chosen, with nothing in the log to say so.
            stringstream ss;
            ss << "The configured adapter \"" << WStringToString(target_name)
               << "\" was not found; falling back to the best adapter by video memory.";
            vddlog("w", ss.str().c_str());
            selectBestAdapter();
        }
    }

    // Set the target adapter from a given name and validate it
    void xmlprovide(const wstring& xtarg) {
        target_name = xtarg;
        if (!findAndSetAdapter(target_name)) {
            // The configured name matched no adapter, and the old code swapped in
            // another one silently: the operator got a render adapter they had not
            // chosen, with nothing in the log to say so.
            stringstream ss;
            ss << "The configured adapter \"" << WStringToString(target_name)
               << "\" was not found; falling back to the best adapter by video memory.";
            vddlog("w", ss.str().c_str());
            selectBestAdapter();
        }
    }

    // Apply the adapter settings to the specified adapter
    void apply(const IDDCX_ADAPTER& adapter) {
        if (hasTargetAdapter && IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
            IDARG_IN_ADAPTERSETRENDERADAPTER arg{};
            arg.PreferredRenderAdapter = adapterLuid;
            IddCxAdapterSetRenderAdapter(adapter, &arg);
        }
    }

private:
    // Find and set the adapter by name, optionally using "name,bus" where bus is the PCI bus number.
    bool findAndSetAdapter(const wstring& adapterSpec) {
        // If user provides "name,bus", use bus to resolve LUID (more deterministic on multi-GPU setups).
        const size_t comma = adapterSpec.find(L',');
        if (comma != wstring::npos) {
            const wstring namePart = adapterSpec.substr(0, comma);
            wstring busPart = adapterSpec.substr(comma + 1);
            // Trim whitespace in bus part
            busPart.erase(remove_if(busPart.begin(), busPart.end(), iswspace), busPart.end());

            wchar_t* end = nullptr;
            const unsigned long busUl = wcstoul(busPart.c_str(), &end, 10);
            const bool parsedOk = (end != nullptr) && (*end == L'\0') && (end != busPart.c_str());
            if (parsedOk && busUl <= 0xFFFFFFFFul) {
                if (auto luidOpt = ResolveAdapterLuidFromPciBus(static_cast<uint32_t>(busUl)); luidOpt.has_value()) {
                    adapterLuid = luidOpt.value();
                    hasTargetAdapter = true;
                    // The bus wins, and the name was never checked against it, so
                    // the log paired one adapter's name with another adapter's LUID.
                    // Say what the bus actually resolved to.
                    const wstring resolved = nameOfAdapter(adapterLuid);
                    if (!resolved.empty() && _wcsicmp(resolved.c_str(), namePart.c_str()) != 0) {
                        stringstream ss;
                        ss << "The adapter on PCI bus " << busUl << " is \"" << WStringToString(resolved)
                            << "\", which is not the configured \"" << WStringToString(namePart) << "\".";
                        vddlog("w", ss.str().c_str());
                        target_name = resolved;
                    }
                    return true;
                }
            }

            // Fall through to name matching using the name portion.
            return findAndSetAdapter(namePart);
        }

        const wstring& adapterName = adapterSpec;
        auto gpus = getAvailableGPUs();

        // Iterate through all available GPUs
        for (const auto& gpu : gpus) {
            if (_wcsicmp(gpu.name.c_str(), adapterName.c_str()) == 0) {
                adapterLuid = gpu.desc.AdapterLuid; // Set the adapter LUID
                hasTargetAdapter = true; // Indicate that a target adapter is selected
                return true;
            }
        }

        hasTargetAdapter = false; // Indicate that no target adapter is selected
        return false;
    }
};
