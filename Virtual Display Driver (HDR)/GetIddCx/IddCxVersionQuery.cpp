#include <windows.h>
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <conio.h>

// What this tool can and cannot tell you.
//
// It CAN read the Windows build number, and it CAN read the IddCx headers
// installed on this machine -- that part is a measurement.
//
// It CANNOT read the IddCx version the running system implements. That answer
// comes from IddCxGetVersion(), which needs an IddDriverGlobals -- a UMDF host
// -- so it cannot be called from a standalone console program. Everything this
// tool prints about the runtime is therefore an inference from the build
// number, and it is labelled as such. IddCx.h documents the API for exactly
// this question: "the driver can call the IddCxGetVersion() callback ... This
// helps if IddCx bugs that affect behavior are fixed and released within a
// given OS release", which is the case a build-number table gets wrong.

// The constants are the SDK's own IDDCX_VERSION_* values:
//   1.2  RS3              0x1200
//   1.3  RS4 / RS5        0x1300 / 0x1380
//   1.4  19H1             0x1400
//   1.5  Vibrainium       0x1500
//   1.6  Manganese        0x1600
//   1.7  Iron             0x1700
//   1.8  Cobalt           0x1800, and 0x1801 for the UpdateMode fix
//   1.9  SV2              0x1900
//   1.10 SV2_SEPT23       0x1A00, and 0x1A80 (GERMANIUM) for the hotfix
// The SDK spells the two "hotfix" values IDDCX_VERSION_<name>_UPDATEMODE_FIX.
//
// Windows 10 Creators Update (15063) is the first build that ships IddCx.
// Builds before it have no IddCx at all and are not in the table: the old
// entries claimed "1.0" for builds 10240 and 14393, and 0x1000 is not an
// IDDCX_VERSION_* constant anywhere in the SDK.
struct IddCxVersionInfo {
    std::string windowsVersion;   // the release this build belongs to
    std::string iddcxVersion;     // the IddCx version expected on that build
    ULONG versionValue;           // the SDK's IDDCX_VERSION_* constant
};

std::map<DWORD, IddCxVersionInfo> versionMap = {
    {26100, {"Windows 11 24H2", "1.10", 0x1A80}},
    {22631, {"Windows 11 23H2", "1.10", 0x1A00}},
    {22621, {"Windows 11 22H2", "1.9",  0x1900}},
    {22000, {"Windows 11 21H2", "1.8",  0x1800}},
    {19045, {"Windows 10 22H2", "1.5",  0x1500}},
    {19044, {"Windows 10 21H2", "1.5",  0x1500}},
    {19043, {"Windows 10 21H1", "1.5",  0x1500}},
    {19042, {"Windows 10 20H2", "1.5",  0x1500}},
    {19041, {"Windows 10 20H1", "1.5",  0x1500}},
    {18363, {"Windows 10 19H2", "1.4",  0x1400}},
    {18362, {"Windows 10 19H1", "1.4",  0x1400}},
    {17763, {"Windows 10 RS5 (October 2018)", "1.3", 0x1380}},
    // RS3, RS4 and RS5 are the Redstone branch names: 16299 is Redstone 3, the
    // Fall Creators Update; 17134 is Redstone 4, the April 2018 Update; 17763 is
    // Redstone 5, the October 2018 Update. The version string is the IddCx version,
    // and 0x1200 is IddCx 1.2 -- the old table said 1.3 next to it.
    {17134, {"Windows 10 RS4 (April 2018)",   "1.3", 0x1300}},
    {16299, {"Windows 10 RS3 (Fall Creators Update)", "1.2", 0x1200}},
    {15063, {"Windows 10 Creators Update", "1.2", 0x1200}}
};

DWORD GetWindowsBuildNumber() {
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

    // Walk the map from the newest build down, so a build that is not in the
    // table gets the newest entry that is not newer than it. A forward walk
    // returned the oldest entry in the table for every unknown build.
    for (auto it = versionMap.rbegin(); it != versionMap.rend(); ++it) {
        if (buildNumber >= it->first) {
            IddCxVersionInfo info = it->second;
            // The release name is the nearest one at or below, so it is an
            // approximation for a build that is not in the table.
            info.windowsVersion += " or newer";
            return info;
        }
    }

    return {"Unknown Windows Version", "Unknown", 0x0000};
}

// A directory name under um\iddcx\ is not guaranteed to be a version. A file
// called README.txt can sit next to the version folders, and std::stol on it
// threw std::invalid_argument out of the sort comparator, which aborted the
// tool with no output at all. Parse the name by hand and skip what is not one.
bool ParseNonNegativeInt(const std::wstring& s, int& out) {
    if (s.empty()) { return false; }
    long value = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') { return false; }
        value = value * 10 + (c - L'0');
        if (value > 100000) { return false; }
    }
    out = (int)value;
    return true;
}

bool ParseVersionDir(const std::wstring& name, int& major, int& minor) {
    const size_t dot = name.find(L'.');
    if (dot == std::wstring::npos) { return false; }
    return ParseNonNegativeInt(name.substr(0, dot), major)
        && ParseNonNegativeInt(name.substr(dot + 1), minor);
}

// The newest IddCx header set installed on this machine. This part is a
// measurement, not an inference: it reads um\iddcx\<version>\IddCx.h out of
// the Windows Kits.
std::string GetNewestInstalledIddCxHeaders(const wchar_t* kitsRoot) {
    if (kitsRoot == nullptr) { return ""; }
    std::vector<std::wstring> roots;
    // The versioned layout the Windows SDK actually ships:
    //   Windows Kits\10\Include\<sdk>\um\iddcx\<version>\IddCx.h
    {
        WIN32_FIND_DATAW fd;
        const std::wstring includeRoot = std::wstring(kitsRoot) + L"\\Include";
        HANDLE h = FindFirstFileW((includeRoot + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) { continue; }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    roots.push_back(includeRoot + L"\\" + fd.cFileName + L"\\um\\iddcx");
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    // The unversioned layout, in case an SDK ever ships one.
    roots.push_back(std::wstring(kitsRoot) + L"\\Include\\iddcx");

    // Every version directory of every root, newest first. The old code walked
    // the roots in turn and returned the newest version of the FIRST root that
    // had any version directory at all, so a machine with two SDKs reported the
    // newest header set of whichever one the directory enumeration reached
    // first -- while the line claimed to be the newest on the machine.
    struct Candidate {
        int major;
        int minor;
        std::wstring name;
        std::wstring root;
    };
    std::vector<Candidate> candidates;
    for (const auto& root : roots) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { continue; }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) { continue; }
            int major = 0, minor = 0;
            if (!ParseVersionDir(fd.cFileName, major, minor)) { continue; }
            candidates.push_back({ major, minor, fd.cFileName, root });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Compare major and minor separately. As a double, "1.10" is 1.1 and
    // sorts BELOW "1.9", which would pick the older header set.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.major != b.major) { return a.major > b.major; }
        return a.minor > b.minor;
    });
    for (const auto& candidate : candidates) {
        if (GetFileAttributesW((candidate.root + L"\\" + candidate.name + L"\\IddCx.h").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return std::string(candidate.name.begin(), candidate.name.end());
        }
    }
    return "";
}

int wmain(int argc, wchar_t** argv) {
    std::cout << "IddCx Version Query Tool\n";
    std::cout << "========================\n\n";

    DWORD buildNumber = GetWindowsBuildNumber();

    if (buildNumber == 0) {
        std::cout << "Error: Could not retrieve Windows build number.\n";
        return 1;
    }

    std::cout << "Windows Build Number: " << buildNumber << "\n";

    IddCxVersionInfo versionInfo = GetIddCxVersionFromBuild(buildNumber);

    std::cout << "Windows Release (nearest at or below this build): " << versionInfo.windowsVersion << "\n";
    std::cout << "IddCx version expected on this build: " << versionInfo.iddcxVersion << "\n";
    std::cout << "IddCx version value (IDDCX_VERSION_*): 0x" << std::hex << std::uppercase << versionInfo.versionValue << "\n\n";

    // The measurement, as opposed to the inference above.
    const wchar_t* kitsRoot = nullptr;
    if (argc > 1) {
        kitsRoot = argv[1];
    } else {
        wchar_t buf[MAX_PATH] = { 0 };
        // GetEnvironmentVariableW returns the required size, including the terminator,
        // when the value does not fit. A value that does not fit would leave a truncated
        // path in the buffer, and the scan below would then report that no IddCx header
        // set is installed -- a wrong answer about the machine, not a missing one.
        const DWORD copied = GetEnvironmentVariableW(L"WindowsSdkDir", buf, MAX_PATH);
        if (copied > 0 && copied < MAX_PATH) {
            kitsRoot = buf;
        }
    }
    const std::string installed = GetNewestInstalledIddCxHeaders(kitsRoot);
    if (!installed.empty()) {
        std::cout << "Newest IddCx header set installed on this machine: " << installed << "\n";
        std::cout << "  (read from the Windows Kits; this is a measurement, not an inference)\n\n";
    }
    else {
        // Say so. The measurement was skipped silently, so "not measured" and
        // "none installed" printed exactly the same thing, and a Kits root that
        // could not be read was indistinguishable from a machine without one.
        std::cout << "Newest IddCx header set installed on this machine: not measured.\n";
        std::cout << "  (the Windows Kits root could not be read, or no IddCx header set\n";
        std::cout << "   was found under it -- that is not the same as 'none installed')\n\n";
    }

    std::cout << "How to read the numbers above:\n";
    std::cout << "- The build number is read from the running system.\n";
    std::cout << "- The IddCx version and its constant are INFERRED from the build\n";
    std::cout << "  number. The table above cannot express every IddCx version, and a\n";
    std::cout << "  service update can ship a newer IddCx than the release did.\n";
    std::cout << "- The only authoritative source is IddCxGetVersion(), which needs a\n";
    std::cout << "  UMDF host and so cannot be called from this tool.\n\n";

    if (versionInfo.iddcxVersion == "Unknown") {
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