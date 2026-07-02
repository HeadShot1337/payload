@echo off
if not exist build mkdir build

echo [+] Process Manager Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/ProcessManagerPlugin.dll ^
    -lws2_32 -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/ProcessManagerPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause
