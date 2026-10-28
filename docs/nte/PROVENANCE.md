# Release provenance

Upstream: https://github.com/maohgad-web/Neural-coprocessor

Fork base: `671e1a935d9db7ecad3627f36199504fc0437527` (main, CN Panel). The NTE work used archived upstream-derived source snapshots, imported into this fork with their source and binary identities.

## Selected implementation

- Main module: archived **M364 rollback-M358 source**. Production files in `src/` match its recorded snapshot. M364 reverts the M358 early official-export gate and two-frame source lead limit; safety retirement remains.
- Helper: archived **M363 mouse-passthrough source**, placed in `experimental/official_fg/`. Main-linked bridge/transport contracts are checked against the M364 snapshot before publication; helper-only output-window changes retain M362 isolation.
- `baseline/gpu1_context.cpp` is the M358 negative-control fixture for M364 dispatch tests.
- No M365 source-lookahead, M366 source-CVar request or M367 source-options wrapper is included.

The release uses the previously deployed binaries:

| Artifact | SHA256 |
| --- | --- |
| `nvngx.dll_mgpu_bridge.addon64` (M364) | `3A6C8FF99FD5D1361A07A9D5D91CE8164326BEDF5521B5151CE886F53BC73D59` |
| `mgpu_official_host.exe` (M363) | `21B70B40195D264E2679D652B0F14A1C0E005F2E95EAF9334F339BE4309B1264` |

Rebuilds can differ in PE timestamps, compiler paths and optimization output; no byte-reproducible build is claimed. A new build must be tested separately before replacing a published binary.

This publication changes only test fixture paths in the root CMake and two helper negative-control includes, to make archived baselines live inside this repository. Dependency/bootstrap, release tooling and documentation are new. The deployed M364 production `src/` files and main-linked official bridge/transport files are checked byte-for-byte against their archive; M363 helper production files retain their archived content.

M362 input isolation improved the user's turning responsiveness; M363 added mouse passthrough. M364 was restored after a later M366 exception report. The publication checks cover the source build, packaging and archived game records.

Hosted CI was run twice and the same asynchronous drain stress case failed both times (74/75 main cases); local publication checks passed 75 main + 20 helper cases. The selected prebuilt artifacts were not altered in response. See BUILD.md for the observable failure and scope.

The release includes source, compact measurement data and test output. Runtime/model dependencies are supplied locally under their own licenses; see [DEPENDENCIES.md](DEPENDENCIES.md).
