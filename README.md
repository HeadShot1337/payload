# C++ Browser Extractor

This is a C++ port of the Rust browser extractor project.

## Project Structure

- `injector/`: The main executable that spawns the browser and performs the extraction.
- `proxydll/`: The DLL that is injected into the browser to retrieve the v20 App-Bound master key.

## Build Instructions (MinGW-w64)

### 1. Download Dependencies

Ensure `nlohmann/json` (json.hpp) and `sqlite3` amalgamation (sqlite3.c, sqlite3.h) are in the source directories.

### 2. Build the Proxy DLL

The DLL is now minimal and does not require SQLite.

```bash
x86_64-w64-mingw32-g++ -O2 -shared proxydll/src/lib.cpp -o payload.dll -lbcrypt -lcrypt32 -lole32 -loleaut32 -lshlwapi -static
```

### 3. Build the Injector

The injector now handles all extraction logic and requires SQLite.

```bash
# 1. Compile sqlite3 separately
x86_64-w64-mingw32-gcc -O2 -c injector/src/sqlite3.c -o injector/src/sqlite3.o

# 2. Build the injector and link with sqlite3.o
x86_64-w64-mingw32-g++ -O2 injector/src/main.cpp injector/src/sqlite3.o -o injector.exe -lbcrypt -lcrypt32 -lole32 -loleaut32 -lshlwapi -static
```

## Usage

1. Build both components as described above.
2. Run `injector.exe`. It will look for `payload.dll` in the same directory.
3. Extracted data will be saved in folders named after the browsers (e.g., `Chrome/Default/passwords.txt`).

Options:
- `-b, --browser <name>`: Target a specific browser (chrome, edge, brave, all). Default is `all`.
