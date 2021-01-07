import sys
import os

def pad_bios(in_name, out_name):
    print("Reading %s..." % in_name)
    with open(in_name, "rb") as f:
        indata = f.read()
    if len(indata) > (512 * 1024):
        print("Input file %s is too large (%u bytes)", in_name, len(indata))
        sys.exit(1)

    padding_size = (512 * 1024) - len(indata)
    padding = b'\0' * padding_size
    print("Padding with %u bytes" % padding_size)

    print("Writing %s..." % out_name)
    with open(out_name, "wb") as f:
        f.write(indata)
        f.write(padding)
        

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: %s <input filename> <output filename>" % sys.argv[0])
        sys.exit(1)

    pad_bios(sys.argv[1], sys.argv[2])
    sys.exit(0)