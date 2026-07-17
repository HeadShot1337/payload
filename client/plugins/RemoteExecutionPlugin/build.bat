@echo off
if not exist build mkdir build

echo [+] Remote Execution Plugin Derleniyor...
g++ -O2 -shared main.cpp -o build/RemoteExecutionPlugin.dll ^
    -lws2_32 -lurlmon -lshell32 -ladvapi32 -luser32 -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/RemoteExecutionPlugin.dll
) else (
    echo [-] Hata Olustu!
)
