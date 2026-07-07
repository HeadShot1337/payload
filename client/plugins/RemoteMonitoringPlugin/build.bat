@echo off
if not exist build mkdir build

echo [+] Remote Monitoring Plugin (H.265) Derleniyor...
g++ -O2 -shared main.cpp -o build/RemoteMonitoringPlugin.dll ^
    -lws2_32 -lgdi32 -luser32 -lole32 -lmfplat -lmfuuid -lwmcodecdspuuid -lstrmiids -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/RemoteMonitoringPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause
