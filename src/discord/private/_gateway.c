#include "discord/utils.h"
#include "discord/private/_gateway.h"
#include "discord/models/message.h"
#include "esp_transport_ws.h"

DISCORD_LOG_DEFINE_BASE();

static esp_err_t dcgw_heartbeat_stop(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    client->heartbeater.running = false;
    client->heartbeater.interval = 0;
    client->heartbeater.tick_ms = 0;
    client->heartbeater.received_ack = false;

    return ESP_OK;
}

static esp_err_t dcgw_reset(discord_client_handle_t client) {
    DISCORD_LOG_FOO();
    
    dcgw_heartbeat_stop(client);
    client->last_sequence_number = DISCORD_NULL_SEQUENCE_NUMBER;
    client->buffer_len = 0;

    return ESP_OK;
}

esp_err_t dcgw_init(discord_client_handle_t client) {
    DISCORD_LOG_FOO();
    
    dcgw_reset(client);
    client->state = DISCORD_CLIENT_STATE_UNKNOWN;
    client->close_reason = DISCORD_CLOSE_REASON_NOT_REQUESTED;
    client->close_code = DISCORD_CLOSEOP_NO_CODE;

    return ESP_OK;
}

/**
 * @brief Send payload (serialized to json) to gateway. Payload will be automatically freed
 */
esp_err_t dcgw_send(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    DISCORD_LOG_FOO();

    char* payload_raw = discord_model_gateway_payload_serialize(payload);

    DISCORD_LOGD("%s", payload_raw);

    int sent_bytes = esp_websocket_client_send_text(client->ws, payload_raw, strlen(payload_raw), 5000 / portTICK_PERIOD_MS); // 5sec timeout
    free(payload_raw);

    if(sent_bytes == ESP_FAIL) {
        DISCORD_LOGW("Fail to send data to gateway");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool dcgw_is_open(discord_client_handle_t client) {
    return client->running && client->state >= DISCORD_CLIENT_STATE_INIT;
}

static bool dcgw_whether_payload_should_go_into_queue(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    if(!payload)
        return false;

    if(payload->op == DISCORD_OP_DISPATCH) {
        if(client->state < DISCORD_CLIENT_STATE_CONNECTED && payload->t != DISCORD_GATEWAY_EVENT_READY) {
            DISCORD_LOGW("Ignoring payload because client is not in CONNECTED state and still not receive READY payload");
            return false;
        }

        switch(payload->t) {
            case DISCORD_GATEWAY_EVENT_MESSAGE_CREATE:
            case DISCORD_GATEWAY_EVENT_MESSAGE_UPDATE: {
                    discord_message_t* msg = (discord_message_t*) payload->d;

                    // ignore messages which are ones from us or does not contain any content
                    if(!msg || !msg->content || !msg->author || discord_streq(msg->author->id, client->session->user->id)) {
                        return false;
                    }
                }
                break;
                
            default:
                break;
        }
    }

    return true;
}

static esp_err_t dcgw_buffer_websocket_data(discord_client_handle_t client, esp_websocket_event_data_t* data) {
    DISCORD_LOG_FOO();

    if(data->payload_len > client->config->buffer_size) {
        DISCORD_LOGW("Payload too big. Wider buffer required.");
        return ESP_FAIL;
    }
    
    DISCORD_LOGD("Buffering received data:\n%.*s", data->data_len, data->data_ptr);
    
    memcpy(client->buffer + data->payload_offset, data->data_ptr, data->data_len);

    if((client->buffer_len = data->data_len + data->payload_offset) >= data->payload_len) {
        DISCORD_LOGD("Buffering done.");

        if(data->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
            client->state = DISCORD_CLIENT_STATE_DISCONNECTING;
        }
        
        // append null terminator
        client->buffer[client->buffer_len] = '\0';

        discord_gateway_payload_t* payload = discord_model_gateway_payload_deserialize(client->buffer, client->buffer_len);

        if(!payload) {
            DISCORD_LOGE("Fail to deserialize payload");
            return ESP_FAIL;
        }

        if(payload->s != DISCORD_NULL_SEQUENCE_NUMBER) {
            client->last_sequence_number = payload->s;
        }

        if(! dcgw_whether_payload_should_go_into_queue(client, payload)) {
            discord_model_gateway_payload_free(payload);
        } else if(xQueueSend(client->queue, &payload, 5000 / portTICK_PERIOD_MS) != pdPASS) { // 5sec timeout
            DISCORD_LOGW("Fail to queue the payload");
            discord_model_gateway_payload_free(payload);
        }
    }

    return ESP_OK;
}

static void dcgw_websocket_event_handler(void* handler_arg, esp_event_base_t base, int32_t event_id, void* event_data) {
    discord_client_handle_t client = (discord_client_handle_t) handler_arg;
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*) event_data;

    if(data->op_code == WS_TRANSPORT_OPCODES_PONG) { // ignore PONG frame
        return;
    }

    DISCORD_LOGD("ws event (event=%d, op_code=%d, payload_len=%d, data_len=%d, payload_offset=%d)",
        event_id,
        data->op_code,
        data->payload_len, 
        data->data_len, 
        data->payload_offset
    );

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            client->state = DISCORD_CLIENT_STATE_CONNECTING;
            break;

        case WEBSOCKET_EVENT_DATA:
            if(data->op_code == WS_TRANSPORT_OPCODES_TEXT || data->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
                dcgw_buffer_websocket_data(client, data);
            }
            break;
        
        case WEBSOCKET_EVENT_ERROR:
            client->state = DISCORD_CLIENT_STATE_ERROR;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            client->state = DISCORD_CLIENT_STATE_DISCONNECTED;
            break;

        case WEBSOCKET_EVENT_CLOSED:
            client->state = DISCORD_CLIENT_STATE_DISCONNECTED;
            break;
            
        default:
            DISCORD_LOGW("Unknown ws event %d", event_id);
            break;
    }
}

esp_err_t dcgw_open(discord_client_handle_t client) {
    if(client == NULL)
        return ESP_ERR_INVALID_ARG;
    
    DISCORD_LOG_FOO();

    if(dcgw_is_open(client)) {
        DISCORD_LOGD("Already open");
        return ESP_OK;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = "wss://gateway.discord.gg/?v=8&encoding=json",
        .buffer_size = DISCORD_GW_WS_BUFFER_SIZE
    };

    client->ws = esp_websocket_client_init(&ws_cfg);

    esp_websocket_register_events(client->ws, WEBSOCKET_EVENT_ANY, dcgw_websocket_event_handler, (void*) client);
    dcgw_start(client);

    return ESP_OK;
}

esp_err_t dcgw_start(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    if(dcgw_is_open(client)) {
        DISCORD_LOGD("Already started");
        return ESP_OK;
    }
    
    client->state = DISCORD_CLIENT_STATE_INIT;

    return esp_websocket_client_start(client->ws);
}

esp_err_t dcgw_close(discord_client_handle_t client, discord_close_reason_t reason) {
    DISCORD_LOG_FOO();

    if(! dcgw_is_open(client)) {
        DISCORD_LOGD("Already closed");
        return ESP_OK;
    }

    // do not set client status in this function
    // it will be automatically set in ws task
    
    client->close_reason = reason;

    if(esp_websocket_client_is_connected(client->ws)) {
        esp_websocket_client_close(client->ws, portMAX_DELAY);
    }

    dcgw_reset(client);

    return ESP_OK;
}

static discord_close_code_t dcgw_close_opcode(discord_client_handle_t client) {
    if(client->state == DISCORD_CLIENT_STATE_DISCONNECTING && client->buffer_len >= 2) {
        int code = (256 * client->buffer[0] + client->buffer[1]);
        return code >= _DISCORD_CLOSEOP_MIN && code <= _DISCORD_CLOSEOP_MAX ? code : DISCORD_CLOSEOP_NO_CODE;
    }

    return DISCORD_CLOSEOP_NO_CODE;
}

char* dcgw_close_desc(discord_client_handle_t client) {
    return client->close_code != DISCORD_CLOSEOP_NO_CODE ? client->buffer + 2 : NULL;
}

static esp_err_t dcgw_heartbeat_start(discord_client_handle_t client, discord_gateway_hello_t* hello) {
    if(client->heartbeater.running)
        return ESP_OK;
    
    DISCORD_LOG_FOO();
    
    // Set ack to true to prevent first ack checking
    client->heartbeater.received_ack = true;
    client->heartbeater.interval = hello->heartbeat_interval;
    client->heartbeater.tick_ms = discord_tick_ms();
    client->heartbeater.running = true;

    return ESP_OK;
}

esp_err_t dcgw_heartbeat_send_if_expired(discord_client_handle_t client) {
    if(client->heartbeater.running && discord_tick_ms() - client->heartbeater.tick_ms > client->heartbeater.interval) {
        DISCORD_LOGD("Heartbeat");

        client->heartbeater.tick_ms = discord_tick_ms();

        if(!client->heartbeater.received_ack) {
            DISCORD_LOGW("ACK has not been received since the last heartbeat. Reconnection will follow using IDENTIFY (RESUME is not implemented yet)");
            return dcgw_close(client, DISCORD_CLOSE_REASON_HEARTBEAT_ACK_NOT_RECEIVED);
        }

        client->heartbeater.received_ack = false;
        int s = client->last_sequence_number;

        return dcgw_send(client, discord_model_gateway_payload(
            DISCORD_OP_HEARTBEAT,
            (discord_gateway_heartbeat_t*) &s
        ));
    }

    return ESP_OK;
}

esp_err_t dcgw_identify(discord_client_handle_t client) {
    DISCORD_LOG_FOO();

    return dcgw_send(client, discord_model_gateway_payload(
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
static esp_err_t dcgw_dispatch(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    DISCORD_LOG_FOO();

    if(DISCORD_GATEWAY_EVENT_READY == payload->t) {
        if(client->session) {
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

        DISCORD_EVENT_EMIT(DISCORD_EVENT_CONNECTED, NULL);

        return ESP_OK;
    }

    // client is connected, we can handle events
    
    switch(payload->t) {
        case DISCORD_GATEWAY_EVENT_MESSAGE_CREATE:
            DISCORD_EVENT_EMIT(DISCORD_EVENT_MESSAGE_RECEIVED, payload->d);
            break;

        case DISCORD_GATEWAY_EVENT_MESSAGE_UPDATE:
            DISCORD_EVENT_EMIT(DISCORD_EVENT_MESSAGE_UPDATED, payload->d);
            break;
        
        case DISCORD_GATEWAY_EVENT_MESSAGE_DELETE:
            DISCORD_EVENT_EMIT(DISCORD_EVENT_MESSAGE_DELETED, payload->d);
            break;

        default:
            DISCORD_LOGW("Ignored dispatch event");
            break;
    }

    return ESP_OK;
}

esp_err_t dcgw_handle_payload(discord_client_handle_t client, discord_gateway_payload_t* payload) {
    DISCORD_LOG_FOO();

    if(!payload)
        return ESP_FAIL;

    if(client->state == DISCORD_CLIENT_STATE_DISCONNECTING) {
        discord_close_code_t close_code = dcgw_close_opcode(client);

        if(close_code == DISCORD_CLOSEOP_NO_CODE) {
            DISCORD_LOGE("Cannot read or invalid close op code");
            return ESP_OK;
        }

        DISCORD_LOGD("Closing with code %d", close_code);
        client->close_code = close_code;

        return ESP_OK;
    }

    DISCORD_LOGD("Received payload (op: %d)", payload->op);

    switch (payload->op) {
        case DISCORD_OP_HELLO:
            dcgw_heartbeat_start(client, (discord_gateway_hello_t*) payload->d);
            discord_model_gateway_payload_free(payload);
            payload = NULL;
            dcgw_identify(client);
            break;
        
        case DISCORD_OP_HEARTBEAT_ACK:
            DISCORD_LOGD("Heartbeat ack received");
            client->heartbeater.received_ack = true;
            break;

        case DISCORD_OP_DISPATCH:
            dcgw_dispatch(client, payload);
            break;
        
        default:
            DISCORD_LOGW("Unhandled payload (op: %d)", payload->op);
            break;
    }

    if(payload) {
        discord_model_gateway_payload_free(payload);
    }

    return ESP_OK;
}