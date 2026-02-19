# Testing Framework - Quick Reference

## Run Tests

```bash
cd scripts
./run_tests.sh              # Run all tests
./run_tests.sh -p /dev/ttyUSB1  # Custom port
./run_tests.sh -b           # Build only
./run_tests.sh -c           # Clean build
./run_tests.sh -v           # Verbose
```

## Write Tests

```c
#include "../test_framework.h"

static void test_example(void)
{
    int result = function_under_test(42);
    TEST_ASSERT_EQUAL(84, result);
}

static const test_case_t tests[] = {
    {"example", test_example},
};

test_stats_t run_tests(void)
{
    return test_run_suite(tests, sizeof(tests)/sizeof(tests[0]));
}
```

## Assertions

| Assertion | Usage |
|-----------|-------|
| `TEST_ASSERT(cond)` | Assert condition true |
| `TEST_ASSERT_EQUAL(exp, act)` | Assert integers equal |
| `TEST_ASSERT_NOT_NULL(ptr)` | Assert pointer not NULL |
| `TEST_ASSERT_EQUAL_STRING(e, a)` | Assert strings equal |
| `TEST_ASSERT_EQUAL_UINT32(e, a)` | Assert uint32 equal |
| `TEST_FAIL_MESSAGE(msg)` | Fail with message |

## Mock MQTT

```c
mock_mqtt_init();
mock_mqtt_set_connected(true);
mock_mqtt_publish("topic", "data", 4, 0, false);
mock_mqtt_subscribe("topic/#", 0);
mock_mqtt_inject_message("topic", "data", 4);
mock_mqtt_get_last_published(&msg);
mock_mqtt_deinit();
```

## Mock Zigbee

```c
mock_zigbee_init();
mock_zigbee_start_network();
mock_zigbee_permit_join(60);
uint8_t ieee[8] = {...};
mock_zigbee_simulate_device_join(0x1234, ieee, TYPE);
mock_zigbee_simulate_attribute_change(addr, cluster, attr, &val, len);
mock_zigbee_send_command(addr, ep, cluster, cmd, val);
mock_zigbee_deinit();
```

## Test Structure

```
tests/
├── test_framework.h/c       # Framework
├── run_all_tests.c          # Runner
├── CMakeLists.txt           # Build
├── unit/                    # Unit tests
│   ├── test_memory_manager.c
│   ├── test_config_manager.c
│   ├── test_json_utils.c
│   ├── test_version.c
│   └── test_mqtt_topics.c
├── integration/             # Integration tests
│   ├── test_wifi_mqtt_integration.c
│   ├── test_zigbee_mqtt_bridge.c
│   └── test_ota.c
└── mocks/                   # Mock utilities
    ├── mock_mqtt.h/c
    └── mock_zigbee.h/c
```

## Test Count

- **Unit Tests**: 73 tests
- **Integration Tests**: 34 tests
- **Total**: 107 tests

## Common Commands

```bash
# Build tests
cd tests && idf.py build

# Flash and monitor
cd tests && idf.py -p /dev/ttyUSB0 flash monitor

# Set target
cd tests && idf.py set-target esp32c5

# Clean build
cd tests && rm -rf build
```

## Test Output

```
================================================
  FINAL TEST SUMMARY
================================================
Total Tests:   107
Passed:        107
FAILED:        0
================================================

*** ALL TESTS PASSED ***
```

## Adding Tests

1. Create `test_my_feature.c` in `unit/` or `integration/`
2. Include `../test_framework.h`
3. Write test functions
4. Define test suite array
5. Create runner function
6. Add to `CMakeLists.txt`
7. Call in `run_all_tests.c`

## Debugging

```c
// Add logging
ESP_LOGI("TEST", "Value: %d", value);

// Enable verbose
CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y
```

## Files Created

- 2 framework files
- 5 unit test files (73 tests)
- 3 integration test files (34 tests)
- 4 mock files
- 1 test runner
- 1 CMakeLists.txt
- 1 run script
- 3 documentation files

**Total: 20 files, ~4,000 lines**
