# Development Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Enthaelt einen Verweis auf das entfernte `zb_device_handler`.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


Guide for developers contributing to or extending the ESP32-C5 gateway.

> ⚠️ Last reviewed 2026-07-31. Toolchain is ESP-IDF **v6.0.2**. Bluetooth is
> disabled (`CONFIG_BT_ENABLED=n`) and `main/bluetooth/` is not compiled.
> The project structure section below predates `main/mmwave/`, the runtime
> converter database (`zb_converter_loader.c` + `data/`), the quirk engine, and
> the host tools under `tools/`. See `CLAUDE.md` for the current picture.
>
> One build gotcha worth knowing before you change any Kconfig value:
> `CMakeLists.txt` feeds **two** files into `SDKCONFIG_DEFAULTS` —
> `sdkconfig.defaults` and then `sdkconfig.local`, where the latter wins and is
> gitignored. Editing `sdkconfig.defaults` alone does nothing once a `sdkconfig`
> exists; delete `sdkconfig` (back it up — `sdkconfig.local` holds credentials)
> and rebuild, then verify `build/config/sdkconfig.h`. `idf.py reconfigure` does
> not reliably regenerate it.

## Table of Contents

- [Development Environment](#development-environment)
- [Project Structure](#project-structure)
- [Coding Standards](#coding-standards)
- [Build System](#build-system)
- [Debugging](#debugging)
- [Testing](#testing)
- [Adding Features](#adding-features)
- [Contributing](#contributing)

## Development Environment

### Recommended Setup

**IDE Options**:
1. **Visual Studio Code** (Recommended)
   - Install ESP-IDF extension
   - Install C/C++ extension
   - Configure IntelliSense

2. **CLion**
   - ESP-IDF plugin
   - Native CMake support

3. **Vim/Neovim**
   - clangd LSP server
   - ESP-IDF integration

### VSCode Setup

```bash
# Install ESP-IDF extension
code --install-extension espressif.esp-idf-extension

# Open project
code /path/to/esp32-c5-zigbee2mqtt
```

Configuration (`.vscode/settings.json`):
```json
{
  "idf.port": "/dev/ttyUSB0",
  "idf.flashBaudRate": "460800",
  "idf.monitorBaudRate": "115200",
  "idf.openOcdDebugLevel": 2,
  "C_Cpp.default.compilerPath": "${env:HOME}/.espressif/tools/riscv32-esp-elf/gcc-<version>/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"
}
```

## Project Structure

```
esp32-c5-zigbee2mqtt/
├── main/                   # Application source
│   ├── core/              # Core modules
│   ├── zigbee/            # Zigbee implementation
│   ├── mqtt/              # MQTT client
│   ├── wifi/              # WiFi manager
│   ├── ota/               # OTA updates
│   ├── utils/             # Utilities
│   ├── main.c             # Entry point
│   ├── CMakeLists.txt     # Build config
│   └── Kconfig.projbuild  # Configuration menu
├── components/            # Custom components
├── docs/                  # Documentation
├── scripts/               # Build/flash scripts
├── tests/                 # Test suite
├── CMakeLists.txt         # Root CMake
├── partitions.csv         # Partition table
└── sdkconfig.defaults     # Default config
```

### Module Organization

Each module follows this structure:
```
module_name.h         # Public API (header)
module_name.c         # Implementation
```

Header template:
```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public API */
esp_err_t module_name_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_NAME_H */
```

## Coding Standards

> **Comprehensive Guide**: For detailed coding standards with extensive examples, see [CODE_STYLE.md](CODE_STYLE.md).

### Quick Reference

| Element | Convention | Example |
|---------|------------|---------|
| Functions | `snake_case` | `mqtt_bridge_init()` |
| Macros/Constants | `UPPER_CASE` | `ZB_MAX_DEVICES` |
| Types (typedef) | `snake_case_t` | `zb_device_t` |
| File-scope vars | `s_` prefix | `static bool s_initialized` |

### Code Formatting

Format code with `clang-format`:
```bash
clang-format -i main/core/*.c main/core/*.h
```

Style: Google-based, 4-space indent, 100-char columns (see `.clang-format` in root).

### Key Rules

1. **Error Handling**: Always check `esp_err_t` returns; use `esp_err_to_name()` for logging
2. **Logging**: Define `static const char *TAG = "MODULE_NAME";` and use `ESP_LOGx()` macros
3. **Documentation**: Use Doxygen-style comments (`@brief`, `@param`, `@return`)

## Build System

### CMake Structure

Root `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-c5-zigbee2mqtt)
```

Main component `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS
        "main.c"
        "core/mqtt_bridge.c"
        "core/device_state_publisher.c"
        # ... more sources
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        mqtt
        esp_zigbee_lib
)
```

### Build Commands

```bash
# Configure
idf.py set-target esp32c5
idf.py menuconfig

# Build
idf.py build

# Clean build
idf.py fullclean
idf.py build

# Build specific component
idf.py build mqtt

# Size analysis
idf.py size
idf.py size-components
```

### Partitions

Modify `partitions.csv` for custom layouts:
```csv
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
phy_init, data, phy,     0xf000,  0x1000
factory,  app,  factory, 0x10000, 0x5E0000
```

## Debugging

### Serial Debugging

```bash
# Monitor with filtering
idf.py monitor | grep MODULE_NAME

# Save log to file
idf.py monitor > debug.log

# Decode crash
idf.py monitor
# On crash, backtrace is automatically decoded
```

### GDB Debugging

With ESP-Prog JTAG debugger:

```bash
# Start OpenOCD
openocd -f board/esp32c5-ftdi.cfg

# In another terminal
riscv32-esp-elf-gdb build/esp32-c5-zigbee2mqtt.elf
(gdb) target remote :3333
(gdb) monitor reset halt
(gdb) b app_main
(gdb) c
```

### Memory Debugging

Enable heap tracing:
```c
#include "esp_heap_trace.h"

// In main
heap_trace_init_standalone(trace_records, NUM_RECORDS);
heap_trace_start(HEAP_TRACE_LEAKS);

// Run code

heap_trace_stop();
heap_trace_dump();
```

### Useful Debugging Macros

```c
// Print function entry
#define TRACE_ENTRY() ESP_LOGD(TAG, ">> %s", __func__)

// Print function exit
#define TRACE_EXIT(ret) ESP_LOGD(TAG, "<< %s: %s", __func__, esp_err_to_name(ret))

// Assert with message
#define ASSERT_MSG(cond, msg) \
    do { if (!(cond)) { ESP_LOGE(TAG, "Assert failed: %s", msg); assert(cond); } } while(0)
```

## Testing

### Unit Tests

Location: `tests/unit/`

Using Unity framework:
```c
#include "unity.h"

TEST_CASE("Device table add", "[device_handler]")
{
    device_handler_init();
    esp_err_t ret = device_handler_add(0x1234, "test_device");
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

TEST_CASE("Device table full", "[device_handler]")
{
    device_handler_init();
    // Add MAX_DEVICES devices
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_handler_add(i, "device");
    }
    // Next add should fail
    esp_err_t ret = device_handler_add(0xFFFF, "overflow");
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ret);
}
```

Run tests:
```bash
cd tests/unit
idf.py build flash monitor
```

### Integration Tests

Location: `tests/integration/`

Python-based tests:
```python
import pytest
import paho.mqtt.client as mqtt

def test_device_state_publish(mqtt_client, gateway):
    # Subscribe to device topic
    mqtt_client.subscribe("zigbee2mqtt/test_device")

    # Trigger state change on device
    gateway.device_trigger(0x1234)

    # Wait for MQTT message
    msg = mqtt_client.wait_for_message(timeout=5)
    assert msg is not None
    assert "state" in msg.payload
```

## Adding Features

### Adding a New Module

1. **Create header** (`main/module/new_module.h`):
```c
#ifndef NEW_MODULE_H
#define NEW_MODULE_H

#include "esp_err.h"

esp_err_t new_module_init(void);
void new_module_process(void);

#endif
```

2. **Implement** (`main/module/new_module.c`):
```c
#include "new_module.h"
#include "esp_log.h"

static const char *TAG = "NEW_MODULE";

esp_err_t new_module_init(void) {
    ESP_LOGI(TAG, "Initializing module");
    return ESP_OK;
}
```

3. **Update CMakeLists.txt**:
```cmake
idf_component_register(
    SRCS
        # ... existing
        "module/new_module.c"
    # ...
)
```

4. **Integrate in main.c**:
```c
#include "module/new_module.h"

void app_main(void) {
    // ... existing init
    ESP_ERROR_CHECK(new_module_init());
}
```

### Adding MQTT Topics

1. **Define topic** in `mqtt/mqtt_topics.h`:
```c
#define TOPIC_NEW_FEATURE "zigbee2mqtt/bridge/new_feature"
```

2. **Subscribe** in `mqtt_bridge.c`:
```c
mqtt_client_subscribe(TOPIC_NEW_FEATURE, 1);
```

3. **Handle messages** in `command_handler.c`:
```c
if (strstr(topic, "new_feature")) {
    handle_new_feature_command(payload, len);
}
```

### Adding Zigbee Clusters

1. **Define cluster** in `zb_device_handler.h`:
```c
#define ZB_CLUSTER_NEW 0xABCD
```

2. **Handle attributes** in `zb_callbacks.c`:
```c
static void handle_new_cluster_attr(uint16_t attr_id, void *value) {
    switch (attr_id) {
        case ATTR_NEW_VALUE:
            // Process attribute
            break;
    }
}
```

3. **Register handler**:
```c
zb_register_cluster_handler(ZB_CLUSTER_NEW, handle_new_cluster_attr);
```

## Contributing

### Workflow

1. **Fork repository**
2. **Create branch**: `git checkout -b feature/my-feature`
3. **Make changes**
4. **Test thoroughly**
5. **Commit**: `git commit -m "Add: brief description"`
6. **Push**: `git push origin feature/my-feature`
7. **Create Pull Request**

### Commit Message Format

```
Type: Brief description (50 chars or less)

Detailed explanation if needed (wrap at 72 characters).

Closes #123
```

**Types**:
- `Add`: New feature
- `Fix`: Bug fix
- `Update`: Improvement to existing feature
- `Refactor`: Code restructuring
- `Docs`: Documentation changes
- `Test`: Test additions/changes
- `Build`: Build system changes

### Pull Request Checklist

- [ ] Code follows style guidelines
- [ ] Builds without warnings
- [ ] All tests pass
- [ ] New features have tests
- [ ] Documentation updated
- [ ] CHANGELOG.md updated
- [ ] No memory leaks
- [ ] Tested on real hardware

### Code Review

PRs will be reviewed for:
- Correctness
- Memory safety
- Thread safety
- Code style compliance
- Test coverage
- Documentation quality

## Best Practices

1. **Memory Management**
   - Free allocated memory
   - Check for leaks
   - Use memory pools where appropriate

2. **Thread Safety**
   - Use mutexes for shared data
   - Minimize critical sections
   - Document thread safety

3. **Error Handling**
   - Always check return values
   - Log errors appropriately
   - Clean up on errors

4. **Performance**
   - Avoid blocking operations in high-priority tasks
   - Use queues for inter-task communication
   - Profile before optimizing

5. **Security**
   - Validate all inputs
   - Don't log sensitive data
   - Use secure protocols (MQTTS, TLS)

## Resources

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/)
- [ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)
- [Project Architecture](ARCHITECTURE.md)
- [API Reference](API_REFERENCE.md)
- [CONTRIBUTING.md](../CONTRIBUTING.md)
