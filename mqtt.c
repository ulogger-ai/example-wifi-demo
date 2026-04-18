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
#endif

//! Certificates to be loaded
#include "aws_starfield_ca.pem.h"

//! AWS files
#include "aws_iot_error.h"
#include "aws_iot_config.h"
#include "aws_iot_shadow_interface.h"

#include "ulogger_certs_keys.h"
#include "logging.h"

//! ulogger lib API
#include <ulogger.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/******************************************************
*                    Constants
******************************************************/

#define CERTIFICATE_INDEX 0

#define PUBLISH_CORE_DUMP_TOPIC "core-dump/v0/" STR(ULOGGER_CUSTOMER_ID)
#define SUBSCRIBE_TO_TOPIC    "boot/v0/" STR(ULOGGER_CUSTOMER_ID) "/" STR(ULOGGER_APPLICATION_ID) "/" STR(ULOGGER_DEVICE_SERIAL)  //! Subscribe Topic to receive the message from cloud
#define PUBLISH_REGISTER_BOOT "boot/v0/" STR(ULOGGER_CUSTOMER_ID) "/" STR(ULOGGER_APPLICATION_ID)
#define PUBLISH_BINLOG_TOPIC  "binlog/v0/" STR(ULOGGER_CUSTOMER_ID) "/" STR(ULOGGER_APPLICATION_ID)                       //! Binary log publish topic
#define SUBSCRIBE_CONFIG_TOPIC "config/v0/" STR(ULOGGER_CUSTOMER_ID) "/" STR(ULOGGER_APPLICATION_ID) "/" STR(ULOGGER_DEVICE_SERIAL) //! Subscribe topic for cloud-pushed log config
#define MQTT_BOOT_PAYLOAD     "{\"device_type\":\"" ULOGGER_DEVICE_TYPE "\", \"git\": \"no_cache\", \"serial\": 1001, \"version\": \"v2.0.0\"}"

//#define PUBLISH_ON_TOPIC      "logs/v0/975773647/226138"  //! Publish Topic to send the status from application to cloud

#define SUBSCRIBE_QOS         QOS1              //! Quality of Service for subscribed topic "SUBSCRIBE_TO_TOPIC"
#define PUBLISH_QOS           QOS1              //! Quality of Service for publish topic "PUBLISH_ON_TOPIC"
#define PUBLISH_PERIODICITY   30000             //! Publish periodicity in milliseconds
#define ENABLE_NWP_POWER_SAVE 1
#define LOW                   0
#define WRAP_PRIVATE_KEY      0 //! Enable this to wrap the private key

#if ENABLE_NWP_POWER_SAVE
volatile uint8_t powersave_given = 0;
#endif

/******************************************************
*               Message Queue Definitions
******************************************************/

//! Message types for inter-task communication
typedef enum {
  MQTT_MSG_PUBLISH_REQUEST,  //! Request to publish a message
  MQTT_MSG_DISCONNECT_REQUEST //! Request to disconnect
} mqtt_message_type_t;

//! Message structure for MQTT task communication
typedef struct {
  mqtt_message_type_t type;
  char topic[128];
  uint16_t topic_len;
  char payload[1024];
  uint16_t payload_len;
  uint8_t qos;
} mqtt_ipc_message_t;

/******************************************************
*               Function Declarations
******************************************************/
sl_status_t load_certificates_in_flash(void);
sl_status_t start_aws_mqtt(void);
void subscribe_handler(struct _Client *pClient,
                       char *pTopicName,
                       short unsigned int topicNameLen,
                       IoT_Publish_Message_Params *pParams,
                       void *pClientData);
void disconnect_notify_handler(AWS_IoT_Client *pClient, void *data);
ulogger_config_t *ulogger_get_config(void);
sl_status_t mqtt_publish_message(const char *topic, const char *payload, uint8_t qos);
void config_subscribe_handler(struct _Client *pClient, char *pTopicName, short unsigned int topicNameLen, IoT_Publish_Message_Params *pParams, void *pClientData);

//! Enumeration for states in application
typedef enum app_state {
  AWS_MQTT_INIT_STATE,
  AWS_MQTT_CONNECT_STATE,
  AWS_MQTT_INIT_SELECT_STATE,
  AWS_MQTT_SUBSCRIBE_STATE,
  AWS_MQTT_PUBLISH_STATE,
  AWS_MQTT_RECEIVE_STATE,
  AWS_MQTT_DISCONNECT,
  AWS_MQTT_SELECT_STATE,
  AWS_MQTT_SLEEP_STATE,
  AWS_MQTT_PUBLISH_BINLOG_STATE,
  IDLE_STATE
} app_state_t;

volatile app_state_t application_state;
volatile uint8_t boot_message_sent = 0;  // Track if boot message was published
volatile uint8_t boot_puback_received = 0;  // Track if boot PUBACK was received
volatile uint32_t session_token = 0;  // Store received session token
volatile uint8_t binary_log_sent = 0;  // Track if binary log was sent
volatile uint8_t config_subscribed = 0; // Track if config topic is subscribed

//! Log config state – saved defaults allow reverting after timeout
static osTimerId_t config_timeout_timer = NULL;
static ulogger_flags_level_t saved_default_flags_level = { .flags = 0xFFFFFFFF, .level = ULOG_ERROR };
static uint8_t default_flags_saved = 0;

/******************************************************
*               Variable Definitions
******************************************************/

IoT_Publish_Message_Params publish_iot_msg = { 0 };

fd_set read_fds;

osSemaphoreId_t data_received_semaphore;

//! Message queue for inter-task communication with MQTT task
osMessageQueueId_t mqtt_message_queue = NULL;

AWS_IoT_Client mqtt_client = { 0 };
#define RSI_FD_ISSET(x, y) rsi_fd_isset(x, y)
volatile uint8_t check_for_recv_data;
extern osSemaphoreId_t select_sem;
extern volatile uint8_t pub_state, qos1_publish_handle, select_given;
//int32_t status = SL_STATUS_OK;

//! No of ltcp socktes
#define RSI_NUMBER_OF_LTCP_SOCKETS 0

//! Default number of sockets supported,max 10 sockets are supported
#define RSI_NUMBER_OF_SOCKETS (6 + RSI_NUMBER_OF_LTCP_SOCKETS)

//! Default number of sockets supported,max 10 selects are supported
#define RSI_NUMBER_OF_SELECTS (RSI_NUMBER_OF_SOCKETS)

/******************************************************
*               Function Definitions
******************************************************/

// ---------------------------------------------------------------------------
// Log config helpers
// ---------------------------------------------------------------------------

static void config_timeout_callback(void *arg)
{
  (void)arg;
  log_local("\r\nLog config timeout expired – reverting to default config\r\n");
  ulogger_set_flags_level(&saved_default_flags_level);
}

void config_subscribe_handler(struct _Client *pClient,
                              char *pTopicName,
                              short unsigned int topicNameLen,
                              IoT_Publish_Message_Params *pParams,
                              void *data)
{
  UNUSED_PARAMETER(pClient);
  UNUSED_PARAMETER(pTopicName);
  UNUSED_PARAMETER(topicNameLen);
  UNUSED_PARAMETER(data);

  /*
   * Binary packet layout (9 bytes, little-endian):
   *   offset 0:   log_level     (uint8)  0=DEBUG 1=INFO 2=WARNING 3=ERROR 4=CRITICAL
   *   offset 1-4: module_flags  (uint32) bitfield, 0xFFFFFFFF = all modules
   *   offset 5-8: timeout_secs  (uint32) seconds
   */
  if (pParams->payloadLen < 9) {
    log_local("\r\nConfig packet too short (%u bytes), ignoring\r\n", pParams->payloadLen);
    return;
  }

  const uint8_t *buf = (const uint8_t *)pParams->payload;

  uint8_t  level_byte      = buf[0];
  uint32_t new_flags       = (uint32_t)buf[1]
                           | ((uint32_t)buf[2] << 8)
                           | ((uint32_t)buf[3] << 16)
                           | ((uint32_t)buf[4] << 24);
  uint32_t timeout_seconds = (uint32_t)buf[5]
                           | ((uint32_t)buf[6] << 8)
                           | ((uint32_t)buf[7] << 16)
                           | ((uint32_t)buf[8] << 24);

  if (level_byte > ULOG_CRITICAL) {
    log_local("\r\nConfig packet: invalid level byte %u, ignoring\r\n", level_byte);
    return;
  }

  log_local("\r\nLog config received: level=%u, module_flags=0x%08lX, timeout=%lus\r\n",
            level_byte, new_flags, timeout_seconds);

  // Save current defaults the first time we receive a config message
  if (!default_flags_saved) {
    ulogger_config_t *cfg = ulogger_get_config();
    if (cfg) {
      saved_default_flags_level = cfg->flags_level;
    }
    default_flags_saved = 1;
  }

  // Apply the new config
  ulogger_flags_level_t new_cfg = { .flags = new_flags, .level = (uint8_t)level_byte };
  ulogger_set_flags_level(&new_cfg);

  // (Re)start the revert timer
  if (config_timeout_timer == NULL) {
    config_timeout_timer = osTimerNew(config_timeout_callback, osTimerOnce, NULL, NULL);
  }
  if (config_timeout_timer != NULL) {
    osTimerStop(config_timeout_timer);
    osTimerStart(config_timeout_timer, timeout_seconds * 1000U);
  }
}

// ---------------------------------------------------------------------------

void async_socket_select(fd_set *fd_read, fd_set *fd_write, fd_set *fd_except, int32_t status)
{
  UNUSED_PARAMETER(fd_except);
  UNUSED_PARAMETER(fd_write);
  UNUSED_PARAMETER(status);

  //!Check the data pending on this particular socket descriptor
  if (FD_ISSET(mqtt_client.networkStack.socket_id, fd_read)) {
    if (pub_state != 1) { //This check is for handling PUBACK in QOS1
      check_for_recv_data = 1;
      osSemaphoreRelease(data_received_semaphore);
      application_state = AWS_MQTT_SELECT_STATE;
    } else if (pub_state == 1) { //This check is for handling PUBACK in QOS1
      osSemaphoreRelease(select_sem);
    }
  }
  application_state = AWS_MQTT_SELECT_STATE;
}

void disconnect_notify_handler(AWS_IoT_Client *pClient, void *data)
{
  UNUSED_PARAMETER(pClient);
  UNUSED_PARAMETER(data);
  log_local("\r\nMQTT disconnected abruptly and pClient state is: %d\r\n", pClient->clientStatus.clientState);
}

void subscribe_handler(struct _Client *pClient,
                       char *pTopicName,
                       short unsigned int topicNameLen,
                       IoT_Publish_Message_Params *pParams,
                       void *data)
{
  UNUSED_PARAMETER(pClient);
  UNUSED_PARAMETER(pTopicName);
  UNUSED_PARAMETER(topicNameLen);
  UNUSED_PARAMETER(data);
  log_local("\r\n========================================\r\n");
  log_local("Session Token Received!\r\n");
  log_local("Topic: %.*s\r\n", topicNameLen, pTopicName);
  log_local("Payload Length: %d\r\n", pParams->payloadLen);
  log_local("Session Token: %.*s\r\n", pParams->payloadLen, (char *)pParams->payload);
  log_local("========================================\r\n\n");

  // Parse session token from JSON (simple parsing for {"token": 1234567890})
  char *payload = (char *)pParams->payload;
  char *token_start = strstr(payload, "\"token\":");
  if (token_start) {
    token_start += 8; // Skip past "token":
    while (*token_start == ' ') token_start++; // Skip whitespace
    session_token = strtoul(token_start, NULL, 10);
    log_local("Parsed session token: %lu\r\n", session_token);
  }
}

sl_status_t load_certificates_in_flash(void)
{
  sl_status_t status;

  // Load SSL CA certificate
  status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                 SL_NET_SIGNING_CERTIFICATE,
                                 aws_starfield_ca,
                                 sizeof(aws_starfield_ca) - 1);
  if (status != SL_STATUS_OK) {
    log_local("\r\nLoading TLS CA certificate in to FLASH Failed, Error Code : 0x%lX\r\n", status);
    return status;
  }
  log_local("\r\nLoading TLS CA certificate at index %d Successful\r\n", CERTIFICATE_INDEX);

  // Load SSL Client certificate
  status = sl_net_set_credential(SL_NET_TLS_CLIENT_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                 SL_NET_CERTIFICATE,
                                 ulogger_user_certificate,
                                 sizeof(ulogger_user_certificate) - 1);
  if (status != SL_STATUS_OK) {
    log_local("\r\nLoading TLS certificate in to FLASH failed, Error Code : 0x%lX\r\n", status);
    return status;
  }
  log_local("\r\nLoading TLS Client certificate at index %d Successful\r\n", CERTIFICATE_INDEX);

#if WRAP_PRIVATE_KEY
  wrap_config.key_type     = SL_SI91X_TRANSPARENT_KEY;
  wrap_config.key_size     = (sizeof(aws_client_private_key) - 1);
  wrap_config.wrap_iv_mode = SL_SI91X_WRAP_IV_ECB_MODE;
  memcpy(wrap_config.key_buffer, aws_client_private_key, wrap_config.key_size);

  if (wrap_config.wrap_iv_mode == SL_SI91X_WRAP_IV_CBC_MODE) {
    memcpy(wrap_config.wrap_iv, iv, SL_SI91X_IV_SIZE);
  }

  // Since the output of wrap API is a 16 byte aligned, update the value of wrapped_private_key_length to the next multiple of 16 bytes.
  uint32_t wrapped_private_key_length =
    ((wrap_config.key_size + SL_SI91X_DEFAULT_16BYTE_ALIGN) & (~SL_SI91X_DEFAULT_16BYTE_ALIGN));

  wrapped_private_key = (uint8_t *)malloc(wrapped_private_key_length);
  SL_VERIFY_POINTER_OR_RETURN(wrapped_private_key, SL_STATUS_NULL_POINTER);

  status = sl_si91x_wrap(&wrap_config, wrapped_private_key);
  if (status != SL_STATUS_OK) {
    log_local("\r\nWrapping private key failed, Error Code : 0x%lX\r\n", status);
    free(wrapped_private_key);
    return status;
  }
  log_local("\r\nWrapping private key is successful\r\n");

  sl_net_credential_type_t credential_type = ((wrap_config.wrap_iv_mode == SL_SI91X_WRAP_IV_ECB_MODE)
                                                ? SL_NET_TLS_PRIVATE_KEY_ECB_WRAP
                                                : SL_NET_TLS_PRIVATE_KEY_CBC_WRAP);

  status = set_tls_wrapped_private_key(SL_NET_TLS_CLIENT_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                       credential_type,
                                       wrapped_private_key,
                                       wrapped_private_key_length,
                                       ((wrap_config.wrap_iv_mode == SL_SI91X_WRAP_IV_ECB_MODE) ? NULL : iv));
  if (status != SL_STATUS_OK) {
    log_local("\r\nLoading TLS Client wrapped private key in to FLASH Failed, Error Code : 0x%lX\r\n", status);
    free(wrapped_private_key);
    return status;
  }
  log_local("\r\nLoading TLS Client wrapped private key at index %d Successful\r\n", CERTIFICATE_INDEX);

  free(wrapped_private_key);
#else
  // Load SSL Client private key
  status = sl_net_set_credential(SL_NET_TLS_CLIENT_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                 SL_NET_PRIVATE_KEY,
                                 ulogger_user_private_key,
                                 sizeof(ulogger_user_private_key) - 1);
  if (status != SL_STATUS_OK) {
    log_local("\r\nLoading TLS Client private key in to FLASH Failed, Error Code : 0x%lX\r\n", status);
    return status;
  }
  log_local("\r\nLoading TLS Client private key at index %d Successful\r\n", CERTIFICATE_INDEX);
#endif

  return SL_STATUS_OK;
}

/******************************************************
*          Inter-Task Communication APIs
******************************************************/

/**
 * @brief Publish a message to MQTT via the message queue
 * 
 * @param topic The MQTT topic to publish to
 * @param payload The message payload
 * @param qos Quality of Service (0 or 1)
 * @return SL_STATUS_OK on success, error code otherwise
 */
sl_status_t mqtt_publish_message(const char *topic, const char *payload, uint8_t qos)
{
  if (mqtt_message_queue == NULL) {
    log_local("\r\nMQTT message queue not initialized\r\n");
    return SL_STATUS_NOT_INITIALIZED;
  }

  if (topic == NULL || payload == NULL) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  mqtt_ipc_message_t msg = { 0 };
  msg.type = MQTT_MSG_PUBLISH_REQUEST;
  msg.qos = qos;

  // Copy topic
  uint16_t topic_len = strlen(topic);
  if (topic_len >= sizeof(msg.topic)) {
    log_local("\r\nTopic too long (max %u bytes)\r\n", (unsigned int)sizeof(msg.topic));
    return SL_STATUS_INVALID_PARAMETER;
  }
  strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
  msg.topic_len = topic_len;

  // Copy payload
  uint16_t payload_len = strlen(payload);
  if (payload_len >= sizeof(msg.payload)) {
    log_local("\r\nPayload too long (max %u bytes)\r\n", (unsigned int)sizeof(msg.payload));
    return SL_STATUS_INVALID_PARAMETER;
  }
  strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
  msg.payload_len = payload_len;

  // Send message to queue (non-blocking, timeout = 0)
  osStatus_t queue_status = osMessageQueuePut(mqtt_message_queue, &msg, 0, 0);
  if (queue_status != osOK) {
    log_local("\r\nFailed to queue MQTT message (status: %d)\r\n", queue_status);
    return SL_STATUS_BUSY;
  }

  log_local("\r\nQueued MQTT publish request: topic=%s, len=%u\r\n", topic, payload_len);
  return SL_STATUS_OK;
}

// MQTT thread wrapper function
void mqtt_task_thread(void *argument)
{
  UNUSED_PARAMETER(argument);
  
  // Create message queue for inter-task communication (10 messages max)
  mqtt_message_queue = osMessageQueueNew(10, sizeof(mqtt_ipc_message_t), NULL);
  if (mqtt_message_queue == NULL) {
    log_local("\r\nFailed to create MQTT message queue\r\n");
    return;
  }
  log_local("\r\nMQTT message queue created successfully\r\n");

  data_received_semaphore = osSemaphoreNew(1, 0, NULL);

  start_aws_mqtt();
}

sl_status_t start_aws_mqtt(void)
{
#if !(defined(SLI_SI91X_MCU_INTERFACE) && ENABLE_NWP_POWER_SAVE)
  uint32_t start_time         = 0;
  uint8_t publish_timer_start = 0;
  uint8_t publish_msg         = 0;
#endif
  IoT_Error_t rc = FAILURE;

  IoT_Client_Init_Params mqtt_init_params       = iotClientInitParamsDefault;
  IoT_Client_Connect_Params mqtt_connect_params = iotClientConnectParamsDefault;

  sl_mac_address_t mac_addr = { 0 };
  char mac_id[18];
  char client_id[25];
  sl_wifi_get_mac_address(SL_WIFI_CLIENT_INTERFACE, &mac_addr);
  sprintf(mac_id,
          "%x:%x:%x:%x:%x:%x",
          mac_addr.octet[0],
          mac_addr.octet[1],
          mac_addr.octet[2],
          mac_addr.octet[3],
          mac_addr.octet[4],
          mac_addr.octet[5]);
  log_local("\r\nMAC ID: %s \r\n", mac_id);
  sprintf(client_id, "silabs_%s", mac_id);
  log_local("\r\nClient ID: %s\r\n", client_id);

  sl_wifi_firmware_version_t fw_version = { 0 };
  int32_t status                             = sl_wifi_get_firmware_version(&fw_version);
  if (status != SL_STATUS_OK) {
    log_local("\r\nFirmware version Failed, Error Code : 0x%lX\r\n", status);
  } else {
    print_firmware_version(&fw_version);
  }

  mqtt_init_params.enableAutoReconnect       = true;
  mqtt_init_params.pHostURL                  = AWS_IOT_MQTT_HOST;
  mqtt_init_params.port                      = AWS_IOT_MQTT_PORT;
  mqtt_init_params.pRootCALocation           = (char *)aws_starfield_ca;
  mqtt_init_params.pDeviceCertLocation       = (char *)ulogger_user_certificate;
  mqtt_init_params.pDevicePrivateKeyLocation = (char *)ulogger_user_private_key;
  mqtt_init_params.mqttCommandTimeout_ms     = 20000;
  mqtt_init_params.tlsHandshakeTimeout_ms    = 5000;
  mqtt_init_params.isSSLHostnameVerify       = true;
  mqtt_init_params.disconnectHandler         = disconnect_notify_handler;
  mqtt_init_params.disconnectHandlerData     = NULL;

  mqtt_connect_params.keepAliveIntervalInSec = 1200;
  mqtt_connect_params.isCleanSession         = true;
  mqtt_connect_params.MQTTVersion            = MQTT_3_1_1;
  mqtt_connect_params.pClientID              = AWS_IOT_MQTT_CLIENT_ID;//client_id;
  mqtt_connect_params.clientIDLen            = (uint16_t)strlen(AWS_IOT_MQTT_CLIENT_ID);
  mqtt_connect_params.isWillMsgPresent       = false;

  application_state = AWS_MQTT_INIT_STATE;

  char test[33];
  strncpy(test, SUBSCRIBE_TO_TOPIC, strlen(SUBSCRIBE_TO_TOPIC) + 1);
  while (1) {

    switch (application_state) {

      case AWS_MQTT_INIT_STATE: {
        rc = aws_iot_mqtt_init(&mqtt_client, &mqtt_init_params);
        if (SUCCESS != rc) {
          application_state = AWS_MQTT_INIT_STATE;
          log_local("\r\nMQTT Initialization failed with error: %d\r\n", rc);
        } else {
          application_state = AWS_MQTT_CONNECT_STATE;
        }

      } break;
      case AWS_MQTT_CONNECT_STATE: {
        rc = aws_iot_mqtt_connect(&mqtt_client, &mqtt_connect_params);
        if (SUCCESS != rc) {
          if (rc == NETWORK_ALREADY_CONNECTED_ERROR) {
            log_local("\r\nNetwork is already connected\r\n");
            application_state = AWS_MQTT_SUBSCRIBE_STATE;
          } else {
            log_local("\r\nMQTT connect failed with error: %d\r\n", rc);
            application_state = AWS_MQTT_INIT_STATE;
          }
        } else {
          log_local("\r\nConnected to AWS IoT Cloud\n");
          // Initialize select before publishing
          application_state = AWS_MQTT_INIT_SELECT_STATE;
        }
      } break;

      case AWS_MQTT_INIT_SELECT_STATE: {
        // Go directly to publish - select will be called after subscription
        log_local("\rReady to publish boot message\r\n");
        application_state = AWS_MQTT_PUBLISH_STATE;
      } break;

      case AWS_MQTT_SUBSCRIBE_STATE: {
        rc = aws_iot_mqtt_subscribe(&mqtt_client,
                                    (const char *)&test,
                                    strlen(test),
                                    SUBSCRIBE_QOS,
                                    subscribe_handler,
                                    NULL);

        if (SUCCESS != rc) {
          if (NETWORK_DISCONNECTED_ERROR == rc) {
            log_local("\r\nSubscription failed with error: %d\r\n", rc);
            application_state = AWS_MQTT_CONNECT_STATE;
          } else if (NETWORK_ATTEMPTING_RECONNECT == rc) {
            // If the client is attempting to reconnect skip the rest of the loop
            continue;
          } else {
            application_state = AWS_MQTT_SUBSCRIBE_STATE;
          }
        } else {
          log_local("\rSubscribed to boot topic: %s with QoS%d\n", test, SUBSCRIBE_QOS);

          // Also subscribe to the cloud log-config topic
          char config_topic[] = SUBSCRIBE_CONFIG_TOPIC;
          IoT_Error_t config_rc = aws_iot_mqtt_subscribe(&mqtt_client,
                                                         config_topic,
                                                         strlen(config_topic),
                                                         SUBSCRIBE_QOS,
                                                         config_subscribe_handler,
                                                         NULL);
          if (config_rc == SUCCESS) {
            log_local("\rSubscribed to config topic: %s\n", config_topic);
            config_subscribed = 1;
          } else {
            log_local("\rConfig topic subscription failed (rc=%d) – continuing\r\n", config_rc);
          }

          log_local("\rNow calling select to monitor for session token...\n");
          select_given = 0;  // Ensure select will be called in SELECT_STATE
          check_for_recv_data = 0;  // Clear any stale data flag
          application_state = AWS_MQTT_SELECT_STATE;
        }

      } break;

      case AWS_MQTT_SELECT_STATE: {
        if (!select_given) {
          // Call select to monitor socket for incoming messages
          select_given = 1;
          memset(&read_fds, 0, sizeof(fd_set));

          FD_SET(mqtt_client.networkStack.socket_id, &read_fds);

          status =
            sl_si91x_select(mqtt_client.networkStack.socket_id + 1, &read_fds, NULL, NULL, NULL, async_socket_select);

          if (status != SL_STATUS_OK) {
            log_local("\rSelect failed with status: 0x%lX\r\n", status);
          }
        }

        if (check_for_recv_data) {
          check_for_recv_data = 0;
          select_given        = 0;
          application_state   = AWS_MQTT_RECEIVE_STATE;
        } else if (boot_message_sent && boot_puback_received) {
          // After boot message and subscription, periodically yield to main_application
          // Stay in SELECT_STATE and wait for async_socket_select callback
          // The callback will set check_for_recv_data when a message arrives
          osDelay(100);  // Yield for 100ms to allow main_application to run
        } else if (SUBSCRIBE_QOS == QOS1 || PUBLISH_QOS == QOS1) {
          if (qos1_publish_handle == 0) {
            application_state = AWS_MQTT_PUBLISH_STATE;
          } else {
#if ENABLE_NWP_POWER_SAVE
            qos1_publish_handle = 0;
            application_state   = AWS_MQTT_SLEEP_STATE;
#else
          application_state = AWS_MQTT_PUBLISH_STATE;
#endif
          }
        } else if (SUBSCRIBE_QOS == QOS0 || PUBLISH_QOS == QOS0) {
          application_state = AWS_MQTT_PUBLISH_STATE;
        }

        if (session_token != 0) {
#define CRASH_ENABLED 0
#ifdef CRASH_ENABLED
          uint32_t core_dump_sz_bytes = ulogger_get_core_dump_size();
          if (core_dump_sz_bytes != 0) {
            log_local("Core dump found: %ld bytes\r\n", core_dump_sz_bytes);

            // Allocate buffer for the total size returned
            uint8_t *core_dump = (uint8_t *)malloc(core_dump_sz_bytes);
            Mem_read(ULOGGER_MEM_TYPE_STACK_TRACE, 0, core_dump, core_dump_sz_bytes);

            // Publish binary log to MQTT (header + log data)
            log_local("Publishing core dump: %lu bytes to topic: %s\r\n", core_dump_sz_bytes, PUBLISH_CORE_DUMP_TOPIC);

            publish_iot_msg.qos        = PUBLISH_QOS;
            publish_iot_msg.payload    = (char *)core_dump;
            publish_iot_msg.isRetained = 0;
            publish_iot_msg.payloadLen = core_dump_sz_bytes;

            if (SUBSCRIBE_QOS == QOS1 || PUBLISH_QOS == QOS1) {
              pub_state = 1;
            }
            rc = aws_iot_mqtt_publish(&mqtt_client, PUBLISH_CORE_DUMP_TOPIC, strlen(PUBLISH_CORE_DUMP_TOPIC), &publish_iot_msg);
            log_local("Core dump publish return code: %d\r\n", rc);
            Mem_erase_all(ULOGGER_MEM_TYPE_STACK_TRACE);
            free(core_dump);
          }
#endif // CRASH_ENABLED
          // Get NV log usage (includes header + log data)
          uint32_t total_size = ulogger_get_nv_log_usage();
          if (total_size > 0) {
            log_local("NV Log usage: %lu bytes\r\n", total_size);

            // Allocate buffer for the total size returned
            uint8_t *log_buffer = (uint8_t *)malloc(total_size);

            // Read logs with header prepended (library handles header creation)
            uint32_t bytes_written = ulogger_read_nv_logs_with_header(log_buffer, total_size, session_token, 0);

            if (bytes_written > 0) {
              // Publish binary log to MQTT (header + log data)
              log_local("Publishing binary log: %lu bytes to topic: %s\r\n", bytes_written, PUBLISH_BINLOG_TOPIC);

              publish_iot_msg.qos        = PUBLISH_QOS;
              publish_iot_msg.payload    = (char *)log_buffer;
              publish_iot_msg.isRetained = 0;
              publish_iot_msg.payloadLen = bytes_written;

              if (SUBSCRIBE_QOS == QOS1 || PUBLISH_QOS == QOS1) {
                pub_state = 1;
              }
              rc = aws_iot_mqtt_publish(&mqtt_client, PUBLISH_BINLOG_TOPIC, strlen(PUBLISH_BINLOG_TOPIC), &publish_iot_msg);

              if (rc != SUCCESS) {
                log_local("Binary log publish failed with error: %d\r\n", rc);
              } else {
                log_local("Binary log published successfully!\r\n");
                binary_log_sent = 1;
                // Clear logs after successful publish
                ulogger_clear_nv_logs();
              }
            } else {
              log_local("Failed to read NV logs\r\n");
            }

            free(log_buffer);
          }
        }

      } break;

      case AWS_MQTT_RECEIVE_STATE: {
        rc = aws_iot_shadow_yield(&mqtt_client, 1);
        if (SUCCESS == rc) {
#if !(defined(SLI_SI91X_MCU_INTERFACE) && ENABLE_NWP_POWER_SAVE)
          publish_msg = 1;
#endif
          // After receiving session token, generate logs and publish binary log
//          if (session_token != 0 && !binary_log_sent) {
//            log_local("\r\nSession token received, generating and publishing binary logs...\r\n");
//            application_state = AWS_MQTT_PUBLISH_BINLOG_STATE;
//          } else {
//            application_state = AWS_MQTT_SELECT_STATE;
//          }
          // go back to the select state
          application_state = AWS_MQTT_SELECT_STATE;
        } else {
          application_state = AWS_MQTT_SELECT_STATE;
        }
      } break;

      case AWS_MQTT_PUBLISH_BINLOG_STATE: {
        // Clear any old logs first to ensure clean slate
        ulogger_clear_nv_logs();

        // Generate test logs
        //generate_test_logs();

        // Flush pretrigger buffer to NV memory before reading
        ulogger_flush_pretrigger_to_nv();

        // Get NV log usage (includes header + log data)
        uint32_t total_size = ulogger_get_nv_log_usage();

        log_local("NV Log usage: %lu bytes\r\n", total_size);

        if (total_size > 0) {
          // Allocate buffer for the total size returned
          uint8_t *log_buffer = (uint8_t *)malloc(total_size);

          if (log_buffer == NULL) {
            log_local("Failed to allocate buffer for binary log publish\r\n");
            binary_log_sent = 1;  // Don't retry
            application_state = IDLE_STATE;
            break;
          }

          // Read logs with header prepended (library handles header creation)
          uint32_t bytes_written = ulogger_read_nv_logs_with_header(log_buffer, total_size, session_token, 0);

          if (bytes_written > 0) {
            // Publish binary log to MQTT (header + log data)
            log_local("Publishing binary log: %lu bytes to topic: %s\r\n", bytes_written, PUBLISH_BINLOG_TOPIC);

            publish_iot_msg.qos        = PUBLISH_QOS;
            publish_iot_msg.payload    = (char *)log_buffer;
            publish_iot_msg.isRetained = 0;
            publish_iot_msg.payloadLen = bytes_written;

            rc = aws_iot_mqtt_publish(&mqtt_client, PUBLISH_BINLOG_TOPIC, strlen(PUBLISH_BINLOG_TOPIC), &publish_iot_msg);

            if (rc != SUCCESS) {
              log_local("Binary log publish failed with error: %d\r\n", rc);
            } else {
              log_local("Binary log published successfully!\r\n");
              binary_log_sent = 1;
              // Clear logs after successful publish
              ulogger_clear_nv_logs();
            }
          } else {
            log_local("Failed to read NV logs\r\n");
          }

          free(log_buffer);
        } else {
          log_local("No logs to publish\r\n");
          binary_log_sent = 1;
        }

        application_state = IDLE_STATE;

      } break;

      case AWS_MQTT_PUBLISH_STATE: {
        // First, publish the boot message
        if (!boot_message_sent) {
          publish_iot_msg.qos        = PUBLISH_QOS;
          publish_iot_msg.payload    = MQTT_BOOT_PAYLOAD;
          publish_iot_msg.isRetained = 0;
          publish_iot_msg.payloadLen = strlen(MQTT_BOOT_PAYLOAD);

          log_local("\rPublishing boot message: %s\n", MQTT_BOOT_PAYLOAD);

          rc = aws_iot_mqtt_publish(&mqtt_client, PUBLISH_REGISTER_BOOT, strlen(PUBLISH_REGISTER_BOOT), &publish_iot_msg);

          if (rc != SUCCESS) {
            log_local("\r\nMQTT Boot Publish failed with error: %d\n", rc);
            application_state = AWS_MQTT_DISCONNECT;
            break;
          }

          log_local("\rBoot message published and acknowledged!\r\n");
          boot_message_sent = 1;
          boot_puback_received = 1;  // PUBACK already handled by SDK
          // Now subscribe to receive session token
          application_state = AWS_MQTT_SUBSCRIBE_STATE;
          break;
        }

        // Normal log publishing (after boot)
#if !(defined(SLI_SI91X_MCU_INTERFACE) && ENABLE_NWP_POWER_SAVE)
        if ((!publish_timer_start) || publish_msg) {
#endif

#if !(defined(SLI_SI91X_MCU_INTERFACE) && ENABLE_NWP_POWER_SAVE)
          publish_msg = 0;
          if (!publish_timer_start) {
            publish_timer_start = 1;
            start_time          = sl_si91x_host_get_timestamp();
          }

        } else {

          if (sl_si91x_host_elapsed_time(start_time) >= PUBLISH_PERIODICITY) {
            start_time          = 0;
            publish_timer_start = 0;
            publish_msg         = 1;
            continue;
          }
        }
#endif

#if ENABLE_NWP_POWER_SAVE
        sl_wifi_performance_profile_v2_t performance_profile = { .profile         = ASSOCIATED_POWER_SAVE_LOW_LATENCY,
                                                                 .listen_interval = 1000 };
        if (!powersave_given) {
          rc = sl_wifi_set_performance_profile_v2(&performance_profile);
          if (rc != SL_STATUS_OK) {
            log_local("\r\nPower save configuration Failed, Error Code : %d\r\n", rc);
          }
          log_local("\r\nAssociated Power Save is enabled\r\n");
          powersave_given = 1;
        }
        if (SUBSCRIBE_QOS == QOS1 || PUBLISH_QOS == QOS1) {
          application_state = AWS_MQTT_SELECT_STATE;
        } else if (SUBSCRIBE_QOS == QOS0 || PUBLISH_QOS == QOS0) {
          application_state = AWS_MQTT_SLEEP_STATE;
        }
#else
        application_state = IDLE_STATE;
#endif
      } break;

#if ENABLE_NWP_POWER_SAVE
      case AWS_MQTT_SLEEP_STATE: {
        if (select_given == 1 && (check_for_recv_data != 1)) {

#ifdef SLI_SI91X_MCU_INTERFACE
#if (SL_SI91X_TICKLESS_MODE == 0)
          sl_si91x_power_manager_sleep();
#else
          log_local("\rM4 going to power save state..\r\n");
          log_local("\rselect_given before sleep: %d\r\n", select_given);
          if (osSemaphoreAcquire(data_received_semaphore, PUBLISH_PERIODICITY) == osOK) {
            log_local("\rM4 woke up from power save state..\r\n");
          }
#endif
#endif
        }
        application_state = AWS_MQTT_SELECT_STATE;

      } break;
#endif
      case AWS_MQTT_DISCONNECT: {
        rc = aws_iot_mqtt_disconnect(&mqtt_client);
        if (SUCCESS != rc) {
          log_local("\r\nMQTT disconnection error\r\n");
        }
        // Reset boot flags for clean reconnection
        boot_message_sent = 0;
        boot_puback_received = 0;
        pub_state = 0;
        select_given = 0;
        session_token = 0;
        binary_log_sent = 0;
        application_state = AWS_MQTT_INIT_STATE;

      } break;
      case IDLE_STATE: {

        application_state = AWS_MQTT_SELECT_STATE;

      } break;

      default:
        break;
    }
  }

  return rc;
}
