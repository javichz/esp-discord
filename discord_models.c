#include "string.h"
#include "esp_system.h"
#include "cJSON.h"
#include "discord_models.h"

discord_user_t* discord_model_user(cJSON* root) {
    discord_user_t* user = calloc(1, sizeof(discord_user_t));

    user->id = strdup(cJSON_GetObjectItem(root, "id")->valuestring);
    cJSON* bot = cJSON_GetObjectItem(root, "bot");
    user->bot = bot && bot->valueint;

    return user;
}

void discord_model_user_free(discord_user_t* user) {
    free(user->id);
    free(user);
}

discord_gateway_identification_t* discord_model_gateway_identification(cJSON* root) {
    discord_gateway_identification_t* id = calloc(1, sizeof(discord_gateway_identification_t));

    id->session_id = strdup(cJSON_GetObjectItem(root, "session_id")->valuestring);
    id->user = discord_model_user(cJSON_GetObjectItem(root, "user"));

    return id;
}

void discord_model_gateway_identification_free(discord_gateway_identification_t* id) {
    discord_model_user_free(id->user);
    free(id->session_id);
    free(id);
}