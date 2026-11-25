#!/usr/bin/env bash
# Runs the device checks on the emulator CI starts: the bench, the storage test
# on private storage and /sdcard, and the AAR's instrumented tests.
# Expects the x64-android build and build/android/ staged, as the native job leaves them.
set -euo pipefail
scripts/run-bench.sh x64-android
scripts/run-bench.sh x64-android geocrew_storage /data/local/tmp/geocrew-bench/private /sdcard/GeoCrew-spike
cd android
./gradlew --no-daemon :geocrew-native:connectedDebugAndroidTest
