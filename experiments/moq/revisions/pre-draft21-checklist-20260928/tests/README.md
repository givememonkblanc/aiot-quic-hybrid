# Host tests

From any directory:

```sh
bash /path/to/aiot_quic_baseline/experiments/moq/run_checks.sh
```

The runner compiles portable C tests with ASan/UBSan and runs the Python tests
for wire parsing, accounting, analysis, OTA restoration checks and screw metrics.
`codec_test_stubs/` and `worker_test_stubs/` emulate hardware/FreeRTOS APIs only
for these tests. They are not included in firmware builds.

Add `--literature` to run the upstream-JavaScript/C estimator differential check.
That check needs Node and the pinned `literature/vendor/public-moq-demo` checkout.
No test in this entry point flashes a device or configures networking.
