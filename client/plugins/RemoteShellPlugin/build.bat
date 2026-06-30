@echo off
if not exist build mkdir build

echo [+] Remote Shell Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/RemoteShellPlugin.dll ^
    -lws2_32 -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/RemoteShellPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause