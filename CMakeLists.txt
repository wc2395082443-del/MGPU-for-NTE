# MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
#
# Target: mgpu_bridge.addon64  (a ReShade 64-bit add-on, API 20)
#
# The .addon64 extension REPLACES .dll - the output is NOT
# mgpu_bridge.addon64.dll. ReShade loads *.addon64 files; a file with the
# wrong name goes green in CI and silently never loads on the rig. CI
# verifies the exact output name for this reason.

cmake_minimum_required(VERSION 3.21)

project(mgpu_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mgpu_queue_trace STATIC src/queue_dependency_trace.cpp)
target_compile_definitions(mgpu_queue_trace PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
add_executable(mgpu_m343_etl_exporter tools/m343_etl_raw_exporter.cpp)
add_executable(mgpu_pipeline_trace_layout tools/pipeline_trace_layout.cpp)
add_executable(mgpu_pipeline_etl_integrity tools/etl_integrity.cpp)
target_link_libraries(mgpu_pipeline_etl_integrity PRIVATE advapi32)
target_compile_definitions(mgpu_pipeline_trace_layout PRIVATE NOMINMAX)
target_link_libraries(mgpu_m343_etl_exporter PRIVATE advapi32)

# Source list follows the closed manifest (P0_RECORD.md, section 01):
# T2 adds diag/adapter, T3 adds worker/gpu1_context. runtime_probe lands
# with its own task; assets/ is deployed by hand and never compiled.
add_library(mgpu_bridge SHARED
    src/aligned_input.cpp
    src/nonblocking_work_trace.cpp
    src/frame_measurement.cpp
    src/game_work_trace.cpp
    src/native_work_observer.cpp
    src/depth_normalizer.cpp
    src/dllmain.cpp
    src/diag.cpp
    src/adapter.cpp
    src/adapter_selection.cpp
    src/worker.cpp
    src/gpu1_context.cpp
    # P9.1: the lateral acquisition probe. Self-contained - it holds every
    # ReShade event callback the probe needs and nothing else, so
    # gpu1_context.cpp keeps its rule of holding no ReShade types and is
    # not touched by this feature at all.
    src/probe.cpp
    # R101: the calibrator. Reads the parameter block the game already hands
    # DLSS every frame, so acquisition stops being inferred. Self-contained
    # like probe.cpp - it holds the import-table interception and the reader
    # and nothing else, so neither gpu1_context nor probe gains a dependency.
    src/calibrator.cpp
    # R108: the idle screen. Draws with scissored clears only - no shader,
    # no font file, no ImGui - so it renders in exactly the situation where
    # nothing else can: an empty reshade-shaders folder.
    src/screen.cpp
    # SL1: the Streamline interposer probe. Self-contained like the three
    # above - it holds no ReShade types and no gpu1_context types, every
    # interface arrives as a void*, and the whole instrument is one
    # documented QueryInterface. No new link library: version.dll is
    # resolved at run time, the way d3dcompiler_47 already is.
    src/sl_probe.cpp
    # SLT1: the Streamline tag tap. Self-contained like the four above - no
    # ReShade type, no gpu1_context type, no Streamline header and no link
    # library; every Streamline struct is read by byte offset, and every
    # offset is quoted from the public header beside it. OFF unless
    # SLTags= is set in mgpu.ini, so this source compiles into every build
    # and touches nothing in any of them until it is asked for.
    src/sl_tags.cpp
    # R208: the experimental FP16 input path. No gpu1_context type;
    # gpu1_context hands it D3D12 objects. OFF unless NRInput16=4 is in
    # mgpu.ini: no texture, no kernel, no extra pass.
    src/nr16.cpp
    # D1 (0.3.0 R&D): the discovery calibrator. OFF unless Discovery=1 in
    # mgpu.ini. Holds no ReShade type and no gpu1_context type.
    src/discovery.cpp
    # D1.5a (0.3.0 R&D): the read-only latency probe. OFF unless
    # LatencyProbe=1 in mgpu.ini. Holds no ReShade type and no gpu1_context type.
    src/latency_probe.cpp
    # D2.0 (0.3.0 R&D): log queue, journal, stall watch, our Reflex for
    # case-B titles. Each OFF unless its key is set (Journal, StallWatch,
    # OwnReflex). No ReShade type and no gpu1_context type in any of them.
    src/log_queue.cpp
    src/journal.cpp
    src/stall_watch.cpp
    src/own_reflex.cpp
    src/native_reflex.cpp
    src/source_fg_guard.cpp
    # D2.0 NR step one: read-only frame-gen vector map, OFF unless NRFrameFilter=1.
    src/fg_map.cpp
    # DX11 transport-hop probe (DX11_CONTRACT_LEDGER.md). OFF unless Dx11Probe=1.
    # Holds no ReShade type and no gpu1_context type; nothing on GPU 1.
    src/dx11_probe.cpp
    # DX11 producer (DX11_CONTRACT_LEDGER.md section 7, R227). OFF unless DX11=1
    # and only ever called from a D3D11 game device; owns a D3D12 device on the
    # game adapter and feeds the existing stream producer. No ReShade type.
    src/dx11_producer.cpp
    # R253: motion-vector census for D3D11 titles (DX11_CONTRACT_LEDGER.md section 13).
    # Observe-only, OFF unless MVecCensus=1; registers its own ReShade events.
    src/mvec_census.cpp
    # R279: TexCensus=1 instrument (remove with the file pair when done)
    src/tex_census.cpp
    # R280: the virtual velocity target for D3D12 titles without an NGX contract (MVEC_D3D12_LEDGER.md)
    src/mvec_extract.cpp
    # PanelOverPicture (N01950): the live panel over the neural frame. OFF unless
    # PanelOverPicture=1; registers its own ReShade event only then.
    src/live_panel.cpp
    # R300 - T1.19 (Dom / LordZaxxon), docs/R300_T119.md. Every path OFF unless its mgpu.ini key is set:
    # t119_keys.cpp is the one place the keys are read (from probe's ini read).
    src/t119_keys.cpp
    src/temporal_diag.cpp         # R300a1-R300a8: temporal record + gated policy (TemporalDiag, TemporalMV, ...)
    src/perf_diag.cpp             # R300e1-R300e6: consumer timing + T1.12 prefetch (Perf11, PerfPrefetch)
    src/feature_diag.cpp          # R300f1: scene-feature silence detector (FeatureDiag)
    src/producer_diag.cpp         # R300g1-R300g6: producer (GPU 0) cost bookkeeping (ProducerDiag)
    src/producer_gpu_timers.cpp   # R300h: GPU 0 timestamps (Gpu0Timers)
    experimental/secondary_fg/dlss_fg_stage.cpp
    experimental/secondary_fg/parameter_bridge.cpp
    experimental/official_fg/shared_frame_transport.cpp
    experimental/official_fg/official_bridge.cpp
)

target_include_directories(mgpu_bridge PRIVATE
    "ext/reshade/include"
)

# ---------------------------------------------------------------------------
# P7.1. The overlay panel's only dependency: two ImGui headers.
#
# "ext/reshade/deps/imgui" used to be listed above and did nothing, because CMake
# silently ignores an include directory that does not exist and __has_include
# then just says no - which is how a correct-looking CMakeLists edit produced no
# change at all in the log. ext/reshade is created by CI from the pinned SHA and
# is not in the repo, so nothing was ever there to find.
#
# We do not vendor ReShade and we compile no ImGui source. reshade.hpp supplies
# the whole ImGui API as a table of function pointers the ReShade runtime fills
# in at register_addon time; imgui.h is needed only for the declarations,
# IMGUI_VERSION_NUM and the struct layouts. So the dependency is imgui.h +
# imconfig.h, fetched into the BUILD tree - no submodule, no build step, and
# nothing third-party committed to this repo.
#
# The version is not negotiable: reshade_overlay.hpp #errors on anything but
# 19250 (ImGui 1.92.5), because the function table is a version-numbered struct
# of raw pointers and a near-miss is a silent ABI mismatch. 3912b3d9 is the
# commit ReShade 18deaa52de0c425a78b329e9cb3c497281cd00ec pins at deps/imgui.
#
# If the fetch fails the build still succeeds - it logs NO OVERLAY PANEL IN THIS
# BUILD and the CTRL+ALT+F8/F9/F11 hotkeys remain the control. The configure
# output below says which of the two you are getting, BEFORE you build.
set(MGPU_IMGUI_SHA "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c")
set(MGPU_IMGUI_DIR "${CMAKE_BINARY_DIR}/ext/imgui")
foreach(_h imgui.h imconfig.h)
    if(NOT EXISTS "${MGPU_IMGUI_DIR}/${_h}")
        # R299c: three attempts (N01950 lost the panel to one failed fetch). Still green
        # on failure by design; CI fails the job instead (build.yml, Configure step).
        foreach(_try 1 2 3)
            file(DOWNLOAD
                 "https://raw.githubusercontent.com/ocornut/imgui/${MGPU_IMGUI_SHA}/${_h}"
                 "${MGPU_IMGUI_DIR}/${_h}"
                 STATUS _mgpu_dl TLS_VERIFY ON)
            list(GET _mgpu_dl 0 _mgpu_code)
            if(_mgpu_code EQUAL 0)
                break()
            endif()
            message(STATUS "MGPU: ${_h} fetch attempt ${_try}/3 failed")
            execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
        endforeach()
        if(NOT _mgpu_code EQUAL 0)
            list(GET _mgpu_dl 1 _mgpu_msg)
            message(WARNING "MGPU: could not fetch ${_h} (${_mgpu_msg}) - building without the overlay panel.")
            file(REMOVE "${MGPU_IMGUI_DIR}/${_h}")
        endif()
    endif()
endforeach()

if(EXISTS "${MGPU_IMGUI_DIR}/imgui.h" AND EXISTS "${MGPU_IMGUI_DIR}/imconfig.h")
    target_include_directories(mgpu_bridge PRIVATE "${MGPU_IMGUI_DIR}")
    message(STATUS "MGPU: overlay panel ENABLED - ImGui headers at ${MGPU_IMGUI_DIR}")
else()
    message(STATUS "MGPU: overlay panel DISABLED - no ImGui headers; hotkeys only.")
endif()
# ---------------------------------------------------------------------------

set_target_properties(mgpu_bridge PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    # $<1:...> suppresses the per-configuration subdirectory that
    # multi-config generators (including "Visual Studio 17 2022", which
    # CI uses) would otherwise append to RUNTIME_OUTPUT_DIRECTORY: the
    # output is build/mgpu_bridge.addon64, not build/Release/...
    # CI locates the artifact by name regardless; this makes the stated
    # path true.
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"
)

target_include_directories(mgpu_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # CI creates ext/reshade/ from crosire/reshade at the pinned SHA
    # (see .github/workflows/build.yml and VENDOR_LOCK.md).
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/reshade
)

target_compile_definitions(mgpu_bridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    # 2026-10-06, Rise of the Tomb Raider (and a reporter's Shadow of the Tomb
    # Raider): the game folder ships msvcp140.dll 14.28.29910 and the process
    # loads that copy, not the system's. Visual Studio 2022 17.10+ made
    # std::mutex's constructor constexpr; a binary built that way crashes at
    # its first mutex use (0xC0000005) against an msvcp140 older than 14.40.
    # The add-on takes a std::mutex in its first device event. This is
    # Microsoft's documented switch for exactly that case. The alternative, a
    # static CRT, is the broader answer and a later milestone.
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
)

# dxgi: factory + EnumAdapters1 (T2). d3d12: the private device (T3).
target_link_libraries(mgpu_bridge PRIVATE
    dxgi
    dxguid
    d3d12
    # V49/V51: the ghost mode. DirectComposition, and D3D11 solely to name an
    # adapter to it - nothing is ever drawn with the D3D11 device. Both are
    # also declared with #pragma comment(lib, ...) inside gpu1_context.cpp, so
    # a build made from src/* without this file still links.
    dcomp
    d3d11
)

# P0: warnings visible, not fatal. A compile error costs the same round
# trip as a logic error and teaches less - keep the first-try bar low.
target_compile_options(mgpu_bridge PRIVATE /W3)

option(MGPU_BUILD_TESTS "Build CPU-only adapter policy tests" OFF)
if(MGPU_BUILD_TESTS)
    add_executable(mgpu_continuous_trace_test tests/continuous_trace_test.cpp)
    target_compile_definitions(mgpu_continuous_trace_test PRIVATE NOMINMAX)
    add_test(NAME mgpu_continuous_trace_test COMMAND mgpu_continuous_trace_test)
    add_executable(mgpu_continuous_measurement_test tests/continuous_measurement_test.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_continuous_measurement_test PRIVATE NOMINMAX)
    add_test(NAME mgpu_continuous_measurement_test COMMAND mgpu_continuous_measurement_test)
    add_test(NAME mgpu_incomplete_measurement_rejected_test COMMAND mgpu_continuous_measurement_test --incomplete)
    add_test(NAME mgpu_legacy_continuous_transition_rejected_test COMMAND mgpu_continuous_measurement_test --legacy-first)
    add_executable(mgpu_continuous_game_work_warp_test tests/continuous_game_work_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_continuous_game_work_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_continuous_game_work_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_continuous_game_work_warp_test COMMAND mgpu_continuous_game_work_warp_test)
    set_tests_properties(mgpu_continuous_game_work_warp_test PROPERTIES TIMEOUT 45)
    add_executable(mgpu_nonblocking_recording_warp_test tests/nonblocking_recording_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_nonblocking_recording_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_nonblocking_recording_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_nonblocking_recording_warp_test COMMAND mgpu_nonblocking_recording_warp_test)
    add_test(NAME mgpu_nonblocking_fine_capacity_warp_test COMMAND mgpu_nonblocking_recording_warp_test --operations 512)
    set_tests_properties(mgpu_nonblocking_recording_warp_test mgpu_nonblocking_fine_capacity_warp_test PROPERTIES TIMEOUT 40)
    add_executable(mgpu_nonblocking_boundaries_warp_test tests/nonblocking_boundaries_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_nonblocking_boundaries_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_nonblocking_boundaries_warp_test PRIVATE d3d12 dxgi dxguid)
    foreach(boundary unsubmitted replay queue-limit delayed-retirement stop-during-scope)
        add_test(NAME mgpu_nonblocking_${boundary}_test COMMAND mgpu_nonblocking_boundaries_warp_test ${boundary})
        set_tests_properties(mgpu_nonblocking_${boundary}_test PROPERTIES TIMEOUT 20)
    endforeach()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu1_context.cpp" _capacity_source)
    string(FIND "${_capacity_source}" "bool present_capacity_gate(" _capacity_begin)
    string(FIND "${_capacity_source}" "bool present_frame(" _capacity_end)
    if(_capacity_begin LESS 0 OR _capacity_end LESS _capacity_begin)
        message(FATAL_ERROR "Official capacity regression cannot find production function")
    endif()
    math(EXPR _capacity_length "${_capacity_end} - ${_capacity_begin}")
    string(SUBSTRING "${_capacity_source}" ${_capacity_begin} ${_capacity_length} OFFICIAL_CAPACITY_IMPLEMENTATION)
    configure_file(tests/official_capacity_test.cpp.in official_capacity_test.cpp @ONLY)
    add_executable(mgpu_official_capacity_test "${CMAKE_CURRENT_BINARY_DIR}/official_capacity_test.cpp")
    target_include_directories(mgpu_official_capacity_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_official_capacity_test COMMAND mgpu_official_capacity_test)
    string(REPLACE "    if (mgpu::official_bridge::selected()) return true;\n" ""
        _old_capacity "${OFFICIAL_CAPACITY_IMPLEMENTATION}")
    if(_old_capacity STREQUAL OFFICIAL_CAPACITY_IMPLEMENTATION)
        message(FATAL_ERROR "Old capacity regression negative control was not produced")
    endif()
    set(OFFICIAL_CAPACITY_IMPLEMENTATION "${_old_capacity}")
    configure_file(tests/official_capacity_test.cpp.in official_capacity_old_test.cpp @ONLY)
    add_executable(mgpu_official_capacity_old_test "${CMAKE_CURRENT_BINARY_DIR}/official_capacity_old_test.cpp")
    target_include_directories(mgpu_official_capacity_old_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_official_capacity_old_rejected COMMAND mgpu_official_capacity_old_test)
    set_tests_properties(mgpu_official_capacity_old_rejected PROPERTIES WILL_FAIL TRUE)
    add_executable(mgpu_game_work_policy_test tests/game_work_policy_test.cpp)
    add_test(NAME mgpu_game_work_policy_test COMMAND mgpu_game_work_policy_test)
    add_executable(mgpu_game_work_trace_warp_test tests/game_work_trace_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_game_work_trace_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_game_work_trace_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_game_work_trace_warp_test COMMAND mgpu_game_work_trace_warp_test)
    add_test(NAME mgpu_game_work_trace_stress_warp_test COMMAND mgpu_game_work_trace_warp_test --stress)
    add_executable(mgpu_game_work_native_boundary_warp_test tests/game_work_native_boundary_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    add_executable(mgpu_native_work_policy_test tests/native_work_policy_test.cpp)
    add_test(NAME mgpu_native_work_policy_test COMMAND mgpu_native_work_policy_test)
    add_executable(mgpu_native_work_abi_test tests/native_work_abi_test.cpp)
    add_test(NAME mgpu_native_work_abi_test COMMAND mgpu_native_work_abi_test)
    add_executable(mgpu_native_work_warp_test tests/native_work_warp_test.cpp src/game_work_trace.cpp src/nonblocking_work_trace.cpp src/native_work_observer.cpp src/frame_measurement.cpp)
    target_compile_definitions(mgpu_native_work_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR MGPU_FINE_TEST_LEARN=1)
    target_link_libraries(mgpu_native_work_warp_test PRIVATE d3d12 dxgi dxguid d3dcompiler)
    add_test(NAME mgpu_native_work_warp_test COMMAND mgpu_native_work_warp_test)
    add_test(NAME mgpu_native_work_stop_warp_test COMMAND mgpu_native_work_warp_test --observer-stop)
    add_test(NAME mgpu_native_work_capacity_warp_test COMMAND mgpu_native_work_warp_test --capacity)
    add_executable(mgpu_trace_log_parts_test tests/trace_log_parts_test.cpp)
    add_test(NAME mgpu_trace_log_parts_test COMMAND mgpu_trace_log_parts_test)
    target_compile_definitions(mgpu_game_work_native_boundary_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_game_work_native_boundary_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_game_work_native_boundary_warp_test COMMAND mgpu_game_work_native_boundary_warp_test)
    add_test(NAME mgpu_game_work_native_reset_after_stop_warp_test COMMAND mgpu_game_work_native_boundary_warp_test --reset-only)
    add_executable(mgpu_frame_measurement_test tests/frame_measurement_test.cpp src/frame_measurement.cpp tests/game_work_poll_stub.cpp)
    target_compile_definitions(mgpu_frame_measurement_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    add_test(NAME mgpu_frame_measurement_test COMMAND mgpu_frame_measurement_test)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/gpu1_context.cpp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu1_context.cpp" _dispatch_source)
    string(FIND "${_dispatch_source}" "static void stream_pump_aligned_locked(" _dispatch_begin)
    string(FIND "${_dispatch_source}" "static bool stream_parallel_compatible" _dispatch_end)
    if(_dispatch_begin LESS 0 OR _dispatch_end LESS _dispatch_begin)
        message(FATAL_ERROR "Worker dispatcher test cannot locate the production implementation")
    endif()
    math(EXPR _dispatch_length "${_dispatch_end} - ${_dispatch_begin}")
    string(SUBSTRING "${_dispatch_source}" ${_dispatch_begin} ${_dispatch_length} WORKER_DISPATCH_IMPLEMENTATION)
    configure_file(tests/worker_dispatch_test.cpp.in worker_dispatch_test.cpp @ONLY)
    add_executable(mgpu_worker_dispatch_test "${CMAKE_CURRENT_BINARY_DIR}/worker_dispatch_test.cpp")
    target_include_directories(mgpu_worker_dispatch_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_worker_dispatch_test COMMAND mgpu_worker_dispatch_test)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/baseline/gpu1_context.cpp" _dispatch_baseline)
    string(FIND "${_dispatch_baseline}" "static void stream_pump_aligned_locked(" _baseline_begin)
    string(FIND "${_dispatch_baseline}" "static bool stream_parallel_compatible" _baseline_end)
    if(_baseline_begin LESS 0 OR _baseline_end LESS _baseline_begin)
        message(FATAL_ERROR "Archived M358 worker dispatcher is missing")
    endif()
    math(EXPR _baseline_length "${_baseline_end} - ${_baseline_begin}")
    string(SUBSTRING "${_dispatch_baseline}" ${_baseline_begin} ${_baseline_length} WORKER_DISPATCH_IMPLEMENTATION)
    configure_file(tests/worker_dispatch_test.cpp.in worker_dispatch_m358_control.cpp @ONLY)
    add_executable(mgpu_worker_dispatch_m358_control "${CMAKE_CURRENT_BINARY_DIR}/worker_dispatch_m358_control.cpp")
    target_compile_definitions(mgpu_worker_dispatch_m358_control PRIVATE MGPU_M358_CONTROL)
    target_include_directories(mgpu_worker_dispatch_m358_control PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_worker_dispatch_m358_control COMMAND mgpu_worker_dispatch_m358_control)
    string(FIND "${_dispatch_source}" "static void stream_prefetch_aligned_locked(" _prefetch_begin)
    string(FIND "${_dispatch_source}" "static HRESULT stream_pipeline_readbacks" _prefetch_end)
    if(_prefetch_begin LESS 0 OR _prefetch_end LESS _prefetch_begin)
        message(FATAL_ERROR "Aligned prefetch test cannot locate the production implementation")
    endif()
    math(EXPR _prefetch_length "${_prefetch_end} - ${_prefetch_begin}")
    string(SUBSTRING "${_dispatch_source}" ${_prefetch_begin} ${_prefetch_length} ALIGNED_PREFETCH_IMPLEMENTATION)
    configure_file(tests/aligned_prefetch_test.cpp.in aligned_prefetch_test.cpp @ONLY)
    add_executable(mgpu_aligned_prefetch_test "${CMAKE_CURRENT_BINARY_DIR}/aligned_prefetch_test.cpp")
    target_include_directories(mgpu_aligned_prefetch_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_aligned_prefetch_test COMMAND mgpu_aligned_prefetch_test)
    string(FIND "${_dispatch_source}" "    while (s.pipeline.pending())" _retire_begin)
    if(_retire_begin LESS 0)
        message(FATAL_ERROR "Neural poll test cannot locate the production retirement loop")
    endif()
    string(SUBSTRING "${_dispatch_source}" ${_retire_begin} -1 _retire_source)
    string(FIND "${_retire_source}" "        s.aligned_history_valid = true;" _retire_length)
    if(_retire_length LESS 0)
        message(FATAL_ERROR "Neural poll test cannot locate the production retirement guard end")
    endif()
    string(SUBSTRING "${_retire_source}" 0 ${_retire_length} NEURAL_RETIRE_GUARD)
    string(REPLACE "s." "stream." NEURAL_RETIRE_GUARD "${NEURAL_RETIRE_GUARD}")
    string(REPLACE "stream_secondary_fg_complete(s," "stream_secondary_fg_complete(stream,"
        NEURAL_RETIRE_GUARD "${NEURAL_RETIRE_GUARD}")
    configure_file(tests/neural_poll_test.cpp.in neural_poll_test.cpp @ONLY)
    add_executable(mgpu_neural_poll_test "${CMAKE_CURRENT_BINARY_DIR}/neural_poll_test.cpp")
    target_include_directories(mgpu_neural_poll_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_neural_poll_test COMMAND mgpu_neural_poll_test)
    string(REPLACE "            break;\n        }\n        if (status"
                   "            return;\n        }\n        if (status"
                   _old_return_guard "${NEURAL_RETIRE_GUARD}")
    if(_old_return_guard STREQUAL NEURAL_RETIRE_GUARD)
        message(FATAL_ERROR "Neural poll regression control could not restore the old early return")
    endif()
    set(NEURAL_RETIRE_GUARD "${_old_return_guard}")
    configure_file(tests/neural_poll_test.cpp.in neural_poll_old_return_test.cpp @ONLY)
    add_executable(mgpu_neural_poll_old_return_test "${CMAKE_CURRENT_BINARY_DIR}/neural_poll_old_return_test.cpp")
    target_include_directories(mgpu_neural_poll_old_return_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME mgpu_neural_poll_old_return_is_rejected COMMAND mgpu_neural_poll_old_return_test)
    set_tests_properties(mgpu_neural_poll_old_return_is_rejected PROPERTIES WILL_FAIL TRUE)
    add_executable(mgpu_pipeline_timing_test tests/pipeline_timing_test.cpp)
    add_test(NAME mgpu_pipeline_timing_test COMMAND mgpu_pipeline_timing_test)
    add_executable(mgpu_queue_clock_cache_test tests/queue_clock_cache_test.cpp)
    add_test(NAME mgpu_queue_clock_cache_test COMMAND mgpu_queue_clock_cache_test)
    add_executable(mgpu_producer_copy_warp_test tests/producer_copy_warp_test.cpp)
    target_compile_definitions(mgpu_producer_copy_warp_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_producer_copy_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_producer_copy_warp_test COMMAND mgpu_producer_copy_warp_test)
    set_tests_properties(mgpu_producer_copy_warp_test PROPERTIES TIMEOUT 20)
    string(FIND "${_dispatch_source}" "static bool stream_cq_submit(" _copy_begin)
    string(FIND "${_dispatch_source}" "// Bridge thread, once per present." _copy_end)
    if(_copy_begin LESS 0 OR _copy_end LESS _copy_begin)
        message(FATAL_ERROR "Cannot extract production consumer COPY submission")
    endif()
    math(EXPR _copy_length "${_copy_end}-${_copy_begin}")
    string(SUBSTRING "${_dispatch_source}" ${_copy_begin} ${_copy_length} CONSUMER_COPY_IMPLEMENTATION)
    configure_file(tests/color_copy_overlap_warp_test.cpp.in color_copy_overlap_warp_test.cpp @ONLY)
    add_executable(mgpu_color_copy_overlap_warp_test "${CMAKE_CURRENT_BINARY_DIR}/color_copy_overlap_warp_test.cpp")
    target_include_directories(mgpu_color_copy_overlap_warp_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(mgpu_color_copy_overlap_warp_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_color_copy_overlap_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_color_copy_overlap_warp_test COMMAND mgpu_color_copy_overlap_warp_test)
    set_tests_properties(mgpu_color_copy_overlap_warp_test PROPERTIES TIMEOUT 30)
    add_executable(mgpu_guide_copy_overlap_warp_test "${CMAKE_CURRENT_BINARY_DIR}/color_copy_overlap_warp_test.cpp")
    target_include_directories(mgpu_guide_copy_overlap_warp_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(mgpu_guide_copy_overlap_warp_test PRIVATE NOMINMAX MGPU_TEST_GUIDE_SPLIT=1)
    target_link_libraries(mgpu_guide_copy_overlap_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_guide_copy_overlap_warp_test COMMAND mgpu_guide_copy_overlap_warp_test)
    set_tests_properties(mgpu_guide_copy_overlap_warp_test PROPERTIES TIMEOUT 30)
    add_executable(mgpu_neural_history_test tests/neural_history_test.cpp)
    add_test(NAME mgpu_neural_history_test COMMAND mgpu_neural_history_test)
    add_executable(mgpu_neural_snapshots_warp_test tests/neural_snapshots_warp_test.cpp)
    target_compile_definitions(mgpu_neural_snapshots_warp_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_neural_snapshots_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_neural_snapshots_warp_test COMMAND mgpu_neural_snapshots_warp_test)
    set_tests_properties(mgpu_neural_snapshots_warp_test PROPERTIES TIMEOUT 20)
    add_executable(mgpu_secondary_fg_pipeline_test tests/secondary_fg_pipeline_test.cpp)
    add_executable(mgpu_async_consumer_test tests/async_consumer_test.cpp)
    target_compile_definitions(mgpu_async_consumer_test PRIVATE NOMINMAX)
    add_test(NAME mgpu_async_consumer_test COMMAND mgpu_async_consumer_test)
    set_tests_properties(mgpu_async_consumer_test PROPERTIES TIMEOUT 10)
    add_executable(mgpu_presentation_freshness_test tests/presentation_freshness_test.cpp)
    add_test(NAME mgpu_presentation_freshness_test COMMAND mgpu_presentation_freshness_test)
    add_executable(mgpu_presentation_feedback_warp_test tests/presentation_feedback_warp_test.cpp)
    target_compile_definitions(mgpu_presentation_feedback_warp_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_presentation_feedback_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_presentation_feedback_warp_test COMMAND mgpu_presentation_feedback_warp_test)
    set_tests_properties(mgpu_presentation_feedback_warp_test PROPERTIES TIMEOUT 20)
    target_compile_definitions(mgpu_secondary_fg_pipeline_test PRIVATE NOMINMAX)
    add_test(NAME mgpu_secondary_fg_pipeline_test COMMAND mgpu_secondary_fg_pipeline_test)
    add_executable(mgpu_secondary_fg_retirement_test tests/secondary_fg_retirement_test.cpp)
    target_compile_definitions(mgpu_secondary_fg_retirement_test PRIVATE NOMINMAX)
    add_test(NAME mgpu_secondary_fg_retirement_test COMMAND mgpu_secondary_fg_retirement_test)
    set_tests_properties(mgpu_secondary_fg_retirement_test PROPERTIES TIMEOUT 10)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS experimental/secondary_fg/source_gate.hpp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/experimental/secondary_fg/source_gate.hpp" _fg_gate_source)
    foreach(_fg_control presentation partial_progress)
        set(FG_GATE_IMPLEMENTATION "${_fg_gate_source}")
        if(_fg_control STREQUAL "presentation")
            string(REPLACE "        return completed();"
                "        const auto shown = presented();\n        return shown != 0 ? shown : completed();"
                FG_GATE_IMPLEMENTATION "${FG_GATE_IMPLEMENTATION}")
        else()
            string(REPLACE "        const auto start = std::chrono::steady_clock::now();"
                "        const auto start = std::chrono::steady_clock::now();\n        const auto before = progress();"
                FG_GATE_IMPLEMENTATION "${FG_GATE_IMPLEMENTATION}")
            string(REPLACE "                ++consecutive_stalls_;"
                "                if (progress() == before) ++consecutive_stalls_;\n                else consecutive_stalls_ = 0;"
                FG_GATE_IMPLEMENTATION "${FG_GATE_IMPLEMENTATION}")
        endif()
        if(FG_GATE_IMPLEMENTATION STREQUAL _fg_gate_source)
            message(FATAL_ERROR "FG pacing negative control failed to restore the old defect")
        endif()
        set(_fg_control_header "${CMAKE_CURRENT_BINARY_DIR}/fg_${_fg_control}_control.hpp")
        configure_file(tests/source_gate_control.hpp.in "${_fg_control_header}" @ONLY)
        add_executable(mgpu_fg_${_fg_control}_control tests/secondary_fg_retirement_test.cpp)
        target_compile_definitions(mgpu_fg_${_fg_control}_control PRIVATE NOMINMAX
            "MGPU_SOURCE_GATE_HEADER=\"${_fg_control_header}\"")
        target_include_directories(mgpu_fg_${_fg_control}_control PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/experimental/secondary_fg")
        add_test(NAME mgpu_fg_${_fg_control}_old_defect_is_rejected COMMAND mgpu_fg_${_fg_control}_control)
        set_tests_properties(mgpu_fg_${_fg_control}_old_defect_is_rejected PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
    endforeach()
    add_executable(mgpu_secondary_fg_contract_test experimental/secondary_fg/frame_contract_test.cpp)
    add_test(NAME mgpu_secondary_fg_contract_test COMMAND mgpu_secondary_fg_contract_test)
    add_executable(mgpu_secondary_fg_api_test experimental/secondary_fg/stage_api_test.cpp
        experimental/secondary_fg/parameter_bridge_test.cpp experimental/secondary_fg/parameter_bridge.cpp
        experimental/secondary_fg/dlss_fg_stage.cpp)
    target_compile_definitions(mgpu_secondary_fg_api_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_secondary_fg_api_test PRIVATE dxguid)
    add_test(NAME mgpu_secondary_fg_api_test COMMAND mgpu_secondary_fg_api_test)
    add_executable(mgpu_secondary_fg_stage_warp_test tests/secondary_fg_stage_warp_test.cpp
        experimental/secondary_fg/parameter_bridge.cpp experimental/secondary_fg/dlss_fg_stage.cpp)
    target_compile_definitions(mgpu_secondary_fg_stage_warp_test PRIVATE NOMINMAX)
    target_link_libraries(mgpu_secondary_fg_stage_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_secondary_fg_stage_warp_test COMMAND mgpu_secondary_fg_stage_warp_test)
    set_tests_properties(mgpu_secondary_fg_stage_warp_test PROPERTIES TIMEOUT 20)
    add_executable(mgpu_streamline_constants_test tests/streamline_constants_test.cpp)
    add_test(NAME mgpu_streamline_constants_test COMMAND mgpu_streamline_constants_test)
    add_executable(mgpu_neural_pipeline_test tests/neural_pipeline_test.cpp)
    add_test(NAME mgpu_neural_pipeline_test COMMAND mgpu_neural_pipeline_test)
    add_executable(mgpu_depth_plane_policy_test tests/depth_plane_policy_test.cpp src/depth_normalizer.cpp)
    target_link_libraries(mgpu_depth_plane_policy_test PRIVATE d3d12 dxguid)
    add_test(NAME mgpu_depth_plane_policy_test COMMAND mgpu_depth_plane_policy_test)
    add_executable(mgpu_depth_normalizer_warp_test tests/depth_normalizer_warp_test.cpp src/depth_normalizer.cpp)
    target_link_libraries(mgpu_depth_normalizer_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_depth_normalizer_warp_test COMMAND mgpu_depth_normalizer_warp_test)
    set_tests_properties(mgpu_depth_normalizer_warp_test PROPERTIES TIMEOUT 20)
    add_executable(mgpu_d3d12_device_identity_test tests/d3d12_device_identity_test.cpp)
    add_test(NAME mgpu_d3d12_device_identity_test COMMAND mgpu_d3d12_device_identity_test)
    add_executable(mgpu_aligned_texture_policy_test tests/aligned_texture_policy_test.cpp)
    add_test(NAME mgpu_aligned_texture_policy_test COMMAND mgpu_aligned_texture_policy_test)
    add_executable(mgpu_aligned_frame_policy_test tests/aligned_frame_policy_test.cpp)
    add_test(NAME mgpu_aligned_frame_policy_test COMMAND mgpu_aligned_frame_policy_test)
    add_executable(mgpu_calibration_policy_test tests/calibration_policy_test.cpp)
    add_test(NAME mgpu_calibration_policy_test COMMAND mgpu_calibration_policy_test)
    add_executable(mgpu_native_reflex_request_test tests/native_reflex_request_test.cpp)
    add_test(NAME mgpu_native_reflex_request_test COMMAND mgpu_native_reflex_request_test)
    add_executable(mgpu_source_fg_options_test tests/source_fg_options_test.cpp)
    add_test(NAME mgpu_source_fg_options_test COMMAND mgpu_source_fg_options_test)
    add_executable(mgpu_source_fg_guard_test tests/source_fg_guard_test.cpp)
    add_test(NAME mgpu_source_fg_guard_test COMMAND mgpu_source_fg_guard_test)
    add_executable(mgpu_aligned_queue_abi_test tests/aligned_queue_abi_test.cpp)
    add_test(NAME mgpu_aligned_queue_abi_test COMMAND mgpu_aligned_queue_abi_test)
    add_executable(mgpu_aligned_submission_test tests/aligned_submission_test.cpp src/depth_normalizer.cpp src/frame_measurement.cpp tests/frame_measurement_log_stub.cpp tests/game_work_poll_stub.cpp)
    target_compile_definitions(mgpu_aligned_submission_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_aligned_submission_test PRIVATE dxguid d3d12)
    add_test(NAME mgpu_aligned_submission_test COMMAND mgpu_aligned_submission_test)
    add_executable(mgpu_fenced_pipeline_warp_test tests/fenced_pipeline_warp_test.cpp src/depth_normalizer.cpp src/frame_measurement.cpp tests/frame_measurement_log_stub.cpp tests/game_work_poll_stub.cpp)
    target_compile_definitions(mgpu_fenced_pipeline_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_fenced_pipeline_warp_test PRIVATE dxguid d3d12 dxgi)
    add_test(NAME mgpu_fenced_pipeline_warp_test COMMAND mgpu_fenced_pipeline_warp_test)
    set_tests_properties(mgpu_fenced_pipeline_warp_test PROPERTIES TIMEOUT 30)
    add_executable(mgpu_source_ready_dispatch_warp_test tests/fenced_pipeline_warp_test.cpp src/depth_normalizer.cpp src/frame_measurement.cpp tests/frame_measurement_log_stub.cpp tests/game_work_poll_stub.cpp)
    target_compile_definitions(mgpu_source_ready_dispatch_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR MGPU_TEST_READY_DISPATCH)
    target_link_libraries(mgpu_source_ready_dispatch_warp_test PRIVATE dxguid d3d12 dxgi)
    add_test(NAME mgpu_source_ready_dispatch_warp_test COMMAND mgpu_source_ready_dispatch_warp_test)
    set_tests_properties(mgpu_source_ready_dispatch_warp_test PROPERTIES TIMEOUT 30)
    add_executable(mgpu_motion_frame_packet_test tests/motion_frame_packet_test.cpp)
    target_compile_definitions(mgpu_motion_frame_packet_test PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    add_test(NAME mgpu_motion_frame_packet_test COMMAND mgpu_motion_frame_packet_test)
    add_executable(mgpu_present_read_retirement_test tests/present_read_retirement_test.cpp)
    add_test(NAME mgpu_present_read_retirement_test COMMAND mgpu_present_read_retirement_test)
    add_executable(mgpu_tagged_motion_policy_test tests/tagged_motion_policy_test.cpp)
    add_test(NAME mgpu_tagged_motion_policy_test COMMAND mgpu_tagged_motion_policy_test)
    add_executable(mgpu_streamline_tag_reader_test tests/streamline_tag_reader_test.cpp tests/game_work_poll_stub.cpp tests/official_source_stub.cpp src/native_reflex.cpp src/source_fg_guard.cpp)
    add_test(NAME mgpu_streamline_tag_reader_test COMMAND mgpu_streamline_tag_reader_test)
    add_executable(mgpu_streamline_route_audit_test tests/streamline_route_audit_test.cpp tests/game_work_poll_stub.cpp tests/official_source_stub.cpp src/native_reflex.cpp tests/source_fg_guard_stub.cpp)
    add_test(NAME mgpu_streamline_route_audit_test COMMAND mgpu_streamline_route_audit_test)
    enable_testing()
    add_executable(mgpu_motion_source_audit_test tests/motion_source_audit_test.cpp)
    target_link_libraries(mgpu_motion_source_audit_test PRIVATE dxguid)
    add_test(NAME mgpu_motion_source_audit_test COMMAND mgpu_motion_source_audit_test)
    add_executable(mgpu_fg_present_evidence_test tests/fg_present_evidence_test.cpp)
    add_test(NAME mgpu_fg_present_evidence_test COMMAND mgpu_fg_present_evidence_test)
    add_executable(mgpu_adapter_selection_test
        tests/adapter_selection_test.cpp
        src/adapter_selection.cpp
    )
    target_include_directories(mgpu_adapter_selection_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME mgpu_adapter_selection_test
             COMMAND mgpu_adapter_selection_test)
    # T1.19 (Dom): the instruments' state machines, off-device
    add_executable(mgpu_temporal_diag_test tests/temporal_diag_test.cpp src/temporal_diag.cpp)
    add_test(NAME mgpu_temporal_diag_test COMMAND mgpu_temporal_diag_test)
    add_executable(mgpu_perf_diag_test tests/perf_diag_test.cpp src/perf_diag.cpp)
    add_test(NAME mgpu_perf_diag_test COMMAND mgpu_perf_diag_test)
    add_executable(mgpu_feature_diag_test tests/feature_diag_test.cpp src/feature_diag.cpp)
    add_test(NAME mgpu_feature_diag_test COMMAND mgpu_feature_diag_test)
    add_executable(mgpu_producer_diag_test tests/producer_diag_test.cpp src/producer_diag.cpp)
    add_test(NAME mgpu_producer_diag_test COMMAND mgpu_producer_diag_test)
    add_executable(mgpu_queue_dependency_trace_warp_test tests/queue_dependency_trace_warp_test.cpp)
    target_sources(mgpu_streamline_tag_reader_test PRIVATE tests/measurement_inactive_stub.cpp)
    target_sources(mgpu_streamline_route_audit_test PRIVATE tests/measurement_inactive_stub.cpp)
    target_compile_definitions(mgpu_queue_dependency_trace_warp_test PRIVATE NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(mgpu_queue_dependency_trace_warp_test PRIVATE d3d12 dxgi dxguid)
    add_test(NAME mgpu_queue_dependency_trace_warp_test COMMAND mgpu_queue_dependency_trace_warp_test)
    set_tests_properties(mgpu_queue_dependency_trace_warp_test PROPERTIES TIMEOUT 30)
    add_test(NAME mgpu_queue_dependency_trace_overflow_test COMMAND mgpu_queue_dependency_trace_warp_test --overflow)
    set_tests_properties(mgpu_queue_dependency_trace_overflow_test PROPERTIES TIMEOUT 30)
endif()

get_property(mgpu_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(mgpu_target IN LISTS mgpu_targets)
    get_target_property(mgpu_target_type ${mgpu_target} TYPE)
    if(mgpu_target STREQUAL "mgpu_bridge" OR mgpu_target_type STREQUAL "EXECUTABLE")
        target_link_libraries(${mgpu_target} PRIVATE mgpu_queue_trace)
    endif()
endforeach()
