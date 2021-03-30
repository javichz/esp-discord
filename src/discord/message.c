#include "discord/message.h"
#include "discord/private/_discord.h"
#include "discord/private/_api.h"
#include "discord/private/_json.h"
#include "cutils.h"
#include "estr.h"

DISCORD_LOG_DEFINE_BASE();

esp_err_t discord_message_send(discord_handle_t client, discord_message_t* message, discord_message_t** out_result) {
    if(! client || ! message || ! message->content || ! message->channel_id) {
        DISCORD_LOGE("Invalid args");
        return ESP_ERR_INVALID_ARG;
    }

    discord_api_response_t* res = NULL;
    
    esp_err_t err = dcapi_post(
        client,
        estr_cat("/channels/", message->channel_id, "/messages"),
        discord_json_serialize(message),
        out_result != NULL,
        &res
    );

    if(err != ESP_OK) {
        return err;
    }

    if(! dcapi_response_is_success(res)) {
        dcapi_response_free(client, res);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if(out_result) {
        if(res->data_len <= 0) {
            DISCORD_LOGW("Message sent but cannot return");
        } else {
            *out_result = discord_json_deserialize_(message, res->data, res->data_len);
        }
    }
    
    dcapi_response_free(client, res);
    return ESP_OK;
}

esp_err_t discord_message_react(discord_handle_t client, discord_message_t* message, const char* emoji) {
    if(!client || !message || !message->id || !message->channel_id) {
        DISCORD_LOGE("Invalid args");
        return ESP_FAIL;
    }

    char* _emoji = estr_url_encode(emoji);
    esp_err_t err = dcapi_put(client, estr_cat("/channels/", message->channel_id, "/messages/", message->id, "/reactions/", _emoji, "/@me"), NULL, false, NULL);
    free(_emoji);

    return err;
}

esp_err_t discord_message_download_attachment(discord_handle_t client, discord_message_t* message, uint8_t attachment_index, discord_download_handler_t download_handler, void* arg) {
    if(!client || !message || !message->attachments) {
        DISCORD_LOGE("Invalid args");
        return ESP_ERR_INVALID_ARG;
    }

    if(message->_attachments_len <= attachment_index) {
        DISCORD_LOGE("Message does not contain attachment with index %d", attachment_index);
        return ESP_ERR_INVALID_ARG;
    }

    discord_attachment_t* attach = message->attachments[attachment_index];

    discord_api_response_t* res = NULL;
    esp_err_t err = dcapi_download(client, attach->url, download_handler, &res, arg);
    if(err != ESP_OK) { return err; }
    err = dcapi_response_to_esp_err(res);
    dcapi_response_free(client, res);

    return err;
}

void discord_message_free(discord_message_t* message) {
    if(!message)
        return;
    
    free(message->id);
    free(message->content);
    free(message->channel_id);
    discord_user_free(message->author);
    free(message->guild_id);
    discord_member_free(message->member);
    cu_list_freex(message->attachments, message->_attachments_len, discord_attachment_free);
    free(message);
}