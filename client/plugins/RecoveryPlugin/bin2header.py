import sys
import os

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_header>")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    if not os.path.exists(input_path):
        # Create a dummy header if input doesn't exist to avoid compilation break
        with open(output_path, "w") as f:
            f.write('#pragma once\n\n')
            f.write('// Dummy payload header (Input file not found during generation)\n')
            f.write('static const unsigned char PAYLOAD_DLL_BYTES[] = { 0x00 };\n')
        return

    try:
        with open(input_path, "rb") as f:
            data = f.read()

        with open(output_path, "w") as f:
            f.write('#pragma once\n\n')
            f.write('// Auto-generated payload header from ' + os.path.basename(input_path) + '\n')
            f.write(f'// Size: {len(data)} bytes\n\n')
            f.write('static const unsigned char PAYLOAD_DLL_BYTES[] = {\n')

            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                line = '    ' + ', '.join(f'0x{b:02x}' for b in chunk)
                if i + 16 < len(data):
                    line += ','
                f.write(line + '\n')

            f.write('};\n')

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
