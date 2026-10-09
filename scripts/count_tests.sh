#!/usr/bin/env bash
#
# count_tests.sh — report the real test counts from a *built* tree.
#
# Prints a single line:  "<unit> <integration> <suites>"
# e.g.                   "244 92 19"
#
# Counts come from the test binaries themselves (gtest --gtest_list_tests), so
# they match what actually runs — not a brittle `grep TEST(` of the sources,
# which overcounts parameterized/disabled macros. Listing does NOT execute the
# tests, so no Infinispan/Docker is needed to count the integration suites.
#
# The integration binaries are discovered via their ctest "integration" label
# (not a filename glob — two of them don't follow the *_integration_tests
# naming), so the list stays correct as suites are added or renamed.
#
# Usage:  scripts/count_tests.sh [build-dir]   (default: build)
#
# Used by CI to render the README test-count badge; also runnable locally to
# refresh the numbers in docs/STATUS.md.

set -euo pipefail

build_dir="${1:-build}"

count_cases() {
    # Indented lines in --gtest_list_tests output are individual test cases;
    # the un-indented lines are suite/fixture headers.
    "$1" --gtest_list_tests 2>/dev/null | grep -cE '^  ' || true
}

unit="$(count_cases "${build_dir}/unit_tests")"

integration=0
suites=0
while IFS= read -r cmd; do
    [ -z "$cmd" ] && continue
    integration=$(( integration + $(count_cases "$cmd") ))
    suites=$(( suites + 1 ))
done < <(ctest --test-dir "${build_dir}" --show-only=json-v1 -L integration 2>/dev/null \
            | jq -r '.tests[].command[0]')

echo "${unit} ${integration} ${suites}"
