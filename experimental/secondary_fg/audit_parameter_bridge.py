import hashlib
import json
from pathlib import Path

import capstone
import lief


ROOT = Path(__file__).resolve().parents[2]
SDK_BLOB = "3a6fe22cddaf8e0b961d0b2e48b3596db80b0632"
PAIRS = {
    "NVSDK_NGX_Parameter_GetI": "probe_parameter_get_int",
    "NVSDK_NGX_Parameter_SetUI": "probe_parameter_set_uint",
    "NVSDK_NGX_Parameter_SetULL": "probe_parameter_set_ull",
    "NVSDK_NGX_Parameter_SetD3d12Resource": "probe_parameter_set_resource",
    "NVSDK_NGX_Parameter_SetF": "probe_parameter_set_float",
    "NVSDK_NGX_Parameter_SetVoidPointer": "probe_parameter_set_pointer",
}


def instructions(binary, name):
    symbol = next(symbol for symbol in binary.symbols if symbol.name == name)
    section = list(binary.sections)[symbol.section_idx - 1]
    later = [
        other.value
        for other in binary.symbols
        if other.section_idx == symbol.section_idx and other.value > symbol.value
    ]
    end = min(later, default=len(section.content))
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    return list(decoder.disasm(bytes(section.content)[symbol.value:end], symbol.value))


def method_offsets(binary, name):
    return sorted({
        operand.mem.disp
        for instruction in instructions(binary, name)
        if instruction.mnemonic == "mov"
        for operand in instruction.operands
        if operand.type == capstone.x86.X86_OP_MEM
        and operand.mem.base == capstone.x86.X86_REG_RAX
        and operand.mem.index == 0
    })


def main():
    library = (ROOT / "ext/ngx/nvsdk_ngx_s.lib").read_bytes()
    blob_hash = hashlib.sha1(b"blob " + str(len(library)).encode() + b"\0" + library).hexdigest()
    if blob_hash != SDK_BLOB:
        raise RuntimeError("SDK library does not match the pinned official Git blob")
    official_path = ROOT / "build/sdk-audit/nvsdk_ngx_parameters_lib.obj"
    bridge_path = ROOT / "build/parameter_bridge.obj"
    official = lief.COFF.parse(official_path)
    bridge = lief.COFF.parse(bridge_path)
    results = []
    for official_name, bridge_name in PAIRS.items():
        expected = method_offsets(official, official_name)
        actual = method_offsets(bridge, bridge_name)
        if len(expected) != 1 or actual != expected:
            raise RuntimeError(f"ABI slot mismatch: {official_name}: {expected} != {actual}")
        results.append({"official": official_name, "bridge": bridge_name, "vtable_offset": actual[0]})
    print(json.dumps({
        "status": "PASS",
        "scope": "Static ABI slot comparison only; not a GPU inference test",
        "sdk_git_blob": blob_hash,
        "bridge_sha256": hashlib.sha256(bridge_path.read_bytes()).hexdigest(),
        "methods": results,
    }, indent=2))


if __name__ == "__main__":
    main()
