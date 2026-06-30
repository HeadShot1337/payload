@echo off
if not exist build mkdir build

echo [+] File Manager Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/FileManagerPlugin.dll ^
    -lws2_32 -lshell32 -lole32 -static-libgcc -static-libstdc++ -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/FileManagerPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause
