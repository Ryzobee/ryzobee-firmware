# IMU background owner

`ryz_sensors_start()` creates one boot-lifetime FreeRTOS task (priority 3,
4096-byte stack, no fixed core). Success means task creation, not a detected or
healthy sensor. Failed mutex/task allocation is retryable; a concurrent start
is rejected and a completed start is idempotent. The caller performs no I2C.
There is no stop/reconfigure Interface and this module is not a boot health gate.

Only this worker calls the public `ryz_imu_init`, `ryz_imu_read_sample` and
`ryz_imu_get_info` Interface. It does not own/rebuild I2C0, change the display or
touch owner, or expose registers to Lua. The BSP keeps all physical transactions
bounded and serializes them with touch on the existing board bus.

The sensor uses the BSP's fixed 25 Hz mode. After a successful polling cycle the
worker sleeps 40 ms (not an exact 25 Hz scheduler guarantee). Initialization,
transport or diagnostic-copy failures invalidate cached coordinates, then wait
5000 ms before explicitly reinitializing. Reinitialization performs real
identification/reset/configuration; it is not an idempotent ready check. There
are no periodic error logs or hardware PASS/FAIL judgments in this module.

`ESP_ERR_NOT_FINISHED` means no new conversion, including the BSP's first
discarded settling sample. It is not an error and does not change cached XYZ,
sequence or acquisition timestamp. One second without a fresh sample, measured
from successful initialization or the last successful read, triggers a TIMEOUT
watchdog and the same invalidation/reinitialization policy. The watchdog is
checked after each read; scheduler and bounded transport latency may delay its
observation. It does not certify movement, PCB identity or sensor self-test.

## Snapshot contract

`ryz_sensors_get_snapshot()` only copies completed-cycle state under a short
mutex and obtains monotonic time for age. The mutex never spans I2C or sleep.
Readers remain available even during a blocked sensor transaction. Before
startup completes, the function clears output and returns INVALID_STATE; after
startup, physical errors are explicit snapshot data rather than API failures.

- `initialized`: this worker's current initialization generation is admitted.
- `sample_valid`: the latest successful sample remains valid in that generation.
  Check `sample_age_ms` as well; a short NO_DATA interval retains the old time.
- `ever_sampled`: at least one real successful read occurred during this boot.
- `sample`: only valid when `sample_valid`; all fields are zero after a failure.
- `last_sampled_ms`: historical acquisition completion time, derived directly
  from the driver's microsecond timestamp; retained after invalidation.
- `sample_age_ms`: age of that historical sample, meaningful only if
  `ever_sampled`; never a replacement for `sample_valid`.
- `imu`: cached driver diagnostics. The worker additionally revokes `imu.ready`
  on its own errors/watchdog; other fields and driver counters remain diagnostic.
- `last_error`: current completed-cycle error, cleared by recovery; NO_DATA alone
  is OK. `last_error_ms` retains the most recent real error/watchdog time.
- Worker attempt/sample/no-data/error counters saturate at UINT32_MAX. Driver
  counters are separate. A successful read followed by a diagnostic-copy failure
  still counts as acquired, but its XYZ is invalidated before publication.

Timestamps are boot-monotonic, not UTC or sensor-internal conversion timestamps.
Samples use physical sensor axes in mg; this module applies no display-orientation
transform. Invalid, stale, absent and unimplemented capabilities must not become
zero-g measurements, “motion OK”, or an automatic hardware test PASS.

## Host verification

From the workspace root:

```sh
python3 -m unittest firmware/rootmaker/tests/test_sensors.py -v
```

Seven public-Interface cases execute the real worker on a pthread with controlled
OS waiting/clock and external IMU outcomes: allocation retries, concurrent start,
copy-only snapshots during blocked I2C, fresh timestamps, short NO_DATA,
initial/transport/diagnostic failures, and both initial and post-sample no-data
watchdogs. The harness uses ASan/UBSan and asserts every driver call belongs to
the worker with no snapshot lock held. It substitutes the physical driver and
FreeRTOS Adapter, so it proves neither real I2C/electrical behavior nor ESP task
scheduling, live motion, or on-board PASS. Those require separate BSP and target
verification.
