@echo off
if not exist build mkdir build

echo [+] Remote Monitoring Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/RemoteMonitoringPlugin.dll ^
    -lws2_32 -lgdiplus -lgdi32 -luser32 -lole32 -loleaut32 -lmfplat -lmfuuid -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/RemoteMonitoringPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause
