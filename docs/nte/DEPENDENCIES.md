# Dependencies and distribution

Our code retains the upstream MIT license. ReShade header notices, ImGui notices and Cost-Scaler shader notices remain in `THIRD_PARTY.md`; Streamline header license is in `ext/streamline/LICENSE.txt`.

## Included in the binary release

- M364 bridge add-on and M363 output helper compiled from the published source lineage.
- The project's `mgpu_depth_tap.fx`, configuration templates, installation/rollback and local runtime-import scripts, hashes and licenses.

## Supplied by the user

- Windows x64, DirectX 12, current compatible NVIDIA driver, and Microsoft Visual C++ 2015–2022 x64 runtime.
- An add-on-enabled ReShade compatible with API 20. The historical rig used ReShade 6.8.0. Install it for the game's D3D12 executable; this release does not replace the game's core DLLs.
- NVIDIA NR runtime/model assets expected by upstream (`mgpu/nvngx_dlssnr.dll` and the appropriate installed NGX stack). They are not hosted here.
- Official Streamline/DLSS-G runtimes in `mgpu-official/runtime`: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`.

The helper was built against Streamline 2.14.16; its deployed runtime includes signed OTA plugin builds. `release/nte/runtime-requirements.json` pins the exact tested file hashes. `Import-Runtime.ps1` copies only matching signed files from a directory on the user's own machine. A release ZIP from NVIDIA may contain different plugin versions: a mismatch is reported rather than silently described as the tested combination. No arbitrary runtime download, patching or signature bypass occurs.

Official sources: [Streamline](https://github.com/NVIDIA-RTX/Streamline/releases), [DLSS SDK](https://github.com/NVIDIA/DLSS), [ReShade](https://reshade.me/), [VC++ runtime](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

NVIDIA runtime/model binaries are excluded from both Git and this release, following the upstream distribution policy. Users are responsible for obtaining and using them under their applicable terms. The installer does not fetch game files or models and does not alter anti-cheat services.

## Hardware/build restrictions

The exact M363 binary currently requires `NVIDIA GeForce RTX 5080` as its neural/output adapter. Do not advertise general RTX 50-series/PRO or arbitrary multi-GPU support from this release. The test rig uses an RTX 4080 SUPER as the game adapter. Some source hooks validate the known game PE/build; new game updates may require adaptation.
