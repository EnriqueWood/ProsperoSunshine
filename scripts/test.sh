#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Portable unit tests on the build host, under AddressSanitizer and UndefinedBehaviorSanitizer.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$root/out"
run_test() {
    local name=$1
    shift
    clang++-18 -std=c++23 -stdlib=libstdc++ -Wall -Wextra -Werror -O1 -g \
        -fsanitize=address,undefined -I "$root/src" "$root/tests/$name.cpp" "$@" -o "$root/out/$name"
    "$root/out/$name"
}
# capture.c itself, with the SDK and the console's libraries replaced by tests/fake_avcap.h.
run_capture_test() {
    local name=$1
    shift
    clang-18 -std=gnu17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I "$root/src" -I "$root/tests" \
        -I "$root/tests/stubs" "$root/tests/$name.c" "$@" -o "$root/out/$name"
    "$root/out/$name"
}
run_capture_test capture_records_test "-DGOLDEN=\"$root/tests/capture_records.golden\""
run_capture_test capture_open_test
run_capture_test capture_slots_test
run_capture_test probe_record_test
run_test experimental_test
run_test video_test
run_test capture_session_test "$root/src/capture/capture_session.cpp"
run_test gamepad_map_test
run_test pad_roles_test
run_test pad_touch_test
run_test keyboard_map_test
run_test keyboard_pad_test
run_test title_profile_test
run_test listener_watch_test
run_test device_modes_test
run_test device_profiles_test
run_test stream_list_test
run_test capture_mode_test
run_test channel_choice_test
run_test channel_map_test
run_test hdr10_metadata_test
run_test titles_test "$root/src/apps/titles.cpp"
run_test mdns_test "$root/src/net/mdns.cpp"
run_test ra_choose_test
run_test probe_scrub_test
run_test host_control_test
run_test http_guard_test
run_test file_bridge_test "$root/src/control/file_bridge.cpp"
run_test quit_policy_test
run_test encoder_qp_test
run_test web_auth_test
run_test web_lines_test
run_test web_plain_test
run_test log_throttle_test
