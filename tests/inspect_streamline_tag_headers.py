import argparse
import ctypes
import json
import struct
import time
import uuid
from ctypes import wintypes


class ClientId(ctypes.Structure):
    _fields_ = [("process", ctypes.c_void_p), ("thread", ctypes.c_void_p)]


class ThreadInfo(ctypes.Structure):
    _fields_ = [("exit_status", wintypes.LONG), ("teb", ctypes.c_void_p),
                ("client", ClientId), ("affinity", ctypes.c_size_t),
                ("priority", wintypes.LONG), ("base_priority", wintypes.LONG)]


def main():
    parser = argparse.ArgumentParser(description="Bounded read-only Streamline tag-header inventory")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--threads", type=int, nargs="+", required=True)
    args = parser.parse_args()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    native = ctypes.WinDLL("ntdll")
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenThread.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    native.NtQueryInformationThread.argtypes = [wintypes.HANDLE, wintypes.ULONG,
                                              ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p]
    native.NtQueryInformationThread.restype = wintypes.LONG
    process = kernel.OpenProcess(0x0410, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    report = {"pid": args.pid, "mode": "read_only_unsuspended", "bytes_read": 0,
              "failures": 0, "headers": []}
    budget = 16 * 1024 * 1024

    def read(address, count):
        nonlocal budget
        if not address or count > budget:
            return None
        budget -= count
        buffer = ctypes.create_string_buffer(count)
        received = ctypes.c_size_t()
        if not kernel.ReadProcessMemory(process, address, buffer, count, ctypes.byref(received)):
            report["failures"] += 1
            return None
        report["bytes_read"] += received.value
        return buffer.raw[:received.value]

    tag_guid = uuid.UUID("4c6a5aad-b445-496c-87ff-1af3845be653").bytes_le
    seen = set()
    inspected_pages = set()
    try:
        for iteration in range(3):
            for thread_id in args.threads[:8]:
                thread = kernel.OpenThread(0x0040, False, thread_id)
                if not thread:
                    report["failures"] += 1
                    continue
                try:
                    info = ThreadInfo()
                    status = native.NtQueryInformationThread(thread, 0, ctypes.byref(info), ctypes.sizeof(info), None)
                    if status < 0 or info.client.process != args.pid:
                        report["failures"] += 1
                        continue
                    tib = read(info.teb, 24)
                    if tib is None or len(tib) != 24:
                        continue
                    _, stack_base, stack_limit = struct.unpack("<QQQ", tib)
                    if stack_base <= stack_limit:
                        continue
                    length = min(stack_base - stack_limit, 512 * 1024)
                    start = stack_base - length
                    data = read(start, length)
                    if data is None:
                        continue
                    pages = []
                    for (pointer,) in struct.iter_unpack("<Q", data[:len(data) - len(data) % 8]):
                        if pointer % 8 != 0 or not (0x200000000 <= pointer < 0x10000000000):
                            continue
                        page = pointer & ~4095
                        if stack_limit <= page < stack_base or page in inspected_pages:
                            continue
                        inspected_pages.add(page)
                        content = read(page, 4096)
                        if content is not None and len(content) == 4096:
                            pages.append(content)
                        if len(pages) >= 128:
                            break
                    data += b"".join(pages)
                    position = data.find(tag_guid)
                    while position >= 0 and len(report["headers"]) < 24:
                        base = position - 8
                        if base >= 0 and base + 64 <= len(data) and (start + base) % 8 == 0:
                            version, resource, buffer_type, life, top, left, width, height = struct.unpack_from("<QQIiIIII", data, base + 24)
                            if buffer_type < 32 and life in (0, 1, 2):
                                resource_data = read(resource, 80)
                                if resource_data is not None and len(resource_data) == 80:
                                    resource_guid = str(uuid.UUID(bytes_le=resource_data[8:24]))
                                    resource_version = struct.unpack_from("<Q", resource_data, 24)[0]
                                    resource_type = resource_data[32]
                                    state = struct.unpack_from("<I", resource_data, 64)[0]
                                    identity = (buffer_type, version, resource_guid, resource_version, resource_type, life, top, left, width, height)
                                    if identity not in seen:
                                        seen.add(identity)
                                        report["headers"].append({"thread": thread_id, "tag_version": version,
                                            "buffer_type": buffer_type, "lifecycle": life,
                                            "extent": [top, left, width, height], "resource_guid": resource_guid,
                                            "resource_version": resource_version, "resource_type": resource_type,
                                            "state": hex(state)})
                        position = data.find(tag_guid, position + 16)
                finally:
                    kernel.CloseHandle(thread)
            if iteration < 2:
                time.sleep(0.1)
    finally:
        kernel.CloseHandle(process)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
