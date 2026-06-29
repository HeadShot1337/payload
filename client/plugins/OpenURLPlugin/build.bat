@echo off
if not exist build mkdir build

echo [+] Open URL Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/OpenURLPlugin.dll ^
    -lws2_32 -lws2_32 -luser32 -lwininet -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/OpenURLPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause