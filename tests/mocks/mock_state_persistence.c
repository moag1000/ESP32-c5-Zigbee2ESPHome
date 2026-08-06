/**
 * @file mock_state_persistence.c
 * @brief Stub for the single state_persistence symbol device_registry.c needs
 *
 * device_registry.c marks the persistence cache dirty whenever device state is
 * written, so that converter-driven reports reach LittleFS instead of living
 * only in RAM until the next reboot. Linking the real state_persistence.c would
 * drag in LittleFS, the mount helper and a FreeRTOS task, all to satisfy one
 * call that sets a boolean.
 *
 * The call count is kept so a test can assert that a state write marks the
 * cache, which is the behaviour that was missing in the first place.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include <stddef.h>

static size_t s_mark_dirty_calls = 0;

void state_persistence_mark_dirty(void)
{
    s_mark_dirty_calls++;
}

size_t mock_state_persistence_mark_dirty_calls(void)
{
    return s_mark_dirty_calls;
}

void mock_state_persistence_reset(void)
{
    s_mark_dirty_calls = 0;
}
