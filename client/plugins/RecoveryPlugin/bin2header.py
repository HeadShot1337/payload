import sys

def main():
    if len(sys.argv) < 3:
        return

    try:
        with open(sys.argv[1], "rb") as f:
            data = f.read()

        with open(sys.argv[2], "w") as f:
            f.write('#pragma once\n\n')
            f.write('// Auto-generated payload header\n')
            f.write('static const unsigned char PAYLOAD_DLL_BYTES[] = {\n')
            for i, b in enumerate(data):
                f.write(f'0x{b:02x},')
                if (i + 1) % 16 == 0:
                    f.write('\n')
            f.write('\n};\n')
    except:
        pass

if __name__ == "__main__":
    main()
