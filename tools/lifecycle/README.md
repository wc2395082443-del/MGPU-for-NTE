# Offline lifecycle measurement tools

`analyze_fg_gpu_audit.py <capture-folder> --require-formal-complete` reads the four FG CSV/status files emitted by the diagnostic helper plus the capture receipt. GPU elapsed, source-fence identity, clock-quality checks and missing boundary records are reported separately. `--require-complete` checks the whole recorded range and can reject unmatched pre-window boundary calls.

Run `python -m unittest discover -s tools/lifecycle -p test_fg_gpu_audit.py` for clock rejection and exact fence-identity checks. No GPU or game process is started.

`export_standalone_timeline.py` and the HTML/JS templates package already-exported timeline records into a self-contained offline viewer. They require Python3 and NumPy. `scripts/Publish-Lifecycle-Evidence.py` selects five consecutive source-ready intervals containing5.000s, exports compact data, renders the SVG and packages three version viewers from locally archived captures.

The full ETLs, binary traces and runtime logs remain in the maintainer's archive; the repository contains compact evidence and public summaries. These scripts do not acquire a new game capture. Actual NGX timestamp instrumentation is in `experimental/official_fg/fg_gpu_audit.cpp`; it records only when its named event is explicitly triggered.
