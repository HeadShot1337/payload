@echo off
if not exist build mkdir build

echo [+] Information Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/information.dll ^
    -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi -lsetupapi -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/information.dll
) else (
    echo [-] Hata Olustu!
)
pause