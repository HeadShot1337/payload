@echo off
if not exist build mkdir build

echo [+] Keylogger Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/KeyloggerPlugin.dll ^
    -lws2_32 -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/KeyloggerPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause