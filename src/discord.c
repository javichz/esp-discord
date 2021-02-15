#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_transport_ws.h"
#include "discord/models.h"
#include "discord/private/models.h"
#include "discord.h"

#define DISCORD_WS_BUFFER_SIZE (512)
#define DISCORD_MIN_BUFFER_SIZE (1024)
#define DISCORD_TASK_STACK_SIZE (4 * 1024)
#define DISCORD_TASK_PRIORITY (3)

#define DC_LOCK(CODE) {\
    if (xSemaphoreTakeRecursive(client->lock, portMAX_DELAY) != pdPASS) {\
        DISCORD_LOGE("Could not lock");\
        return ESP_FAIL;\
    }\
    CODE;\
    xSemaphoreGiveRecursive(client->lock);\
}

static const char* TAG = DISCORD_LOG_TAG;

ESP_EVENT_DEFINE_BASE(DISCORD_EVENTS);

typedef enum {
    DISCORD_CLOSE_REASON_NOT_REQUESTED = -1,
    DISCORD_CLOSE_REASON_RECONNECT,
    DISCORD_CLOSE_REASON_LOGOUT
} discord_close_reason_t;

typedef enum {
    DISCORD_CLIENT_STATE_ERROR = -2,
    DISCORD_CLIENT_STATE_DISCONNECTED = -1,
    DISCORD_CLIENT_STATE_UNKNOWN,
    DISCORD_CLIENT_STATE_INIT,
    DISCORD_CLIENT_STATE_CONNECTING,
    DISCORD_CLIENT_STATE_CONNECTED
} discord_client_state_t;

enum {
    DISCORD_CLIENT_STATUS_BIT_BUFFER_READY = (1 << 0)
};

typedef struct {
    bool running;
    int interval;
    uint64_t tick_ms;
    bool received_ack;
} discord_heartbeater_t;

struct discord_client {
    discord_client_state_t state;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;
    EventGroupHandle_t status_bits;
    esp_event_loop_handle_t event_handle;
    discord_client_config_t* config;
    bool running;
    esp_websocket_client_handle_t ws;
    discord_heartbeater_t heartbeater;
    discord_gateway_session_t* session;
    int last_sequence_number;
    discord_close_reason_t close_reason;
    char* buffer;
    int buffer_len;
};

static uint64_t dc_tick_ms(void) {
    return esp_timer_get_time() / 1000;
}

static esp_err_t gw_heartbeat_send_if_expired(discord_client_handle_t client);
#define gw_heartbeat_init(client) gw_heartbeat_stop(client)
static esp_err_t gw_heartbeat_start(discord_client_handle_t client, discord_gateway_hello_t* hello);
static esp_err_t gw_heartbeat_stop(discord_client_handle_t client);

static esp_err_t gw_reset(discord_client_handle_t client) {
    DISCORD_LOG_FOO();
    
    gw_heartbeat_stop(client);
    client->last_sequence_number = DISCORD_NULL_SEQUENCE_NUMBER;
    client->close_reason = DISCORD_CLOSE_REASON_NOT_REQUESTED;
    xEventGroupClearBits(client->status_bits, DISCORD_CLIENT_STATUS_BIT_BUFFER_READY);
    client->buffer_len = 0;

    return ESP_OK;
}

/**
 * @brief Send payload (serialized to json) to gateway. Payload will be automatically freed
 */
static esp_err_t gw_send(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    DISCORD_LOG_FOO();

    DC_LOCK(
        char* payload_raw = discord_model_gateway_payload_serialize(payload);

        DISCORD_LOGD("%s", payload_raw);

        esp_websocket_client_send_text(client->ws, payload_raw, strlen(payload_raw), portMAX_DELAY);
        free(payload_raw);
    );

    return ESP_OK;
}

static esp_err_t gw_buffer_websocket_data(discord_client_handle_t client, esp_websocket_event_data_t* data) {
    DC_LOCK(
        if(data->payload_len > client->config->buffer_size) {
            DISCORD_LOGW("Payload too big. Wider buffer required.");
            return ESP_FAIL;
        }
        
        DISCORD_LOGD("Received data:\n%.*s", data->data_len, data->data_ptr);

        DISCORD_LOGD("Buffering...");
        memcpy(client->buffer + data->payload_offset, data->data_ptr, data->data_len);

        if((client->buffer_len = data->data_len + data->payload_offset) >= data->payload_len) {
            DISCORD_LOGD("Buffering done.");
            xEventGroupSetBits(client->status_bits, DISCORD_CLIENT_STATUS_BIT_BUFFER_READY);
        }
    );

    return ESP_OK;
}

static void gw_websocket_event_handler(void* handler_arg, esp_event_base_t base, int32_t event_id, void* event_data) {
    discord_client_handle_t client = (discord_client_handle_t) handler_arg;
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*) event_data;

    if(data->op_code == WS_TRANSPORT_OPCODES_PONG) { // ignore PONG frame
        return;
    }

    DISCORD_LOGD("Received WebSocket frame (op_code=%d, payload_len=%d, data_len=%d, payload_offset=%d)",
        data->op_code,
        data->payload_len, 
        data->data_len, 
        data->payload_offset
    );

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            DISCORD_LOGD("WEBSOCKET_EVENT_CONNECTED");
            client->state = DISCORD_CLIENT_STATE_CONNECTING;
            break;

        case WEBSOCKET_EVENT_DATA:
            if(data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                gw_buffer_websocket_data(client, data);
            }
            break;
        
        case WEBSOCKET_EVENT_ERROR:
            DISCORD_LOGD("WEBSOCKET_EVENT_ERROR");
            client->state = DISCORD_CLIENT_STATE_ERROR;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            DISCORD_LOGD("WEBSOCKET_EVENT_DISCONNECTED");
            client->state = DISCORD_CLIENT_STATE_DISCONNECTED;
            break;

        case WEBSOCKET_EVENT_CLOSED:
            DISCORD_LOGD("WEBSOCKET_EVENT_CLOSED");
            client->state = DISCORD_CLIENT_STATE_DISCONNECTED;
            break;
            
        default:
            DISCORD_LOGW("WEBSOCKET_EVENT_UNKNOWN %d", event_id);
            break;
    }
}

static esp_err_t gw_start(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    esp_err_t err;

    DC_LOCK(
        client->state = DISCORD_CLIENT_STATE_INIT;
        err = esp_websocket_client_start(client->ws);
    );

    return err;
}

static esp_err_t gw_open(discord_client_handle_t client) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;
    
    DISCORD_LOG_FOO();

    esp_websocket_client_config_t ws_cfg = {
        .uri = "wss://gateway.discord.gg/?v=8&encoding=json",
        .buffer_size = DISCORD_WS_BUFFER_SIZE
    };

    client->ws = esp_websocket_client_init(&ws_cfg);

    ESP_ERROR_CHECK(esp_websocket_register_events(client->ws, WEBSOCKET_EVENT_ANY, gw_websocket_event_handler, (void*) client));
    ESP_ERROR_CHECK(gw_start(client));

    return ESP_OK;
}

static esp_err_t gw_close(discord_client_handle_t client, discord_close_reason_t reason) {
    DISCORD_LOG_FOO();

    DC_LOCK(
        client->close_reason = reason;

        if(esp_websocket_client_is_connected(client->ws)) {
            esp_websocket_client_close(client->ws, portMAX_DELAY);
        }
        
        gw_reset(client);

        client->state = DISCORD_CLIENT_STATE_INIT;
    );

    return ESP_OK;
}

static esp_err_t gw_reconnect(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    gw_close(client, DISCORD_CLOSE_REASON_RECONNECT);
    ESP_ERROR_CHECK(gw_start(client));

    return ESP_OK;
}

static discord_client_config_t* dc_config_copy(const discord_client_config_t* config) {
    discord_client_config_t* clone = calloc(1, sizeof(discord_client_config_t));

    clone->token = strdup(config->token);
    clone->intents = config->intents;
    clone->buffer_size = config->buffer_size > DISCORD_MIN_BUFFER_SIZE ? config->buffer_size : DISCORD_MIN_BUFFER_SIZE;

    return clone;
}

static void dc_config_free(discord_client_config_t* config) {
    if(config == NULL)
        return;

    free(config->token);
    free(config);
}

static esp_err_t gw_init(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    ESP_ERROR_CHECK(gw_heartbeat_init(client));
    gw_reset(client);

    return ESP_OK;
}

static esp_err_t dc_dispatch_event(discord_client_handle_t client, discord_event_id_t event, discord_event_data_ptr_t data_ptr) {
    DISCORD_LOG_FOO();

    esp_err_t err;

    discord_event_data_t event_data;
    event_data.client = client;
    event_data.ptr = data_ptr;

    if ((err = esp_event_post_to(client->event_handle, DISCORD_EVENTS, event, &event_data, sizeof(discord_event_data_t), portMAX_DELAY)) != ESP_OK) {
        return err;
    }

    return esp_event_loop_run(client->event_handle, 0);
}

static esp_err_t gw_identify(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    return gw_send(client, discord_model_gateway_payload(
        DISCORD_OP_IDENTIFY,
        discord_model_gateway_identify(
            client->config->token,
            client->config->intents,
            discord_model_gateway_identify_properties(
                "freertos",
                "esp-idf",
                "esp32"
            )
        )
    ));
}

/**
 * @brief Check event name in payload and invoke appropriate functions
 */
static esp_err_t gw_dispatch(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    DISCORD_LOG_FOO();
    
    if(DISCORD_GATEWAY_EVENT_READY == payload->t) {
        if(client->session != NULL) {
            discord_model_gateway_session_free(client->session);
        }

        client->session = (discord_gateway_session_t*) payload->d;

        // Detach pointer in order to prevent session deallocation by payload free function
        payload->d = NULL;

        client->state = DISCORD_CLIENT_STATE_CONNECTED;
        
        DISCORD_LOGD("Identified [%s#%s (%s), session: %s]", 
            client->session->user->username,
            client->session->user->discriminator,
            client->session->user->id,
            client->session->session_id
        );

        dc_dispatch_event(client, DISCORD_EVENT_CONNECTED, NULL);
    } else if(DISCORD_GATEWAY_EVENT_MESSAGE_CREATE == payload->t) {
        discord_message_t* msg = (discord_message_t*) payload->d;

        DISCORD_LOGD("New message (from %s#%s): %s",
            msg->author->username,
            msg->author->discriminator,
            msg->content
        );

        dc_dispatch_event(client, DISCORD_EVENT_MESSAGE_RECEIVED, msg);
    } else {
        DISCORD_LOGW("Ignored dispatch event");
    }

    return ESP_OK;
}

static esp_err_t gw_handle_buffered_data(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    discord_gateway_payload_t* payload;

    DC_LOCK(payload = discord_model_gateway_payload_deserialize(client->buffer, client->buffer_len));

    if(payload == NULL) {
        DISCORD_LOGE("Cannot deserialize payload");
        return ESP_FAIL;
    }

    if(payload->s != DISCORD_NULL_SEQUENCE_NUMBER) {
        client->last_sequence_number = payload->s;
    }

    DISCORD_LOGD("Received payload (op: %d)", payload->op);

    switch (payload->op) {
        case DISCORD_OP_HELLO:
            gw_heartbeat_start(client, (discord_gateway_hello_t*) payload->d);
            discord_model_gateway_payload_free(payload);
            payload = NULL;
            gw_identify(client);
            break;
        
        case DISCORD_OP_HEARTBEAT_ACK:
            DISCORD_LOGD("Heartbeat ack received");
            client->heartbeater.received_ack = true;
            break;

        case DISCORD_OP_DISPATCH:
            gw_dispatch(client, payload);
            break;
        
        default:
            DISCORD_LOGW("Unhandled payload (op: %d)", payload->op);
            break;
    }

    if(payload != NULL) {
        discord_model_gateway_payload_free(payload);
    }

    return ESP_OK;
}

static void dc_task(void* arg) {
    DISCORD_LOG_FOO();

    discord_client_handle_t client = (discord_client_handle_t) arg;

    while(client->running) {
        if (xSemaphoreTakeRecursive(client->lock, portMAX_DELAY) != pdPASS) {
            DISCORD_LOGE("Failed to lock discord tasks, exiting the task...");
            break;
        }

        switch(client->state) {
            case DISCORD_CLIENT_STATE_UNKNOWN:
                // state shouldn't be unknown in this task
                break;

            case DISCORD_CLIENT_STATE_INIT:
                // client trying to connect...
                break;

            case DISCORD_CLIENT_STATE_CONNECTING:
                // ws connected, but gateway not identified yet
                break;

            case DISCORD_CLIENT_STATE_CONNECTED:
                gw_heartbeat_send_if_expired(client);
                break;

            case DISCORD_CLIENT_STATE_DISCONNECTED:
                if(client->close_reason == DISCORD_CLOSE_REASON_NOT_REQUESTED) {
                    // This event will be invoked when token is invalid as well
                    // correct reason of closing the connection can be found in frame data
                    // (https://discord.com/developers/docs/topics/opcodes-and-status-codes#gateway-gateway-close-event-codes)
                    //
                    // In this moment websocket client does not emit data in this event
                    // (issue reported: https://github.com/espressif/esp-idf/issues/6535)

                    DISCORD_LOGE("Connection closed unexpectedly. Reason cannot be identified in this moment. Maybe your token is invalid?");
                    discord_logout(client);
                } else {
                    gw_reset(client);
                    client->state = DISCORD_CLIENT_STATE_INIT;
                }
                break;
            
            case DISCORD_CLIENT_STATE_ERROR:
                DISCORD_LOGE("Unhandled error occurred. Disconnecting...");
                discord_logout(client);
                break;
        }

        xSemaphoreGiveRecursive(client->lock);

        if(client->state >= DISCORD_CLIENT_STATE_CONNECTING) {
            EventBits_t bits = xEventGroupWaitBits(client->status_bits, DISCORD_CLIENT_STATUS_BIT_BUFFER_READY, pdTRUE, pdTRUE, 1000 / portTICK_PERIOD_MS); // poll every 1000ms

            if((DISCORD_CLIENT_STATUS_BIT_BUFFER_READY & bits) != 0) {
                gw_handle_buffered_data(client);
            }
        } else {
            vTaskDelay(125 / portTICK_PERIOD_MS);
        }
    }

    client->state = DISCORD_CLIENT_STATE_INIT;
    vTaskDelete(NULL);
}

discord_client_handle_t discord_create(const discord_client_config_t* config) {
    DISCORD_LOG_FOO();

    discord_client_handle_t client = calloc(1, sizeof(struct discord_client));
    
    client->state = DISCORD_CLIENT_STATE_UNKNOWN;

    client->lock = xSemaphoreCreateRecursiveMutex();
    client->status_bits = xEventGroupCreate();

    esp_event_loop_args_t event_args = {
        .queue_size = 1,
        .task_name = NULL // no task will be created
    };

    if (esp_event_loop_create(&event_args, &client->event_handle) != ESP_OK) {
        DISCORD_LOGE("Cannot create event handler for discord client");
        free(client);
        return NULL;
    }

    client->config = dc_config_copy(config);
    client->buffer = malloc(client->config->buffer_size);

    gw_init(client);
    
    return client;
}

esp_err_t discord_login(discord_client_handle_t client) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;
    
    DISCORD_LOG_FOO();

    if(client->state >= DISCORD_CLIENT_STATE_INIT) {
        DISCORD_LOGE("Client is above (or equal to) init state");
        return ESP_FAIL;
    }

    client->state = DISCORD_CLIENT_STATE_INIT;
    client->running = true;

    if (xTaskCreate(dc_task, "discord_task", DISCORD_TASK_STACK_SIZE, client, DISCORD_TASK_PRIORITY, &client->task_handle) != pdTRUE) {
        DISCORD_LOGE("Cannot create discord task");
        return ESP_FAIL;
    }

    return gw_open(client);
}

esp_err_t discord_register_events(discord_client_handle_t client, discord_event_id_t event, esp_event_handler_t event_handler, void* event_handler_arg) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;
    
    DISCORD_LOG_FOO();
    
    return esp_event_handler_register_with(client->event_handle, DISCORD_EVENTS, event, event_handler, event_handler_arg);
}

esp_err_t discord_logout(discord_client_handle_t client) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;

    DISCORD_LOG_FOO();

    client->running = false;

    gw_close(client, DISCORD_CLOSE_REASON_LOGOUT);

    esp_websocket_client_destroy(client->ws);
    client->ws = NULL;

    if(client->event_handle) {
        esp_event_loop_delete(client->event_handle);
        client->event_handle = NULL;
    }

    discord_model_gateway_session_free(client->session);
    client->session = NULL;

    client->state = DISCORD_CLIENT_STATE_UNKNOWN;

    return ESP_OK;
}

esp_err_t discord_destroy(discord_client_handle_t client) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;
    
    DISCORD_LOG_FOO();

    if(client->state >= DISCORD_CLIENT_STATE_INIT) {
        discord_logout(client);
    }

    vSemaphoreDelete(client->lock);
    if(client->status_bits) {
        vEventGroupDelete(client->status_bits);
    }
    free(client->buffer);
    dc_config_free(client->config);
    free(client);

    return ESP_OK;
}

static esp_err_t gw_heartbeat_send_if_expired(discord_client_handle_t client) {
    if(client->heartbeater.running && dc_tick_ms() - client->heartbeater.tick_ms > client->heartbeater.interval) {
        DISCORD_LOGD("Heartbeat");

        client->heartbeater.tick_ms = dc_tick_ms();

        if(! client->heartbeater.received_ack) {
            DISCORD_LOGW("ACK has not been received since the last heartbeat. Reconnection will follow using IDENTIFY (RESUME is not implemented yet)");
            return gw_reconnect(client);
        }

        client->heartbeater.received_ack = false;
        int s = client->last_sequence_number;

        return gw_send(client, discord_model_gateway_payload(
            DISCORD_OP_HEARTBEAT,
            (discord_gateway_heartbeat_t*) &s
        ));
    }

    return ESP_OK;
}

static esp_err_t gw_heartbeat_start(discord_client_handle_t client, discord_gateway_hello_t* hello) {
    if(client->heartbeater.running)
        return ESP_OK;
    
    DISCORD_LOG_FOO();
    
    // Set ack to true to prevent first ack checking
    client->heartbeater.received_ack = true;
    client->heartbeater.interval = hello->heartbeat_interval;
    client->heartbeater.tick_ms = dc_tick_ms();
    client->heartbeater.running = true;

    return ESP_OK;
}

static esp_err_t gw_heartbeat_stop(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    client->heartbeater.running = false;
    client->heartbeater.interval = 0;
    client->heartbeater.tick_ms = 0;
    client->heartbeater.received_ack = false;

    return ESP_OK;
}