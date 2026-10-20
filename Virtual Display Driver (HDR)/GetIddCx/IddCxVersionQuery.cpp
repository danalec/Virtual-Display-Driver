#include <windows.h>
#include <iostream>
#include <string>
#include <map>
#include <conio.h>

struct IddCxVersionInfo {
    std::string windowsVersion;
    std::string iddcxVersion;
    ULONG versionValue;
};

std::map<DWORD, IddCxVersionInfo> versionMap = {
    {26100, {"Windows 11 24H2", "1.10", 0x1A80}},
    {22631, {"Windows 11 23H2", "1.10", 0x1A00}},
    {22621, {"Windows 11 22H2", "1.9", 0x1900}},
    {22000, {"Windows 11 21H2", "1.8", 0x1800}},
    {19045, {"Windows 10 22H2", "1.5", 0x1500}},
    {19044, {"Windows 10 21H2", "1.5", 0x1500}},
    {19043, {"Windows 10 21H1", "1.5", 0x1500}},
    {19042, {"Windows 10 20H2", "1.5", 0x1500}},
    {19041, {"Windows 10 20H1", "1.5", 0x1500}},
    {18363, {"Windows 10 19H2", "1.4", 0x1400}},
    {18362, {"Windows 10 19H1", "1.4", 0x1400}},
    {17763, {"Windows 10 RS5", "1.3", 0x1300}},
    {17134, {"Windows 10 RS4", "1.3", 0x1300}},
    {16299, {"Windows 10 RS3", "1.3", 0x1300}},
    {15063, {"Windows 10 Creators Update", "1.2", 0x1200}},
    {14393, {"Windows 10 Anniversary Update", "1.0", 0x1000}},
    {10240, {"Windows 10 RTM", "1.0", 0x1000}}
};

DWORD GetWindowsBuildNumber() {
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        
        if (RtlGetVersion) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (RtlGetVersion(&rovi) == 0) {
                return rovi.dwBuildNumber;
            }
        }
    }
    
    return 0;
}

IddCxVersionInfo GetIddCxVersionFromBuild(DWORD buildNumber) {
    auto it = versionMap.find(buildNumber);
    if (it != versionMap.end()) {
        return it->second;
    }
    
    // Walk the map from the newest build down. std::map iterates in ascending key order, so
    // the original forward loop returned the first entry with buildNumber >= pair.first --
    // which is the oldest entry in the table (10240), reporting IddCx 1.0 for every build
    // that is not an exact match.
    for (auto it = versionMap.rbegin(); it != versionMap.rend(); ++it) {
        if (buildNumber >= it->first) {
            return it->second;
        }
    }
    
    return {"Unknown Windows Version", "Unknown", 0x0000};
}

int main() {
    std::cout << "IddCx Version Query Tool\n";
    std::cout << "========================\n\n";
    
    DWORD buildNumber = GetWindowsBuildNumber();
    
    if (buildNumber == 0) {
        std::cout << "Error: Could not retrieve Windows build number.\n";
        return 1;
    }
    
    std::cout << "Windows Build Number: " << buildNumber << "\n";
    
    IddCxVersionInfo versionInfo = GetIddCxVersionFromBuild(buildNumber);
    
    std::cout << "Windows Version: " << versionInfo.windowsVersion << "\n";
    std::cout << "IddCx Version: " << versionInfo.iddcxVersion << "\n";
    std::cout << "IddCx Version Value: 0x" << std::hex << std::uppercase << versionInfo.versionValue << "\n\n";
    
    if (versionInfo.iddcxVersion == "Unknown") {
        std::cout << "Note: This Windows build may not support IddCx or uses an unknown version.\n";
        std::cout << "IddCx was introduced in Windows 10 Creators Update (build 15063).\n";
    } else {
        std::cout << "IddCx Framework Information:\n";
        std::cout << "- IddCx (Indirect Display Driver Class eXtension) is a framework for\n";
        std::cout << "  developing indirect display drivers in Windows.\n";
        std::cout << "- This version provides specific capabilities and APIs for indirect\n";
        std::cout << "  display driver development.\n";
    }
    
    // Wait for a key only when a real console is attached. _getch() reads the console input
    // buffer directly, so with the output redirected it would block forever; the std::cin.get()
    // it replaced returned immediately on EOF but required Enter, which is not what the prompt
    // claimed. Gating on GetConsoleMode gives the intended single-key wait interactively and
    // keeps the tool usable from a script or a pipe.
    const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwOutMode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &dwOutMode)) {
        std::cout << "\nPress any key to exit...";
        (void)_getch();
    }

    return 0;
}