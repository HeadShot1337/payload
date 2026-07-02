@echo off
if not exist build mkdir build

echo [+] ProxyDLL Derleniyor...
g++ -O2 -shared proxydll.cpp -o build/proxydll.dll -lole32 -loleaut32 -lcrypt32 -static

echo [+] SQLite3 Derleniyor...
gcc -O2 -c sqlite3.c -o build/sqlite3.o

echo [+] Recovery Plugin Derleniyor...
g++ -O2 -shared main.cpp build/sqlite3.o -o build/RecoveryPlugin.dll ^
    -lws2_32 -lcrypt32 -lbcrypt -lshlwapi -lole32 -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/RecoveryPlugin.dll
) else (
    echo [-] Hata Olustu!
)
pause