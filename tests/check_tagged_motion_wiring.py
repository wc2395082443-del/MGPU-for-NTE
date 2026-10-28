from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
tags = (root / "src/sl_tags.cpp").read_text(encoding="utf-8")
copy = gpu.split("void stream_mvec_copy(", 1)[1].split("// ---- R300d", 1)[0]
cached = tags.split("bool find_main_slots(", 1)[1].split("void install_now()", 1)[0]
tag_capture = gpu.split("void stream_mvec_copy_tagged(", 1)[1].split("// ---- R26:", 1)[0]
checks = {
    "cached scan is confined to main-image writable non-executable data":
        "GetModuleHandleW(nullptr)" in cached and "IMAGE_SCN_MEM_WRITE" in cached
        and "IMAGE_SCN_MEM_EXECUTE" in cached and "32u * 1024u * 1024u" in cached,
    "unidentified cached pointers still require uniqueness and no protection changes":
        "matches.unique_slot()" in cached and "InterlockedCompareExchangePointer(slot, hook, target)" in cached
        and not any(value in cached for value in ("VirtualProtect", "WriteProcessMemory", "CreateRemoteThread")),
    "only PCL can select a verified main-game caller cache":
        "scanned && index == 7 ? verified_pcl_marker_slot(target)" in cached
        and "pcl_marker_route_matches" in cached and "*slot == target ? slot : nullptr" in cached,
    "restore does not clobber changed pointer slots":
        "InterlockedCompareExchangePointer(entry.slot, entry.original, entry.hook)" in tags,
    "tag callback occurs before forwarding both original calls":
        tags.index("read_array(tags, n, cmd, read_identity(frame, viewport));", tags.index("int hook_stff(")) < tags.index("g_real_stff(frame, viewport, tags, n, cmd)")
        and tags.index("read_array(tags, n, cmd, read_identity(nullptr, viewport));", tags.index("int hook_st(")) < tags.index("g_real_st(viewport, tags, n, cmd)"),
    "tagged priority is checked before any recorded copy":
        copy.index("mvec_source_priority.accepts") < copy.index("gl->CopyTextureRegion"),
    "authority is granted only after recording a successful copy":
        copy.index("gl->CopyTextureRegion") < copy.index("mvec_source_priority.recorded"),
    "tag scopes are restored and copy-source state is still tagged":
        "t_tag_capture_scope = previous_scope;" in gpu
        and 't_tag_capture_scope ? "Streamline-tag"' in gpu,
    "one-time reset waits for the tagged sealed frame":
        "f >= tagged_switch && s.nr_tag_reset_frame != tagged_switch" in gpu,
    "native interface, shape and extent are validated before tagged copy":
        tag_capture.index("mgpu::tagged_motion::query_resource") < tag_capture.index("description.Dimension")
        < tag_capture.index("mgpu::tagged_motion::whole_extent") < tag_capture.index("stream_mvec_copy(cmd_list_v"),
    "zero resource header exception is bounded to the observed v1 texture layout":
        "out.tag_version == 1u && out.resource_type == 0u" in tags
        and "memcmp(r, zero_header, BASE_SIZE) == 0" in tags,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
