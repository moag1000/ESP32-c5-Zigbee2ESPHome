# Contributing to ESP32-C5 Zigbee2MQTT Gateway

Thank you for considering contributing to the ESP32-C5 Zigbee2MQTT Gateway! This document provides guidelines for contributing to the project.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [How Can I Contribute?](#how-can-i-contribute)
- [Development Setup](#development-setup)
- [Coding Standards](#coding-standards)
- [Commit Guidelines](#commit-guidelines)
- [Pull Request Process](#pull-request-process)
- [Reporting Bugs](#reporting-bugs)
- [Suggesting Enhancements](#suggesting-enhancements)

## Code of Conduct

### Our Pledge

We are committed to providing a welcoming and inclusive experience for everyone. We expect all contributors to:

- Be respectful and considerate
- Accept constructive criticism gracefully
- Focus on what is best for the community
- Show empathy towards other community members

### Unacceptable Behavior

- Harassment, discrimination, or offensive comments
- Trolling, insulting, or derogatory comments
- Public or private harassment
- Publishing others' private information

## How Can I Contribute?

### Reporting Bugs

Before creating bug reports, please check existing issues to avoid duplicates.

**When reporting bugs, include**:
- Clear, descriptive title
- Steps to reproduce the issue
- Expected vs. actual behavior
- Screenshots/logs if applicable
- Environment details:
  - ESP-IDF version
  - ESP-Zigbee-SDK version
  - Hardware revision
  - Configuration (from `config_get`)

**Example Bug Report**:
```markdown
**Title**: MQTT disconnects after 10 minutes

**Description**:
Gateway disconnects from MQTT broker after approximately 10 minutes of operation.

**Steps to Reproduce**:
1. Flash firmware v1.0.0
2. Configure MQTT broker at 192.168.1.100
3. Wait 10 minutes
4. Observe disconnection in logs

**Expected**: Stay connected indefinitely
**Actual**: Disconnects after ~10 minutes

**Environment**:
- ESP-IDF: v5.0-dev
- ESP-Zigbee-SDK: v1.0
- Hardware: ESP32-C5-DevKitC-1

**Logs**:
```
[MQTT] Connected to broker
[MQTT] Keep-alive timeout
[MQTT] Disconnected
```
```

### Suggesting Enhancements

Enhancement suggestions are tracked as GitHub issues.

**When suggesting enhancements, include**:
- Clear, descriptive title
- Detailed description of the proposed functionality
- Use cases and benefits
- Potential implementation approach
- Any drawbacks or concerns

**Example Enhancement**:
```markdown
**Title**: Add support for Zigbee groups

**Description**:
Add ability to create and control Zigbee groups (multiple devices as one entity).

**Use Case**:
User wants to control all living room lights as a single group.

**Benefits**:
- Simplified control of multiple devices
- More efficient Zigbee communication
- Better Home Assistant integration

**Implementation Ideas**:
- New MQTT topics: `zigbee2mqtt/groups/{group_name}`
- Configuration via bridge requests
- Stored in NVS

**Concerns**:
- Additional memory usage (~500 bytes per group)
- Complexity in state management
```

### Contributing Code

1. **Fork the repository**
2. **Clone your fork**:
   ```bash
   git clone https://github.com/yourusername/esp32-c5-zigbee2mqtt.git
   cd esp32-c5-zigbee2mqtt
   ```
3. **Create a branch**:
   ```bash
   git checkout -b feature/my-new-feature
   ```
4. **Make your changes**
5. **Test thoroughly**
6. **Commit your changes**
7. **Push to your fork**
8. **Create a Pull Request**

### Contributing Documentation

Documentation improvements are always welcome!

- Fix typos and grammar
- Clarify confusing sections
- Add missing information
- Improve examples
- Translate documentation

## Development Setup

### Prerequisites

1. **Install ESP-IDF v6.0**:
   ```bash
   cd ~/esp
   git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
   cd esp-idf-v6
   ./install.sh esp32c5
   . ./export.sh
   ```

2. **Install ESP-Zigbee-SDK**:
   ```bash
   cd ~/esp
   git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git
   export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
   ```

3. **Clone and build**:
   ```bash
   git clone https://github.com/yourusername/esp32-c5-zigbee2mqtt.git
   cd esp32-c5-zigbee2mqtt
   idf.py set-target esp32c5
   idf.py build
   ```

### Development Workflow

1. **Make changes** in your branch
2. **Build and test**:
   ```bash
   idf.py build
   idf.py flash monitor
   ```
3. **Run tests** (if applicable):
   ```bash
   cd tests/unit
   idf.py build flash monitor
   ```
4. **Check for memory leaks** and warnings
5. **Update documentation** if needed
6. **Commit and push**

## Coding Standards

### Style Guide

Follow ESP-IDF coding conventions:

**Naming**:
- Functions: `snake_case()`
- Macros/Constants: `UPPER_CASE`
- Types: `snake_case_t`
- Static variables: `s_variable_name`
- Global variables: `g_variable_name` (avoid if possible)

**Example**:
```c
#define MAX_DEVICES 50

typedef struct {
    uint16_t address;
    char name[32];
} device_info_t;

static device_info_t s_device_table[MAX_DEVICES];

esp_err_t device_handler_init(void);
```

### Code Formatting

Use `clang-format` with project's `.clang-format`:

```bash
# Format specific files
clang-format -i main/core/mqtt_bridge.c

# Format all C files
find main -name "*.c" -o -name "*.h" | xargs clang-format -i
```

### Documentation

Use Doxygen-style comments:

```c
/**
 * @brief Initialize the device handler
 *
 * Allocates device table and registers Zigbee callbacks.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t device_handler_init(void);
```

### Error Handling

Always check and handle errors:

```c
// Good
esp_err_t ret = wifi_connect(ssid, password);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "WiFi connection failed: %s", esp_err_to_name(ret));
    return ret;
}

// Bad
wifi_connect(ssid, password);  // Ignores errors
```

### Memory Management

- Always free allocated memory
- Use `calloc()` for zero-initialized memory
- Check allocation success
- Document ownership of pointers

```c
// Good
char *buffer = malloc(size);
if (buffer == NULL) {
    return ESP_ERR_NO_MEM;
}
// Use buffer...
free(buffer);

// Bad
char *buffer = malloc(size);  // No NULL check
// Use buffer...
// Never freed - memory leak
```

## Commit Guidelines

### Commit Message Format

```
Type: Brief description (50 chars or less)

Detailed explanation of the changes, wrapped at 72 characters.
Explain what and why, not how.

Closes #123
```

### Commit Types

- **Add**: New feature
- **Fix**: Bug fix
- **Update**: Improvement to existing feature
- **Refactor**: Code restructuring without behavior change
- **Docs**: Documentation only changes
- **Test**: Adding or modifying tests
- **Build**: Build system or dependency changes
- **Style**: Formatting, missing semicolons, etc.

### Examples

**Good commits**:
```
Add: Support for Zigbee color temperature control

Implements color temperature control for Zigbee lights supporting
the Color Control cluster. Adds command handling for setting color
temperature via MQTT and reports color temperature changes back.

Closes #45
```

```
Fix: MQTT reconnection loop on network loss

The MQTT client was not properly handling network disconnections,
causing a reconnection loop. Added exponential backoff and proper
state management.

Fixes #78
```

**Bad commits**:
```
Update stuff
Fixed bug
WIP
asdf
```

## Pull Request Process

### Before Submitting

- [ ] Code builds without errors or warnings
- [ ] All tests pass
- [ ] Code follows project style guidelines
- [ ] Documentation is updated
- [ ] CHANGELOG.md is updated (for user-facing changes)
- [ ] Commits are well-formed and descriptive
- [ ] Branch is up-to-date with `main`

### Pull Request Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change
- [ ] Documentation update

## Testing
Describe testing performed:
- [ ] Unit tests added/updated
- [ ] Integration tests pass
- [ ] Manual testing on hardware

## Checklist
- [ ] Code follows project style
- [ ] Self-review completed
- [ ] Documentation updated
- [ ] No new warnings
- [ ] Tests added for new features
- [ ] CHANGELOG.md updated

## Related Issues
Closes #123
Relates to #456
```

### Review Process

1. **Automated checks** run (build, tests, linting)
2. **Code review** by maintainers
3. **Feedback addressed** through additional commits
4. **Approval** from at least one maintainer
5. **Merge** into main branch

### Review Criteria

Reviewers will check for:
- **Correctness**: Does it work as intended?
- **Code Quality**: Is it clean, readable, maintainable?
- **Performance**: Any performance concerns?
- **Memory Safety**: No leaks or buffer overflows?
- **Thread Safety**: Proper synchronization?
- **Documentation**: Adequate comments and docs?
- **Testing**: Sufficient test coverage?
- **Style**: Follows project conventions?

## Testing

### Unit Tests

Add unit tests for new functionality:

```c
// tests/unit/test_device_handler.c
#include "unity.h"
#include "device_handler.h"

TEST_CASE("Device handler initialization", "[device_handler]")
{
    esp_err_t ret = device_handler_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

TEST_CASE("Add device to table", "[device_handler]")
{
    device_handler_init();
    esp_err_t ret = device_handler_add(0x1234, "test_device");
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    // Verify device was added
    const char *name = device_handler_get_name(0x1234);
    TEST_ASSERT_EQUAL_STRING("test_device", name);
}
```

### Integration Tests

Test interactions between components:

```python
# tests/integration/test_mqtt_bridge.py
def test_device_state_publish(gateway, mqtt_client):
    """Test that device state changes are published to MQTT"""
    # Trigger Zigbee device state change
    gateway.zigbee_device_update(0x1234, {"state": "ON"})

    # Verify MQTT message received
    msg = mqtt_client.wait_for_message("zigbee2mqtt/test_device", timeout=5)
    assert msg is not None
    assert json.loads(msg.payload)["state"] == "ON"
```

### Hardware Testing

Test on real hardware before submitting:
- ESP32-C5 DevKit
- Real Zigbee devices
- Actual MQTT broker
- Extended runtime testing (hours/days)

## Communication

### Where to Get Help

- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: Questions and general discussion
- **ESP32 Forums**: ESP-IDF and hardware questions

### Channels

- **GitHub**: https://github.com/yourusername/esp32-c5-zigbee2mqtt
- **Documentation**: Comprehensive docs in `docs/` directory

## Recognition

Contributors will be:
- Listed in the project CHANGELOG
- Credited in commit messages
- Acknowledged in release notes

Thank you for contributing to the ESP32-C5 Zigbee2MQTT Gateway!
