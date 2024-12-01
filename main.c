#include <assert.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define cleanup_return(x) { ret = (x); goto cleanup; }

#define discord_api_url "https://discord.com/api/v10"
#define discord_get_gateway_url (discord_api_url"/gateway")
#define gateway_query_params "/?v=10&encoding=json"

// https://discord.com/developers/docs/events/gateway#gateway-intents
#define gateway_intents (1 << 9) /* GUILD_MESSAGES */

enum gateway_events
{
    GATEWAY_DISPATCH = 0,
    GATEWAY_HEARTBEAT = 1,
    GATEWAY_IDENTIFY = 2,
    GATEWAY_PRESENCE_UPDATE = 3,
    GATEWAY_VOICE_STATE_UPDATE = 4,
    GATEWAY_RESUME = 6,
    GATEWAY_RECONNECT = 7,
    GATEWAY_REQUEST_GUILD_MEMBERS = 8,
    GATEWAY_INVALID_SESSION = 9,
    GATEWAY_HELLO = 10,
    GATEWAY_HEARTBEAT_ACK = 11,
    GATEWAY_REQUEST_SOUNDBOARD_SOUNDS = 31,
};

enum gateway_interaction_types
{
    GATEWAY_INTERACTION_PING = 1,
    GATEWAY_INTERACTION_APPLICATION_COMMAND = 2,
    GATEWAY_INTERACTION_MESSAGE_COMPONENT = 3,
    GATEWAY_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE = 4,
    GATEWAY_INTERACTION_MODAL_SUBMIT = 5,
};

enum gateway_interaction_callback_types
{
    GATEWAY_INTERACTION_CALLBACK_PONG = 1,
    GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE = 4,
    GATEWAY_INTERACTION_CALLBACK_DEFERRED_CHANNEL_MESSAGE_WITH_SOURCE = 5,
    GATEWAY_INTERACTION_CALLBACK_DEFERRED_UPDATE_MESSAGE = 6,
    GATEWAY_INTERACTION_CALLBACK_UPDATE_MESSAGE = 7,
    GATEWAY_INTERACTION_CALLBACK_APPLICATION_COMMAND_AUTOCOMPLETE_RESULT = 8,
    GATEWAY_INTERACTION_CALLBACK_MODAL = 9,
    GATEWAY_INTERACTION_CALLBACK_LAUNCH_ACTIVITY = 12,
};

enum gateway_app_command_types
{
    GATEWAY_APP_CMD_CHAT_INPUT = 1,
    GATEWAY_APP_CMD_USER = 2,
    GATEWAY_APP_CMD_MESSAGE = 3,
    GATEWAY_APP_CMD_PRIMARY_ENTRY_POINT = 4,
};

enum gateway_app_command_option_types
{
    GATEWAY_APP_CMD_OPT_SUB_COMMAND = 1,
    GATEWAY_APP_CMD_OPT_SUB_COMMAND_GROUP = 2,
    GATEWAY_APP_CMD_OPT_STRING = 3,
    GATEWAY_APP_CMD_OPT_INTEGER = 4,
    GATEWAY_APP_CMD_OPT_BOOLEAN = 5,
    GATEWAY_APP_CMD_OPT_USER = 6,
    GATEWAY_APP_CMD_OPT_CHANNEL = 7,
    GATEWAY_APP_CMD_OPT_ROLE = 8,
    GATEWAY_APP_CMD_OPT_MENTIONABLE = 9,
    GATEWAY_APP_CMD_OPT_NUMBER = 10,
    GATEWAY_APP_CMD_OPT_ATTACHMENT = 11,
};

struct buffer_write_data
{
    char* buffer;
    size_t size;
    size_t offset;
};

size_t buffer_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    struct buffer_write_data* data = userdata;
    size_t count = size * nmemb;
    if (count > data->size - data->offset)
    {
        size_t required = data->offset + count;
        data->size = 2 * data->size > required ? 2 * data->size : required;
        data->buffer = realloc(data->buffer, data->size);
    }

    memcpy(data->buffer + data->offset, ptr, count);
    data->offset += count;
    return count;
}

char* get_gateway_url()
{
    char* ret = NULL;
    CURL* c = NULL;
    cJSON* response_json = NULL;
    struct buffer_write_data buffer_write_data = { 0 };

    c = curl_easy_init();
    if (!c)
    {
        fprintf(stderr, "curl_easy_init failed\n");
        cleanup_return(NULL);
    }

    curl_easy_setopt(c, CURLOPT_URL, discord_get_gateway_url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer_write_data);

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        cleanup_return(NULL);
    }

    response_json = cJSON_ParseWithLength(buffer_write_data.buffer, buffer_write_data.offset);
    cJSON* gateway_url_json = cJSON_GetObjectItemCaseSensitive(response_json, "url");
    char* gateway_url = cJSON_GetStringValue(gateway_url_json);
    if (!gateway_url)
    {
        fprintf(stderr, "failed to parse gateway url query results\n");
        cleanup_return(NULL);
    }

    int url_length = strlen(gateway_url);
    ret = malloc(url_length + sizeof(gateway_query_params));
    memcpy(ret, gateway_url, url_length);
    memcpy(ret + url_length, gateway_query_params, sizeof(gateway_query_params));

cleanup:
    free(buffer_write_data.buffer);
    cJSON_Delete(response_json);
    curl_easy_cleanup(c);
    return ret;
}

struct gateway_websocket_data
{
    CURL* c;
    int heartbeat_interval;
    int heartbeat_timer;
    size_t bufsize;
    size_t offset;
    char* buffer;
    uint32_t sequence;
    const char* token;
    const char* app_id;
    char* resume_gateway_url;
    char* session_id;
};

cJSON* create_gateway_payload_json(int opcode)
{
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "op", opcode);
    return json;
}

int send_gateway_payload_json(struct gateway_websocket_data* data, const cJSON* json)
{
    int ret = 0;
    char* json_string = NULL;
    if (json)
    {
        json_string = cJSON_PrintUnformatted(json);
    }
    if (!json_string)
    {
        fprintf(stderr, "failed to serialize payload json\n");
        cleanup_return(1);
    }

    size_t size = strlen(json_string);
    size_t rsize;
    CURLcode code = curl_ws_send(data->c, json_string, size, &rsize, 0, CURLWS_TEXT);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_ws_send failed: %s\n", curl_easy_strerror(code));
        cleanup_return(1);
    }

cleanup:
    free(json_string);
    return ret;
}

int send_heartbeat(struct gateway_websocket_data* data)
{
    int ret = 0;
    cJSON* payload_json = create_gateway_payload_json(GATEWAY_HEARTBEAT);
    cJSON_AddNumberToObject(payload_json, "d", data->sequence);

    if (send_gateway_payload_json(data, payload_json))
    {
        fprintf(stderr, "failed to send heartbeat payload\n");
        cleanup_return(1);
    }

cleanup:
    cJSON_Delete(payload_json);

    return ret;
}

int send_identify(struct gateway_websocket_data* data)
{
    int ret = 0;
    cJSON* payload_json = create_gateway_payload_json(GATEWAY_IDENTIFY);
    cJSON* data_json = cJSON_AddObjectToObject(payload_json, "d");
    cJSON_AddStringToObject(data_json, "token", data->token);
    cJSON_AddNumberToObject(data_json, "intents", gateway_intents);
    cJSON* properties_json = cJSON_AddObjectToObject(data_json, "properties");
    cJSON_AddStringToObject(properties_json, "os", "linux");
    cJSON_AddStringToObject(properties_json, "browser", "mcserver-discord");
    cJSON_AddStringToObject(properties_json, "device", "mcserver-discord");

    if (send_gateway_payload_json(data, payload_json))
    {
        fprintf(stderr, "failed to send identify payload\n");
        cleanup_return(1);
    }

cleanup:
    cJSON_Delete(payload_json);

    return ret;
}

int handle_hello(struct gateway_websocket_data* data, const cJSON* event_data_json)
{
    int ret = 0;

    cJSON* heartbeat_interval_json = cJSON_GetObjectItemCaseSensitive(event_data_json, "heartbeat_interval");
    if (!cJSON_IsNumber(heartbeat_interval_json))
    {
        fprintf(stderr, "failed to parse heartbeat interval\n");
        cleanup_return(1);
    }

    data->heartbeat_interval = heartbeat_interval_json->valueint;
    uint64_t first_interval = (uint64_t)data->heartbeat_interval * rand() / RAND_MAX;
    printf("setting heartbeat_timer: %d, initial: %lu\n", data->heartbeat_interval, first_interval);
    if (timerfd_settime(data->heartbeat_timer, 0, &(struct itimerspec) {
                .it_value.tv_sec = first_interval / 1000,
                .it_value.tv_nsec = (first_interval % 1000) * 1000000,
            }, NULL) != 0)
    {
        perror("timerfd_settime failed");
        cleanup_return(1);
    }

    if (send_identify(data))
    {
        fprintf(stderr, "failed to send identify\n");
        cleanup_return(1);
    }

cleanup:
    return ret;
}

int handle_ready(struct gateway_websocket_data* data, const cJSON* event_data_json)
{
    if (!(data->session_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "session_id"))))
    {
        fprintf(stderr, "failed to parse session_id\n");
        return 1;
    }
    if (!(data->resume_gateway_url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "resume_gateway_url"))))
    {
        fprintf(stderr, "failed to parse resume_gateway_url\n");
        return 1;
    }

    printf("got session_id: %s, resume_gateway_url: %s\n", data->session_id, data->resume_gateway_url);

    return 0;
}

int send_interaction_response(const cJSON* response_json, const char* interaction_id, const char* interaction_token)
{
    int ret = 0;
    char* json_string = NULL;
    char* endpoint_url = NULL;
    CURL* c = NULL;
    struct curl_slist* headers = NULL;
    struct buffer_write_data buffer_write_data;

    if (response_json)
    {
        json_string = cJSON_PrintUnformatted(response_json);
    }
    if (!json_string)
    {
        fprintf(stderr, "failed to serialize interaction response json\n");
        cleanup_return(1);
    }

    const char* fmt = discord_api_url"/interactions/%s/%s/callback";
    int sz = snprintf(NULL, 0, fmt, interaction_id, interaction_token);
    if (sz < 0)
    {
        fprintf(stderr, "failed to format interaction callback endpoint url\n");
        cleanup_return(1);
    }
    endpoint_url = malloc(sz + 1);
    if (snprintf(endpoint_url, sz + 1, fmt, interaction_id, interaction_token) != sz)
    {
        fprintf(stderr, "failed to format interaction callback endpoint url\n");
        cleanup_return(1);
    }

    if (!(c = curl_easy_init()))
    {
        fprintf(stderr, "curl_easy_init failed\n");
        cleanup_return(1);
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, endpoint_url);
    curl_easy_setopt(c, CURLOPT_POST, 1);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_string);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer_write_data);

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        cleanup_return(1);
    }

    printf("interaction callback response: %.*s\n", (int)buffer_write_data.offset, buffer_write_data.buffer);

cleanup:
    free(buffer_write_data.buffer);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    free(endpoint_url);
    free(json_string);
    return ret;
}

int handle_interaction_create(struct gateway_websocket_data* data, const cJSON* event_data_json)
{
    int ret = 0;
    cJSON* response_json = NULL;

    cJSON* type_json = cJSON_GetObjectItemCaseSensitive(event_data_json, "type");
    if (!cJSON_IsNumber(type_json))
    {
        fprintf(stderr, "failed to parse interaction type\n");
        cleanup_return(1);
    }
    int type = type_json->valueint;

    const char* interaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "id"));
    if (!interaction_id)
    {
        fprintf(stderr, "failed to parse interaction id\n");
        cleanup_return(1);
    }

    const char* interaction_token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "token"));
    if (!interaction_token)
    {
        fprintf(stderr, "failed to parse interaction token\n");
        cleanup_return(1);
    }

    if (type == GATEWAY_INTERACTION_APPLICATION_COMMAND)
    {
        cJSON* app_cmd_json = cJSON_GetObjectItemCaseSensitive(event_data_json, "data");

        response_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(response_json, "type", GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE);
        cJSON* response_message_json = cJSON_AddObjectToObject(response_json, "data");
        cJSON_AddStringToObject(response_message_json, "content", "hello world");
    }

    if (response_json)
    {
        if (send_interaction_response(response_json, interaction_id, interaction_token))
        {
            fprintf(stderr, "failed to send interaction response\n");
            cleanup_return(1);
        }
    }

cleanup:
    cJSON_Delete(response_json);
    return ret;
}

int handle_gateway_payload(struct gateway_websocket_data* data)
{
    int ret = 0;
    cJSON* response_json = NULL;
    char* response_json_string = NULL;
    cJSON* gateway_payload_json = cJSON_ParseWithLength(data->buffer, data->offset);
    if (!gateway_payload_json)
    {
        fprintf(stderr, "failed to parse gateway event payload \"%.*s\", error at position %ld\n", (int)data->offset, data->buffer, cJSON_GetErrorPtr() - data->buffer);
        cleanup_return(1);
    }

    cJSON* op_code_json = cJSON_GetObjectItemCaseSensitive(gateway_payload_json, "op");
    if (!cJSON_IsNumber(op_code_json))
    {
        fprintf(stderr, "failed to parse gateway event op code\n");
        cleanup_return(1);
    }

    cJSON* event_data_json = cJSON_GetObjectItemCaseSensitive(gateway_payload_json, "d");

    int op = op_code_json->valueint;
    if (op == GATEWAY_DISPATCH)
    {
        cJSON* sequence_json = cJSON_GetObjectItemCaseSensitive(gateway_payload_json, "s");
        if (!cJSON_IsNumber(sequence_json))
        {
            fprintf(stderr, "failed to parse dispatch sequence number\n");
            cleanup_return(1);
        }
        data->sequence = sequence_json->valueint;

        const char* event_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(gateway_payload_json, "t"));
        if (!event_name)
        {
            fprintf(stderr, "failed to parse dispatch event name\n");
            cleanup_return(1);
        }

        int ret = 0;
        if (strcmp(event_name, "READY") == 0)
        {
            ret = handle_ready(data, event_data_json);
        }
        else if (strcmp(event_name, "INTERACTION_CREATE") == 0)
        {
            ret = handle_interaction_create(data, event_data_json);
        }
        else
        {
            fprintf(stderr, "unhandled dispatch event: %s\n", event_name);
        }

        if (ret)
        {
            fprintf(stderr, "failed to handle event: %s\n", event_name);
            cleanup_return(1);
        }
    }
    else if (op == GATEWAY_HEARTBEAT)
    {
        if (send_heartbeat(data))
        {
            fprintf(stderr, "failed to send heartbeat\n");
            cleanup_return(1);
        }
    }
    else if (op == GATEWAY_HELLO)
    {
        if (handle_hello(data, event_data_json))
        {
            fprintf(stderr, "failed to handle hello event\n");
            cleanup_return(1);
        }
    }
    else if (op == GATEWAY_HEARTBEAT_ACK)
    {
        // TODO: handle as per https://discord.com/developers/docs/events/gateway#sending-heartbeats
    }
    else
    {
        fprintf(stderr, "unhandled opcode: %d\n", op);
    }

cleanup:
    free(response_json_string);
    cJSON_Delete(response_json);
    cJSON_Delete(gateway_payload_json);
    return ret;
}

int gateway_websocket_receive(struct gateway_websocket_data* data)
{
    size_t rlen;
    const struct curl_ws_frame* frame;

    while (1)
    {
        CURLcode r = curl_ws_recv(data->c, data->buffer + data->offset, data->bufsize - data->offset, &rlen, &frame);
        printf("curl_ws_recv returned: %d %s\n", r, r == CURLE_OK ? "CURLE_OK" : r == CURLE_GOT_NOTHING ? "CURLE_GOT_NOTHING" : r == CURLE_AGAIN ? "CURLE_AGAIN" : "unknown");
        if (r == CURLE_AGAIN)
        {
            break;
        }
        if (r)
        {
            fprintf(stderr, "curl_ws_recv failed: %s\n", curl_easy_strerror(r));
            return 1;
        }

        printf("frame: 0x%X, %lu, %lu, %lu\n", frame->flags, frame->offset, frame->bytesleft, frame->len);
        data->offset += rlen;

        if (frame->bytesleft > data->bufsize - data->offset)
        {
            data->bufsize = data->offset + frame->bytesleft;
            data->buffer = realloc(data->buffer, data->bufsize);
        }

        if (!frame->bytesleft)
        {
            printf("read buffer: %.*s\n", (int)data->offset, data->buffer);
            if(data->offset <= 8)
            {
                unsigned long v = 0;
                for (size_t i = 0; i < data->offset; ++i)
                {
                    v = v << 8;
                    v = v | ((unsigned char*)data->buffer)[i];
                }
                printf("as integer: 0x%lX / %lu\n", v, v);
            }

            if (handle_gateway_payload(data))
            {
                fprintf(stderr, "failed to handle gateway payload\n");
                return 1;
            }

            data->offset = 0;
        }
    }

    return 0;
}

CURL* connect_to_gateway(const char* url)
{
    CURL* c = curl_easy_init();
    if (!c)
    {
        fprintf(stderr, "curl_easy_init failed\n");
        return NULL;
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2);
    // curl_easy_setopt(c, CURLOPT_VERBOSE, 1);

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        curl_easy_cleanup(c);
        return NULL;
    }

    return c;
}

int create_app_command(const char* app_id, const char* token, const cJSON* command_json)
{
    int ret = 0;
    char* endpoint_url = NULL;
    CURL* c = NULL;
    struct curl_slist* headers = NULL;
    char* bot_header = NULL;
    char* json_string = NULL;
    struct buffer_write_data buffer_write_data = { 0 };

    const char* fmt = discord_api_url"/applications/%s/commands";
    int sz = snprintf(NULL, 0, fmt, app_id);
    if (sz < 0)
    {
        fprintf(stderr, "failed to format app commands endpoint\n");
        cleanup_return(1);
    }
    endpoint_url = malloc(sz + 1);
    if (snprintf(endpoint_url, sz + 1, fmt, app_id) != sz)
    {
        fprintf(stderr, "failed to format app commands endpoint\n");
        cleanup_return(1);
    }

    c = curl_easy_init();
    if (!c)
    {
        fprintf(stderr, "curl_easy_init failed\n");
        cleanup_return(1);
    }

    const char* bot_header_fmt = "Authorization: Bot %s";
    sz = snprintf(NULL, 0, bot_header_fmt, token);
    if (sz < 0)
    {
        fprintf(stderr, "failed to format header\n");
        cleanup_return(1);
    }
    bot_header = malloc(sz + 1);
    if (snprintf(bot_header, sz + 1, bot_header_fmt, token) != sz)
    {
        fprintf(stderr, "failed to format header\n");
        cleanup_return(1);
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, bot_header);

    if (command_json)
    {
        json_string = cJSON_PrintUnformatted(command_json);
    }
    if (!json_string)
    {
        fprintf(stderr, "failed to serialize app command json\n");
        cleanup_return(1);
    }

    curl_easy_setopt(c, CURLOPT_URL, endpoint_url);
    curl_easy_setopt(c, CURLOPT_POST, 1);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_string);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer_write_data);

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        cleanup_return(1);
    }

    printf("got response to add app command: %.*s\n", (int)buffer_write_data.offset, buffer_write_data.buffer);

cleanup:
    free(buffer_write_data.buffer);
    free(json_string);
    free(bot_header);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    free(endpoint_url);
    return ret;
}

int create_app_commands(struct gateway_websocket_data* data)
{
    int ret = 0;
    cJSON* command_json = cJSON_CreateObject();
    cJSON_AddStringToObject(command_json, "name", "testcmd");
    cJSON_AddNumberToObject(command_json, "type", GATEWAY_APP_CMD_CHAT_INPUT);
    cJSON_AddStringToObject(command_json, "description", "testcmd desc");

    if (create_app_command(data->app_id, data->token, command_json))
    {
        fprintf(stderr, "failed to create testcmd\n");
        cleanup_return(1);
    }

cleanup:
    cJSON_Delete(command_json);
    return ret;
}

int run_websocket_client(CURL* c)
{
    int ret = 0;
    int efd = -1;
    int heartbeat_timer = -1;
    struct gateway_websocket_data gateway_websocket_data = { .c = c };

    if (!(gateway_websocket_data.token = getenv("TOKEN")))
    {
        fprintf(stderr, "TOKEN environment variable not set\n");
        cleanup_return(1);
    }

    if (!(gateway_websocket_data.app_id = getenv("APP_ID")))
    {
        fprintf(stderr, "APP_ID environment variable not set\n");
        cleanup_return(1);
    }

    if (create_app_commands(&gateway_websocket_data))
    {
        fprintf(stderr, "failed to create app commands\n");
        cleanup_return(1);
    }

    curl_socket_t ws_socket;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &ws_socket) != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_getinfo failed\n");
        cleanup_return(1);
    }

    efd = epoll_create1(0);
    if (efd == -1)
    {
        perror("epoll_create1 failed");
        cleanup_return(1);
    }

    if (epoll_ctl(efd, EPOLL_CTL_ADD, ws_socket, &(struct epoll_event) {
                .events = EPOLLIN,
                .data.fd = ws_socket,
            }) != 0)
    {
        perror("epoll_ctl failed");
        cleanup_return(1);
    }

    heartbeat_timer = timerfd_create(CLOCK_REALTIME, 0);
    if (heartbeat_timer == -1)
    {
        perror("timerfd_create failed");
        cleanup_return(1);
    }

    if (epoll_ctl(efd, EPOLL_CTL_ADD, heartbeat_timer, &(struct epoll_event){ .events = EPOLLIN, .data.fd = heartbeat_timer }) != 0)
    {
        perror("epoll_ctl failed");
        cleanup_return(1);
    }
    gateway_websocket_data.heartbeat_timer = heartbeat_timer;

    while (1)
    {
        struct epoll_event event;
        int epr = epoll_wait(efd, &event, 1, -1);
        printf("epr: %d \n", epr);

        if (epr < 0)
        {
            perror("epoll_wait failed");
            cleanup_return(1);
        }

        if (epr)
        {
            if (event.data.fd == heartbeat_timer)
            {
                if (event.events & EPOLLIN)
                {
                    uint64_t timeouts;
                    int nread = read(heartbeat_timer, &timeouts, sizeof(timeouts));
                    if (nread != sizeof(uint64_t))
                    {
                        perror("read failed");
                        cleanup_return(1);
                    }
                    printf("heartbeat_timer: %lu\n", timeouts);

                    if (send_heartbeat(&gateway_websocket_data))
                    {
                        fprintf(stderr, "failed to send heartbeat\n");
                        cleanup_return(1);
                    }

                    if (timerfd_settime(heartbeat_timer, 0, &(struct itimerspec) {
                                .it_value.tv_sec = gateway_websocket_data.heartbeat_interval / 1000,
                                .it_value.tv_nsec = (gateway_websocket_data.heartbeat_interval % 1000) * 1000000,
                            }, NULL) != 0)
                    {
                        perror("timerfd_settime failed");
                        cleanup_return(1);
                    }
                }
            }
            else if (event.data.fd == ws_socket)
            {
                if (event.events & EPOLLIN)
                {
                    if (gateway_websocket_receive(&gateway_websocket_data))
                    {
                        fprintf(stderr, "websocket receive failed\n");
                        cleanup_return(1);
                    }
                }
            }
        }
    }

cleanup:
    free(gateway_websocket_data.buffer);
    if (heartbeat_timer > -1)
    {
        close(heartbeat_timer);
    }
    if (efd > -1)
    {
        close(efd);
    }
    return ret;
}

int main(int argc, char** argv)
{
    int ret = 0;
    char* gateway_url = NULL;
    CURL* c = NULL;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        fprintf(stderr, "curl_global_init failed\n");
        cleanup_return(1);
    }

    if (!(gateway_url = get_gateway_url()))
    {
        fprintf(stderr, "failed to get gateway url\n");
        cleanup_return(1);
    }

    c = connect_to_gateway(gateway_url);
    if (!c)
    {
        fprintf(stderr, "failed to connect to gateway\n");
        cleanup_return(1);
    }

    printf("connected to gateway\n");

    if (run_websocket_client(c))
    {
        fprintf(stderr, "error running websocket client\n");
        cleanup_return(1);
    }

    printf("exiting\n");

cleanup:
    curl_easy_cleanup(c);
    free(gateway_url);
    curl_global_cleanup();

    return ret;
}
