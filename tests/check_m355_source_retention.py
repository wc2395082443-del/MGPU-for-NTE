"""Retention wiring guards; runtime recovery still needs game validation."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = root / "experimental/official_fg"
channel = (folder / "official_channel.hpp").read_text(encoding="utf-8")
host = (folder / "official_host.cpp").read_text(encoding="utf-8")
test = (folder / "source_history_test.cpp").read_text(encoding="utf-8")
checks = {
    "rolling capacity unchanged, not a blind quota increase": "marker_history = 64" in channel,
    "pending tuple retention bounded to transport slots": "std::array<source_frame_state, shared_slots> retained_" in channel,
    "unseen retained frame has no fabricated bits": "source_frame_state{frames[index], 0}" in channel,
    "all actual source phases still required": "entry.bits == 31u" in channel,
    "retention reads only published tuple identities": "packet.sequence == sequence && packet.generation == channel.manifest.generation" in channel,
    "publication checked before and after snapshot": "latest == read_atomic(channel.published)" in channel,
    "retention snapshot attempts bounded": "attempt < 3" in channel,
    "retention refreshed before validation": host.index("refresh_retention();\n                        updated = history;") > 0,
    "post-SDK commit refreshes rather than clobbers retained state": "refresh_retention();\n                            require(history.note(marker.frame, marker.kind)" in host and "history = updated" not in host,
    "source sleep still authentic and acknowledged": "runtime.source_sleep(marker.frame)" in host and "channel.sleep_completed" in host,
    "missing simulation still fails after bounded wait": "GetTickCount64() + 100" in host and "missing real source simulation markers" in host,
    "helper waits on actual source simulation rather than diagnostic render end":
        "source.simulation_ready(packet.color_frame)" in host and "source.complete(packet.color_frame)" not in host,
    "failure exposes rolling owner and retained identity": "rolling_owner=%u latest_sleep=%u" in host,
    "negative control executes rolling-only expiry": "negative control: old rolling-only history loses pending1312" in test,
    "late phases cannot fabricate render start": "still reject render end without actual render start" in test,
    "main IPC contract remains checked": "sizeof(official_channel) == 12488 && channel_version == 3" in test,
    "own OSD stays removed": "output_osd.hpp" not in host and "kBufferTypeUIAlpha" not in host,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
