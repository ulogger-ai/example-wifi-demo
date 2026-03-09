#ifndef ULOGGER_CONFIG_H
#define ULOGGER_CONFIG_H

/**
 * @file ulogger_config.h
 * @brief User configuration settings for uLogger
 *
 * This file contains all user-configurable settings for the uLogger library.
 * Modify these definitions to match your application requirements and hardware.
 */

// ============================================================================
// Debug Module Definitions (USER-DEFINED)
// ============================================================================

/**
 * Define your debug modules using the X-macro pattern.
 * Each entry: X(NAME, bit_position)
 * - NAME: Module identifier (will generate NAME_MODULE enum value)
 * - bit_position: Bit position (0-31) for the module flag
 *
 * Example customization:
 * #define ULOGGER_DEBUG_MODULE_LIST(X) \
 *   X(MAIN,    0) \
 *   X(NETWORK, 1) \
 *   X(STORAGE, 2)
 */
#define ULOGGER_DEBUG_MODULE_LIST(X) \
  X(MAIN,   0) \
  X(SYSTEM, 1) \
  X(COMM,   2) \
  X(SENSOR, 3) \
  X(POWER,  4)

// ULOGGER TODO:
// Unique numeric ID that identifies this firmware build / application.
// Change this whenever a new, incompatible log format is released so that
// the cloud back-end can apply the correct decoder.
#define ULOGGER_DEVICE_SERIAL         1001      // @ULOGGER TODO - define device serial number
#define ULOGGER_CUSTOMER_ID           12345     // @ULOGGER TODO - define your customer ID
#define ULOGGER_APPLICATION_ID        12345     // @ULOGGER TODO - define your application ID
#define ULOGGER_DEVICE_TYPE           "WiFi_Demo"

// ============================================================================
// Non-Volatile Memory Configuration (USER-DEFINED)
// ============================================================================

/**
 * Non-volatile memory addresses for persistent logging
 * Customize these for your target hardware
 * 
 * If using internal flash, ensure these addresses do not overlap with your
 * application code or other critical data.
 */
#define ULOGGER_LOG_NV_START_ADDRESS       0
#define ULOGGER_LOG_NV_END_ADDRESS         1499
#define ULOGGER_EXCEPTION_NV_START_ADDRESS 1500
#define ULOGGER_EXCEPTION_NV_END_ADDRESS   2599

#endif // ULOGGER_CONFIG_H
