# Isolated official Streamline initialization probe

## Shared-frame transport extension

`shared_frame_transport` provides three slots of same-adapter shared DEFAULT
textures plus ready/released GPU fences. `shared_frame_contract` validates
frame identities, generation, camera values and ordered ownership.
`shared_constants` converts pointer-free metadata into official SDK constants.

Run `run-transport.ps1 -Mode 5080` or `-Mode 5080-4k` from build/m348-streamline
for bounded, no-window two-process synthetic tests. CTest exercises WARP only,
plus CPU contracts. The hardware runner refuses tests while HTGame is running.
The transport itself never maps pixels to the CPU; staging and readback are
test-only. Ordinary shared heaps are used, not cross-adapter heaps.

Callers must record/export the exact same-frame NR and guide tuple, submit it
before ready signaling, and preserve all source allocations until that signal
completes. A failed or unconfirmed submission must disconnect the generation,
not be cancelled as if it never ran. Slot/command allocator reuse requires
both GPU directions complete. Metadata is published after enqueuing ready.
Consumers copy into private inputs before releasing shared slots; private inputs
need a separate official FG completion fence before subsequent modification.
Resize requires a new, drained generation, not in-place descriptor changes.

This extension is not yet connected to real NR export or official FG execution.
See failure-records/shared-transport-m348-20261021/REPORT.md for exact evidence.

## Initialization probe

This is a standalone executable, not a game add-on or frame presenter. It uses
the signed NVIDIA Streamline SDK v2.14.16 in a separate process, never rebinds
the game's existing Streamline device, and accepts only an exact RTX 5080 match.

Modes require absolute existing runtime and log directories:

- `--verify`: NVIDIA signature validation and exported functions; no `slInit`.
- `--initialize-5080`: independent SL initialization, device selection, feature
  availability, Reflex capability, and maximum FG multiplier query.
- `--hidden-swapchain-5080`: additionally requests five generated frames and
  creates a hidden HWND swapchain through official device/factory proxies.
  Verifies proxy identity and that no frame was presented. It does not render.

Use `build/m348-streamline/build-probe.ps1` and `run-probe.ps1` from the repository
root. The runner checks signatures, refuses GPU tests while HTGame is running,
starts the helper hidden, enforces a 45-second timeout on that helper only,
and records before/after game DLL and configuration hashes. CTest is CPU-only.

Successful initialization is not proof of FG inference, frame pacing, shared
texture transport, image quality, game compatibility, or input latency. Resolving
the Reflex functions is not executing them or integrating the game's input loop.
No generated-frame Present loop or fabricated source markers exist in this tool.

Official runtime files remain in the verified SDK dependency directory; they
are not copied into either game installation. Full trial evidence is under
`failure-records/official-streamline-m348-20261021/`.
