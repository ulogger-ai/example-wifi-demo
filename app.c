/***************************************************************************/ /**
 * @file
 * @brief AWS MQTT Application
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

#include <string.h>
#include "cmsis_os2.h"
#include "sl_status.h"
#include "sl_net.h"
#include "sl_wifi.h"
#include "sl_wifi_device.h"
#include "sl_net_wifi_types.h"
#include "sl_utility.h"
#include "sl_si91x_driver.h"

#include "sl_board_configuration.h"
#include "errno.h"
#include "socket.h"
#include "sl_net_si91x.h"
#include "sl_wifi_callback_framework.h"
#include "sl_si91x_socket.h"

#ifdef SLI_SI91X_MCU_INTERFACE
#include "sl_si91x_power_manager.h"
#include "sl_si91x_m4_ps.h"
#include "sl_si91x_driver_gpio.h"
#include "sl_si91x_button_init_btn0_config.h"
#endif

//! uLogger API
#include <ulogger.h>
#include "sl_sleeptimer.h"

#include "logging.h"

/******************************************************
*                    Constants
******************************************************/

#define CERTIFICATE_INDEX 0

// NV memory simulation (using a static noinit buffer for this example)
// Initialize to 0xFF to simulate erased Flash memory
#define NV_LOG_SIZE (ULOGGER_EXCEPTION_NV_END_ADDRESS - ULOGGER_LOG_NV_START_ADDRESS + 1)
static uint8_t nv_log_storage[NV_LOG_SIZE] __attribute__((section(".noinit")));


#define ENABLE_NWP_POWER_SAVE 1
#define LOW                   0
#define WRAP_PRIVATE_KEY      0 //! Enable this to wrap the private key

//#if ENABLE_NWP_POWER_SAVE
//volatile uint8_t powersave_given = 0;
//#endif

#if WRAP_PRIVATE_KEY
#include "sl_si91x_wrap.h"

sl_si91x_wrap_config_t wrap_config = { 0 };
uint8_t *wrapped_private_key       = NULL;

// IV used for SL_NET_TLS_PRIVATE_KEY_CBC_WRAP mode.
uint8_t iv[SL_SI91X_IV_SIZE] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
#endif

/******************************************************
*               Function Declarations
******************************************************/
static void main_application(void *argument);
extern void mqtt_task_thread(void *argument);
sl_status_t load_certificates_in_flash(void);
sl_status_t mqtt_publish_message(const char *topic, const char *payload, uint8_t qos);

const void *stack_get_top_address(ulogger_stack_type_t stack_type);
void fault_reboot(void);

#if WRAP_PRIVATE_KEY
sl_status_t set_tls_wrapped_private_key(sl_net_credential_id_t id,
                                        sl_net_credential_type_t type,
                                        const uint8_t *key_ptr,
                                        uint32_t key_length,
                                        const uint8_t *iv);
#endif

/******************************************************
*               Variable Definitions
******************************************************/

extern osSemaphoreId_t select_sem;

// Button event signal
static osEventFlagsId_t button_event_flags = NULL;
static osThreadId_t app_thread_id = NULL;

const osThreadAttr_t thread_attributes = {
  .name       = "app",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 4000, //3072,
  .priority   = osPriorityNormal,
  .tz_module  = 0,
  .reserved   = 0,
};

//! MQTT thread attributes - with larger stack for MQTT operations
const osThreadAttr_t mqtt_thread_attributes = {
  .name       = "mqtt",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 10000,  //! Larger stack for MQTT client operations
  .priority   = osPriorityAboveNormal,  //! MQTT thread runs with higher priority
  .tz_module  = 0,
  .reserved   = 0,
};

//! No of ltcp socktes
#define RSI_NUMBER_OF_LTCP_SOCKETS 0

//! Default number of sockets supported,max 10 sockets are supported
#define RSI_NUMBER_OF_SOCKETS (6 + RSI_NUMBER_OF_LTCP_SOCKETS)

//! Default number of sockets supported,max 10 selects are supported
#define RSI_NUMBER_OF_SELECTS (RSI_NUMBER_OF_SOCKETS)

static const sl_wifi_device_configuration_t client_init_configuration = {
  .boot_option = LOAD_NWP_FW,
  .mac_address = NULL,
  .band        = SL_SI91X_WIFI_BAND_2_4GHZ,
  .region_code = US,
  .boot_config = { .oper_mode = SL_SI91X_CLIENT_MODE,
                   .coex_mode = SL_SI91X_WLAN_ONLY_MODE,
                   .feature_bit_map =
#ifdef SLI_SI91X_MCU_INTERFACE
                     (SL_SI91X_FEAT_SECURITY_OPEN | SL_SI91X_FEAT_WPS_DISABLE | SL_SI91X_FEAT_ULP_GPIO_BASED_HANDSHAKE),
#else
                     (SL_SI91X_FEAT_SECURITY_OPEN | SL_SI91X_FEAT_AGGREGATION
#if ENABLE_NWP_POWER_SAVE
                      | SL_SI91X_FEAT_ULP_GPIO_BASED_HANDSHAKE
#endif
                      ),
#endif

                   .tcp_ip_feature_bit_map =
                     (SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT | SL_SI91X_TCP_IP_FEAT_SSL
#ifdef SLI_SI91X_ENABLE_IPV6
                      | SL_SI91X_TCP_IP_FEAT_DHCPV6_CLIENT | SL_SI91X_TCP_IP_FEAT_IPV6
#endif
                      | SL_SI91X_TCP_IP_FEAT_ICMP | SL_SI91X_TCP_IP_FEAT_EXTENSION_VALID),
                   .custom_feature_bit_map = SL_SI91X_CUSTOM_FEAT_EXTENTION_VALID,
                   .ext_custom_feature_bit_map =
                     (SL_SI91X_EXT_FEAT_XTAL_CLK | SL_SI91X_EXT_FEAT_UART_SEL_FOR_DEBUG_PRINTS
                      | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0 | SL_SI91X_EXT_FEAT_LOW_POWER_MODE
                      | MEMORY_CONFIG
#if defined(SLI_SI917) || defined(SLI_SI915)
                      | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
#endif
                      ),
                   .bt_feature_bit_map = 0,
                   .ext_tcp_ip_feature_bit_map =
                     (SL_SI91X_EXT_TCP_IP_WINDOW_SCALING | SL_SI91X_EXT_TCP_IP_TOTAL_SELECTS(1)
                      | SL_SI91X_CONFIG_FEAT_EXTENTION_VALID),
                   .ble_feature_bit_map     = 0,
                   .ble_ext_feature_bit_map = 0,
#ifdef SLI_SI91X_MCU_INTERFACE
                   .config_feature_bit_map = (SL_SI91X_FEAT_SLEEP_GPIO_SEL_BITMAP | SL_SI91X_ENABLE_ENHANCED_MAX_PSP)
#else
#if ENABLE_NWP_POWER_SAVE
                   .config_feature_bit_map = (SL_SI91X_FEAT_SLEEP_GPIO_SEL_BITMAP | SL_SI91X_ENABLE_ENHANCED_MAX_PSP)
#else
                   .config_feature_bit_map = 0
#endif
#endif
  }
};

#if ULOGGER_CUSTOMER_ID == 12345
#error "You must copy the Customer ID from your uLogger Cloud account"
#endif

#if ULOGGER_APPLICATION_ID == 12345
#error "You must copy the Application ID from your uLogger Cloud account"
#endif

// ============================================================================
// Memory Driver for NV Logs
// ============================================================================

extern uint8_t __StackTop;
static const uint8_t *LINKER_STACK_TOP = (uint8_t *)&__StackTop;

static const mem_drv_t nv_log_mem_driver = {
    .read = ulogger_nv_mem_read,
    .write = ulogger_nv_mem_write,
    .erase = ulogger_nv_mem_erase,
};

// Memory control blocks for ulogger
static mem_ctl_block_t ulogger_mem_ctl_blocks[] = {
    {
        .type = ULOGGER_MEM_TYPE_DEBUG_LOG,
        .start_addr = ULOGGER_LOG_NV_START_ADDRESS,
        .end_addr = ULOGGER_LOG_NV_END_ADDRESS,
        .mem_drv = &nv_log_mem_driver
    },
    {
        .type = ULOGGER_MEM_TYPE_STACK_TRACE,
        .start_addr = ULOGGER_EXCEPTION_NV_START_ADDRESS,
        .end_addr = ULOGGER_EXCEPTION_NV_END_ADDRESS,
        .mem_drv = &nv_log_mem_driver
    }
};

// ============================================================================
// ULogger Configuration
// ============================================================================

#define PRETRIG_BUF_SIZE 300
static uint8_t pretrigger_buf[PRETRIG_BUF_SIZE];

static uint32_t get_tick(void);

static ulogger_config_t g_ulogger_config = {
    .fault_reboot_cb = fault_reboot,
    .stack_top_address_cb = stack_get_top_address,
    .flags_level = {
        .flags = 0xFFFFFFFF,  // Enable all modules initially
        .level = ULOG_DEBUG,  // Log all levels
    },
    .mcb_param = ulogger_mem_ctl_blocks,
    .mcb_len = sizeof(ulogger_mem_ctl_blocks),
    .pretrigger_log_count = 0,
    .pretrigger_buffer = pretrigger_buf,
    .pretrigger_buffer_size = PRETRIG_BUF_SIZE,

    .get_tick = get_tick,
    .tick_rate_hz = 32768,          // Sleeptimer RTC at 32.768 kHz

    // Crash dump header metadata
    .application_id = ULOGGER_APPLICATION_ID,
    .git_hash = "no_cache",
    .device_type = ULOGGER_DEVICE_TYPE,
    .device_serial = "1001",
    .version_string = "v2.0.0",
};

// ============================================================================
// Function Definitions
// ============================================================================

// ============================================================================
// Ulogger callback functions
// ============================================================================

static uint32_t get_tick(void)
{
  return sl_sleeptimer_get_tick_count();
}

void fault_reboot(void) {
  NVIC_SystemReset();
}

const void *stack_get_top_address(ulogger_stack_type_t stack_type) {
  const void *stack_addr;
  if (stack_type == ULOGGER_STACK_TYPE_PSP) {
    TaskStatus_t status_task;
    vTaskGetInfo(NULL, &status_task, pdFALSE, eInvalid);
    stack_addr = status_task.pxEndOfStack;
  }
  else {
    stack_addr = LINKER_STACK_TOP;
  }
  return stack_addr;
}

// ============================================================================
// NV Memory Implementation (using static buffer)
// ============================================================================

/**
 * @brief Read data from non-volatile memory
 */
bool ulogger_nv_mem_read(uint32_t address, uint8_t *data, uint32_t size)
{
  assert((address + size) <= sizeof(nv_log_storage));

  //log_local("ulogger_nv_mem_read(%ld): size %ld bytes\r\n", address, size);
  memcpy(data, &nv_log_storage[address], size);
  return true;
}

/**
 * @brief Write data to non-volatile memory
 */
bool ulogger_nv_mem_write(uint32_t address, const uint8_t *data, uint32_t size)
{
  assert((address + size) <= sizeof(nv_log_storage));

  //log_local("ulogger_nv_mem_write(%ld): size %ld bytes\r\n", address, size);
  memcpy(&nv_log_storage[address], data, size);

  return true;
}

/**
 * @brief Erase a region of non-volatile memory
 */
bool ulogger_nv_mem_erase(uint32_t address, uint32_t size)
{
  assert((address + size) <= sizeof(nv_log_storage));

  //log_local("ulogger_nv_mem_erase(%ld): size %ld bytes\r\n", address, size);
  memset(&nv_log_storage[address], 0xFF, size);
  
  return true;
}

static void test_func_1(void) {
  static uint32_t i = 0;
  while (i++ < 3) {
    test_func_1();
  }
  *(volatile uint32_t *)0x000000000 = 0xDEADBEEF;

  return;
}

void sl_si91x_button_isr(uint8_t button, uint8_t state) {
  if (button == SL_BUTTON_BTN0_PIN &&
      state == 0) {
      // Signal the app thread that button was pressed
      if (button_event_flags != NULL) {
        osEventFlagsSet(button_event_flags, 0x01);
      }
      // Wake up app thread
      if (app_thread_id != NULL) {
        osThreadFlagsSet(app_thread_id, 0x01);
      }
  }
}


#if WRAP_PRIVATE_KEY
sl_status_t set_tls_wrapped_private_key(sl_net_credential_id_t id,
                                        sl_net_credential_type_t type,
                                        const uint8_t *key_ptr,
                                        uint32_t key_length,
                                        const uint8_t *iv)
{
  if ((key_ptr == NULL) || (key_length == 0) || ((type == SL_NET_TLS_PRIVATE_KEY_CBC_WRAP) && (iv == NULL))) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  // Append the key_length with the size of IV in case of CBC WRAP mode
  size_t wrapped_key_len = key_length + ((type == SL_NET_TLS_PRIVATE_KEY_CBC_WRAP) ? SL_SI91X_IV_SIZE : 0);

  unsigned char *wrapped_key = (unsigned char *)malloc(wrapped_key_len);
  if (wrapped_key == NULL) {
    return SL_STATUS_ALLOCATION_FAILED;
  }

  // Copy the original key_ptr into the new wrapped key
  memcpy(wrapped_key, key_ptr, key_length);

  // Append the IV to the wrapped key in case of CBC WRAP mode
  if (type == SL_NET_TLS_PRIVATE_KEY_CBC_WRAP) {
    memcpy(wrapped_key + key_length, iv, SL_SI91X_IV_SIZE);
  }

  sl_status_t status = sl_net_set_credential(id, type, wrapped_key, wrapped_key_len);

  free(wrapped_key);
  return status;
}
#endif

void app_init(void)
{
  osThreadNew((osThreadFunc_t)main_application, NULL, &thread_attributes);
}

/******************************
 * Button Event Helper
 ******************************/
/**
 * @brief Check if button event has been triggered
 * @return true if button was pressed and event is pending
 */
bool app_button_event_pending(void)
{
  if (button_event_flags == NULL) {
    return false;
  }
  
  // Check if the event flag is set (non-blocking check)
  uint32_t flags = osEventFlagsGet(button_event_flags);
  
  // Check if bit 0 is set
  if (flags & 0x01) {
    // Clear the flag after checking
    osEventFlagsClear(button_event_flags, 0x01);
    return true;
  }
  
  return false;
}

static void main_application(void *argument)
{
  UNUSED_PARAMETER(argument);
  sl_net_wifi_client_profile_t profile = { 0 };
  sl_ip_address_t ip_address           = { 0 };

  // Store app thread ID for button ISR to signal
  app_thread_id = osThreadGetId();
  
  // Create button event flags
  button_event_flags = osEventFlagsNew(NULL);
  if (button_event_flags == NULL) {
    log_local("Failed to create button event flags");
  }

  select_sem              = osSemaphoreNew(1, 0, NULL);

  // Calculate TCB pxEndOfStack offset using helper function
  // This must be done from within a task context to access task information
  TaskStatus_t status_task;
  vTaskGetInfo(NULL, &status_task, pdFALSE, eInvalid);

  ulogger_init(&g_ulogger_config);
  logging_init_local();
  generate_init_logs_local();

  log_local("\r\nBinary logging system initialized\r\n");

  sl_status_t status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &client_init_configuration, NULL, NULL);
  if (status != SL_STATUS_OK) {
    log_local("\r\nUnexpected error while initializing Wi-Fi: 0x%lx\r\n", status);
    return;
  }
  log_local("\r\nWi-Fi is Initialized\r\n");

#ifdef SLI_SI91X_MCU_INTERFACE
  uint8_t xtal_enable = 1;
  status              = sl_si91x_m4_ta_secure_handshake(SL_SI91X_ENABLE_XTAL, 1, &xtal_enable, 0, NULL);
  if (status != SL_STATUS_OK) {
    log_local("\r\nFailed to bring m4_ta_secure_handshake: 0x%lx\r\n", status);
    return;
  }
  log_local("\r\nM4-NWP secure handshake is successful\r\n");
#endif

  status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID);
  if (status != SL_STATUS_OK) {
    log_local("\r\nError while connecting to Access point: 0x%lx\r\n", status);
    log_local("\r\nCheck your defined DEFAULT_WIFI_CLIENT_PROFILE_SSID and credentials\n");
    return;
  }
  log_local("\r\nConnected to Access point\r\n");

  status = sl_net_get_profile(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &profile);
  if (status != SL_STATUS_OK) {
    log_local("\r\nFailed to get client profile: 0x%lx\r\n", status);
    return;
  }
  log_local("\r\nGetting client profile is successful\r\n");

#ifdef SLI_SI91X_ENABLE_IPV6
  ip_address.type = SL_IPV6;
  memcpy(&ip_address.ip.v6.bytes, &profile.ip.ip.v6.link_local_address, sizeof(sl_ipv6_address_t));
  log_local("\r\nLink Local Address: ");
  print_sl_ip_address(&ip_address);

  memcpy(&ip_address.ip.v6.bytes, &profile.ip.ip.v6.global_address, sizeof(sl_ipv6_address_t));
  log_local("\r\nGlobal Address: ");
  print_sl_ip_address(&ip_address);

  memcpy(&ip_address.ip.v6.bytes, &profile.ip.ip.v6.gateway, sizeof(sl_ipv6_address_t));
  log_local("\r\nGateway Address: ");
  print_sl_ip_address(&ip_address);
  log_local("\r\n");
#else
  ip_address.type = SL_IPV4;
  memcpy(&ip_address.ip.v4.bytes, &profile.ip.ip.v4.ip_address.bytes, sizeof(sl_ipv4_address_t));
  log_local("\r\nIP address is ");
  print_sl_ip_address(&ip_address);
  log_local("\r\n");
#endif

  status = load_certificates_in_flash();
  if (status != SL_STATUS_OK) {
    log_local("\r\nError while loading certificates: 0x%lx\r\n", status);
    return;
  }
  log_local("\r\nLoaded certificates\r\n");

  // Spawn MQTT task as a separate thread
  log_local("\r\nSpawning MQTT task thread...\r\n");
  osThreadId_t mqtt_thread_id = osThreadNew(mqtt_task_thread, NULL, &mqtt_thread_attributes);
  if (mqtt_thread_id == NULL) {
    log_local("\r\nFailed to create MQTT thread\r\n");
    return;
  }
  log_local("\r\nMQTT task thread spawned successfully\r\n");

  while (1) {
    // Block until button event flag is set (yields the task)
    uint32_t flags = osEventFlagsWait(button_event_flags, 0x01, 
                                       osFlagsWaitAny, osWaitForever);
    
    if (flags & 0x01) {
      // Button was pressed - handle it
      log_local("Button was pressed!\r\n");
      logging_generate_test_logs();
      test_func_1();
    }
  }
}



