import argparse
import ctypes
import json
import struct
import sys
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "devtools" / "pe-tools"))
import pefile


def main():
    parser = argparse.ArgumentParser(description="Read-only main-image Streamline pointer inventory")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--exe-base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--interposer", required=True)
    parser.add_argument("--interposer-base", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()

    interposer = pefile.PE(args.interposer, fast_load=True)
    interposer.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    names = {b"slSetTag", b"slSetTagForFrame", b"slEvaluateFeature"}
    addresses = {
        symbol.name.decode("ascii"): args.interposer_base + symbol.address
        for symbol in interposer.DIRECTORY_ENTRY_EXPORT.symbols
        if symbol.name in names and symbol.forwarder is None
    }
    image = pefile.PE(args.exe, fast_load=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    process = kernel.OpenProcess(0x0410, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    result = {"pid": args.pid, "mode": "read_only", "exports": addresses,
              "slots": [], "bytes_read": 0, "unreadable_pages": 0, "truncated": False}
    budget = 32 * 1024 * 1024
    try:
        for section in image.sections:
            if not section.Characteristics & 0x80000000 or section.Characteristics & 0x20000000:
                continue
            size = min(section.Misc_VirtualSize, image.OPTIONAL_HEADER.SizeOfImage - section.VirtualAddress)
            if size < 0:
                raise ValueError("Section exceeds main image bounds")
            for offset in range(0, size, 4096):
                count = min(4096, size - offset)
                if count > budget:
                    result["truncated"] = True
                    break
                budget -= count
                address = args.exe_base + section.VirtualAddress + offset
                buffer = ctypes.create_string_buffer(count)
                received = ctypes.c_size_t()
                if not kernel.ReadProcessMemory(process, address, buffer, count, ctypes.byref(received)):
                    result["unreadable_pages"] += 1
                    continue
                data = buffer.raw[:received.value]
                result["bytes_read"] += received.value
                for name, target in addresses.items():
                    needle = struct.pack("<Q", target)
                    position = data.find(needle)
                    while position >= 0:
                        if (address + position) % 8 == 0:
                            result["slots"].append({"name": name, "address": hex(address + position),
                                                    "rva": hex(address + position - args.exe_base),
                                                    "section": section.Name.rstrip(b"\0").decode("ascii", "replace")})
                        position = data.find(needle, position + 8)
            if result["truncated"]:
                break
    finally:
        kernel.CloseHandle(process)
        image.close()
        interposer.close()
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
