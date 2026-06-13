#pragma once
#include <stdint.h>

// Optional accelerometer-driven orientation tracker. Returns 0..3 (quarter
// turns CW from default mounting). Boards without an IMU — or boards with
// rotation intentionally disabled, like AMOLED-1.8 fixed at 0° — return 0
// from imu_hal_rotation_quadrant() and no-op on init/tick.

void    imu_hal_init(void);
void    imu_hal_tick(void);
uint8_t imu_hal_rotation_quadrant(void);
void    imu_hal_print_debug(void);  // print raw accel + rotation state to Serial

// Freeze/unfreeze orientation updates. While locked, imu_hal_tick() keeps the
// last quadrant so the UI (e.g. the approval card) doesn't reflow under the
// user's fingers. No-op on boards without rotation.
void    imu_hal_lock_rotation(bool locked);
