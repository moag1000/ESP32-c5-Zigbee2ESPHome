# ESP32-C5 Zigbee2MQTT Gateway - Testing Framework

Comprehensive testing framework for the ESP32-C5 Zigbee2MQTT Gateway project.

## Overview

This testing framework provides:
- **Lightweight test framework** - No external dependencies, works directly on ESP-IDF
- **Unit tests** - Test individual components in isolation
- **Integration tests** - Test component interactions and end-to-end flows
- **Mock utilities** - Simulate MQTT broker and Zigbee coordinator without hardware
- **Automated test runner** - Run all tests with a single command

## Directory Structure

```
tests/
├── test_framework.h           # Test framework header
├── test_framework.c           # Test framework implementation
├── run_all_tests.c            # Main test runner
├── CMakeLists.txt             # Build configuration
├── README.md                  # This file
│
├── unit/                      # Unit Tests
│   ├── test_memory_manager.c      # Memory manager tests (11 tests)
│   ├── test_config_manager.c      # Config manager tests (14 tests)
│   ├── test_json_utils.c          # JSON utilities tests (15 tests)
│   ├── test_version.c             # Version management tests (17 tests)
│   └── test_mqtt_topics.c         # MQTT topic tests (16 tests)
│
├── integration/               # Integration Tests
│   ├── test_wifi_mqtt_integration.c   # WiFi+MQTT tests (10 tests)
│   ├── test_zigbee_mqtt_bridge.c      # Zigbee-MQTT bridge tests (11 tests)
│   └── test_ota.c                     # OTA update tests (13 tests)
│
└── mocks/                     # Mock Utilities
    ├── mock_mqtt.h/c             # Mock MQTT client
    └── mock_zigbee.h/c           # Mock Zigbee coordinator
```

## Test Coverage

### Unit Tests (73 tests)
- **Memory Manager** (11 tests)
  - Initialization, statistics, threshold checking
  - Memory allocation tracking, fragmentation calculation
  - Periodic logging, low memory callbacks
  - Memory optimization

- **Config Manager** (14 tests)
  - Load/save/reset configuration
  - Validation (valid and invalid configs)
  - Get/set by key, JSON export/import
  - NULL parameter handling

- **JSON Utilities** (15 tests)
  - Bridge state and availability JSON
  - IEEE address formatting
  - Command parsing (ON/OFF, brightness, color)
  - Permit join, device remove/rename parsing
  - Component type strings, NULL parameters

- **Version Management** (17 tests)
  - Version number and string retrieval
  - Build date/time, git commit
  - Version comparison (equal, older, newer)
  - JSON export, format validation
  - Consistency checks

- **MQTT Topics** (16 tests)
  - Device state and command topics
  - Bridge topics (info, state, devices, requests)
  - Topic parsing and validation
  - Special characters, buffer protection
  - Topic matching

### Integration Tests (34 tests)
- **WiFi + MQTT Integration** (10 tests)
  - Mock initialization, connection flow
  - Publish/subscribe operations
  - Message injection, reconnection
  - Multiple messages, QoS/retain flags
  - Statistics tracking

- **Zigbee-MQTT Bridge** (11 tests)
  - Network formation, permit join
  - Device join/leave simulation
  - Attribute changes, command sending
  - End-to-end device join → MQTT publish
  - End-to-end MQTT command → Zigbee device
  - Bridge statistics

- **OTA Updates** (13 tests)
  - Download start/progress/completion
  - Invalid inputs, state validation
  - Full OTA workflow
  - Configuration integration
  - Version checking, rollback capability

**Total: 107 tests**

## Building and Running Tests

### Prerequisites
- ESP-IDF v5.1 or later
- ESP32-C5 development board
- USB connection to host

### Quick Start

```bash
# Navigate to scripts directory
cd scripts

# Run all tests (build, flash, and monitor)
./run_tests.sh

# Or with custom serial port
./run_tests.sh -p /dev/ttyUSB1

# Build only (don't flash)
./run_tests.sh -b

# Clean build before testing
./run_tests.sh -c

# Verbose output
./run_tests.sh -v
```

### Manual Build

```bash
# Navigate to tests directory
cd tests

# Set target
idf.py set-target esp32c5

# Build tests
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## Test Framework API

### Basic Assertions

```c
TEST_ASSERT(condition)              // Assert condition is true
TEST_ASSERT_TRUE(condition)         // Assert true
TEST_ASSERT_FALSE(condition)        // Assert false
TEST_ASSERT_EQUAL(expected, actual) // Assert integers equal
TEST_ASSERT_NOT_EQUAL(expected, actual)
TEST_ASSERT_NULL(ptr)               // Assert pointer is NULL
TEST_ASSERT_NOT_NULL(ptr)           // Assert pointer is not NULL
TEST_ASSERT_EQUAL_STRING(exp, act)  // Assert strings equal
TEST_ASSERT_EQUAL_MEMORY(exp, act, size)
TEST_ASSERT_GREATER_THAN(threshold, actual)
TEST_ASSERT_LESS_THAN(threshold, actual)
TEST_FAIL_MESSAGE(msg)              // Fail with message
```

### Type-Specific Assertions

```c
TEST_ASSERT_EQUAL_UINT8(expected, actual)
TEST_ASSERT_EQUAL_UINT16(expected, actual)
TEST_ASSERT_EQUAL_UINT32(expected, actual)
```

### Writing Tests

```c
// Define test function
static void test_my_feature(void)
{
    // Arrange
    int value = 42;

    // Act
    int result = my_function(value);

    // Assert
    TEST_ASSERT_EQUAL(84, result);
}

// Define test suite
static const test_case_t my_tests[] = {
    {"my_feature", test_my_feature},
    // Add more tests...
};

// Create test runner
test_stats_t run_my_tests(void)
{
    return test_run_suite(my_tests,
                         sizeof(my_tests) / sizeof(my_tests[0]));
}
```

## Mock Utilities

### Mock MQTT Client

Simulates MQTT broker operations without network connection.

```c
#include "mocks/mock_mqtt.h"

// Initialize
mock_mqtt_init();
mock_mqtt_set_connected(true);

// Publish
mock_mqtt_publish("topic", "payload", 7, 0, false);

// Subscribe
mock_mqtt_subscribe("topic/#", 0);

// Inject message (simulate receiving)
mock_mqtt_inject_message("topic/test", "data", 4);

// Get published messages
mock_mqtt_message_t msg;
mock_mqtt_get_last_published(&msg);

// Statistics
mock_mqtt_stats_t stats = mock_mqtt_get_stats();

// Cleanup
mock_mqtt_deinit();
```

### Mock Zigbee Coordinator

Simulates Zigbee coordinator without hardware.

```c
#include "mocks/mock_zigbee.h"

// Initialize
mock_zigbee_init();
mock_zigbee_start_network();

// Permit join
mock_zigbee_permit_join(60);

// Simulate device join
uint8_t ieee[8] = {...};
mock_zigbee_simulate_device_join(0x1234, ieee, ZB_DEVICE_TYPE_ON_OFF_LIGHT);

// Simulate attribute change
uint8_t value = 1;
mock_zigbee_simulate_attribute_change(0x1234, 0x0006, 0x0000, &value, 1);

// Send command
mock_zigbee_send_command(0x1234, 1, 0x0006, 0x01, 1);

// Statistics
mock_zigbee_stats_t stats = mock_zigbee_get_stats();

// Cleanup
mock_zigbee_deinit();
```

## Test Output

### Successful Test Run

```
================================================
  ESP32-C5 Zigbee2MQTT Gateway Test Suite
================================================

========================================
UNIT TESTS
========================================
Running test suite: 11 tests
Running test: memory_manager_init
Test PASSED: memory_manager_init
...

Suite: Memory Manager
  Passed:  11 / 11

========================================
INTEGRATION TESTS
========================================
...

================================================
  FINAL TEST SUMMARY
================================================
Total Tests:   107
Passed:        107
FAILED:        0
================================================

*** ALL TESTS PASSED ***
```

### Failed Test Example

```
Running test: config_manager_validate_invalid_port
E (1234) TEST: Assertion failed: expected 0xFFFFFFFF, got 0x00000000
E (1234) TEST:   at test_config_manager.c:78
Test FAILED: config_manager_validate_invalid_port
```

## Continuous Integration

The test framework is designed for CI/CD integration:

```yaml
# Example GitHub Actions workflow
- name: Run Tests
  run: |
    cd scripts
    ./run_tests.sh
```

## Adding New Tests

1. **Create test file** in `tests/unit/` or `tests/integration/`
2. **Include test framework**: `#include "../test_framework.h"`
3. **Write test functions** using `TEST_ASSERT_*` macros
4. **Define test suite** array
5. **Create test runner** function
6. **Update CMakeLists.txt** to include new file
7. **Update run_all_tests.c** to call your test runner

## Debugging Tests

### Enable Verbose Logging

```c
// In sdkconfig or menuconfig
CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y
```

### Add Debug Output

```c
void test_my_feature(void)
{
    ESP_LOGI("TEST", "Debug info: value=%d", value);
    TEST_ASSERT_EQUAL(expected, actual);
}
```

### Run Single Test Suite

Modify `run_all_tests.c` to comment out other test suites.

## Best Practices

1. **Keep tests focused** - One test, one assertion ideally
2. **Use descriptive names** - `test_memory_manager_init_success`
3. **Clean up resources** - Free memory, close files
4. **Test edge cases** - NULL pointers, zero values, overflow
5. **Mock external dependencies** - Use mock utilities
6. **Document complex tests** - Add comments explaining what's tested

## Performance

- Test framework overhead: ~500 bytes RAM per test
- Total test suite execution time: ~2-3 minutes
- Mock utilities overhead: ~2KB RAM

## Troubleshomarks

### Build Errors

```bash
# Clean and rebuild
rm -rf build
idf.py build
```

### Flash Errors

```bash
# Check serial port
ls /dev/tty*

# Try different baud rate
idf.py -p /dev/ttyUSB0 -b 115200 flash
```

### Test Timeouts

Increase timeout in `run_tests.sh`:
```bash
timeout 180 idf.py -p "$SERIAL_PORT" monitor
```

## License

Copyright (c) 2026
Apache License 2.0

## Support

For issues or questions:
- Check test output for error messages
- Review test framework documentation
- Examine mock utility implementations
- Add debug logging to specific tests
