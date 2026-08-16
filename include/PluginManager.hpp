#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

class PluginManager {
private:
    std::map<std::string, HMODULE> loadedPlugins;

public:
    bool isPluginLoaded(const std::string& pluginId);
    bool loadPluginFromMemory(const std::string& pluginId, const std::vector<unsigned char>& buffer);
    bool loadPluginFromFile(const std::string& pluginId, const std::string& filePath);

    void executePlugin(const std::string& pluginId, const std::string& funcName, SOCKET serverSock);
    void executePluginCommand(const std::string& pluginId, const std::string& funcName, SOCKET serverSock, const std::string& commandJson);
    void executePluginBinary(const std::string& pluginId, const std::string& funcName, SOCKET serverSock, const std::vector<uint8_t>& payload);
};