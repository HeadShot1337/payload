@echo off
if not exist build mkdir build

echo [+] Night Client Derleniyor (GCC) (BU OZEL OLARAK SIRKET ICIN URETILMISTIR)...

g++ -O2 -I./include src/*.cpp -o build/client.exe ^
    -lws2_32 -ladvapi32 -luser32 -lole32 -lshell32 -lcomctl32 -luxtheme -static

if %ERRORLEVEL% EQU 0 (
    echo [!] Derleme Basarili: build/client.exe
) else (
    echo [-] Hata Olustu!
)
pause
