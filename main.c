#include <assert.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define cleanup_return(x) { ret = (x); goto cleanup; }

#define discord_api_url "https://discord.com/api/v10"
#define discord_get_gateway_url (discord_api_url"/gateway")
#define gateway_query_params "/?v=10&encoding=json"

#define add_whitelist_app_command "addme"

#define rcon_login_id 777

#define whitelist_request_timeout 10000

#define default_rcon_host "localhost"
#define default_rcon_port "25575"

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

enum rcon_packet_type
{
    RCON_PACKET_COMMAND_RESPONSE = 0,
    RCON_PACKET_COMMAND = 2,
    RCON_PACKET_LOGIN = 3,
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
    int rcon_socket;
    int ws_socket;
    int epoll_fd;
    int heartbeat_interval;
    int heartbeat_timer;
    size_t bufsize;
    size_t offset;
    char* buffer;
    uint32_t sequence;
    const char* token;
    const char* app_id;
    const char* gateway_url;
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
    cJSON_AddNumberToObject(data_json, "intents", 0);
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

int send_resume(struct gateway_websocket_data* data)
{
    int ret = 0;
    cJSON* payload_json = create_gateway_payload_json(GATEWAY_RESUME);
    cJSON* data_json = cJSON_AddObjectToObject(payload_json, "d");
    cJSON_AddStringToObject(data_json, "token", data->token);
    cJSON_AddStringToObject(data_json, "session_id", data->session_id);
    cJSON_AddNumberToObject(data_json, "seq", data->sequence);

    if (send_gateway_payload_json(data, payload_json))
    {
        fprintf(stderr, "failed to send resume payload\n");
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
    srand48(time(0));
    uint64_t first_interval = (uint64_t)data->heartbeat_interval * lrand48() / INT32_MAX;
    printf("setting heartbeat_timer: %d, initial: %lu\n", data->heartbeat_interval, first_interval);
    if (timerfd_settime(data->heartbeat_timer, 0, &(struct itimerspec) {
                .it_value.tv_sec = first_interval / 1000,
                .it_value.tv_nsec = (first_interval % 1000) * 1000000,
            }, NULL) != 0)
    {
        perror("timerfd_settime failed");
        cleanup_return(1);
    }

    if (data->session_id)
    {
        if (send_resume(data))
        {
            fprintf(stderr, "failed to send resume\n");
            cleanup_return(1);
        }
    }
    else
    {
        if (send_identify(data))
        {
            fprintf(stderr, "failed to send identify\n");
            cleanup_return(1);
        }
    }

    printf("gateway server acknowledged connection\n");

cleanup:
    return ret;
}

int handle_ready(struct gateway_websocket_data* data, const cJSON* event_data_json)
{
    const char* session_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "session_id"));
    const char* resume_gateway_url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event_data_json, "resume_gateway_url"));
    if (!session_id)
    {
        fprintf(stderr, "failed to parse session_id\n");
        return 1;
    }
    if (!resume_gateway_url)
    {
        fprintf(stderr, "failed to parse resume_gateway_url\n");
        return 1;
    }

    free(data->session_id);
    free(data->resume_gateway_url);

    data->session_id = malloc(strlen(session_id) + 1);
    strcpy(data->session_id, session_id);

    int url_length = strlen(resume_gateway_url);
    data->resume_gateway_url = malloc(url_length + sizeof(gateway_query_params));
    memcpy(data->resume_gateway_url, resume_gateway_url, url_length);
    memcpy(data->resume_gateway_url + url_length, gateway_query_params, sizeof(gateway_query_params));

    return 0;
}

int rcon_connect(const char* host, const char* port, const char* password)
{
    int ret = -1;
    struct addrinfo* addrinfo = NULL;

    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };

    int gai_code = getaddrinfo(host, port, &hints, &addrinfo);
    if (gai_code != 0)
    {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(gai_code));
        cleanup_return(-1);
    }

    for (struct addrinfo* ai = addrinfo; ai; ai = ai->ai_next)
    {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
        {
            perror("socket failed");
            continue;
        }

        int cc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (cc < 0)
        {
            perror("connect failed");
            close(s);
            continue;
        }

        cleanup_return(s);
    }

cleanup:
    if (addrinfo)
    {
        freeaddrinfo(addrinfo);
    }
    return ret;
}

int rcon_send(int rcon_socket, int32_t msg_id, int32_t msg_type, const char* body, int body_len)
{
    int ret = 0;
    char* buffer = NULL;

    int32_t size = 2 * sizeof(int32_t) + 2 + body_len;
    if (size > 4096)
    {
        fprintf(stderr, "rcon packet too large\n");
        cleanup_return(1);
    }

    buffer = malloc(sizeof(int32_t) + size);
    memcpy(buffer, &size, sizeof(int32_t));
    memcpy(buffer + sizeof(int32_t), &msg_id, sizeof(int32_t));
    memcpy(buffer + 2 * sizeof(int32_t), &msg_type, sizeof(int32_t));
    memcpy(buffer + 3 * sizeof(int32_t), body, body_len);
    buffer[body_len + 3 * sizeof(int32_t)] = 0;
    buffer[body_len + 3 * sizeof(int32_t) + 1] = 0;

    size_t offset  = 0;
    size_t remaining = sizeof(int32_t) + size;
    do
    {
        ssize_t ns = write(rcon_socket, buffer + offset, remaining);
        if (ns < 0)
        {
            perror("failed to write data");
            cleanup_return(1);
        }
        offset += ns;
        remaining -= ns;
    } while (remaining > 0);

cleanup:
    free(buffer);
    return ret;
}

typedef int (*rcon_callback)(int msg_id, int msg_type, const char* data, int data_len, void* userdata);

int handle_receive_rcon(int rcon_socket, rcon_callback callback, void* userdata)
{
    int ret = 0;
    char* buffer = NULL;

    int read_size;
    if (ioctl(rcon_socket, FIONREAD, &read_size) < 0)
    {
        perror("ioctl failed");
        cleanup_return(1);
    }

    if (read_size < 3 * sizeof(int32_t) + 2)
    {
        fprintf(stderr, "rcon packet too small\n");
        cleanup_return(1);
    }
    if (read_size > 4096 + sizeof(int32_t))
    {
        fprintf(stderr, "rcon packet too large\n");
        cleanup_return(1);
    }

    buffer = malloc(read_size);
    size_t offset = 0;

    do
    {
        ssize_t nr = read(rcon_socket, buffer + offset, read_size - offset);
        if (nr < 0)
        {
            perror("failed to read socket");
            cleanup_return(1);
        }
        offset += nr;
    }
    while (offset < read_size);

    int32_t size, msg_id, msg_type;
    memcpy(&size, buffer, sizeof(int32_t));
    memcpy(&msg_id, buffer + sizeof(int32_t), sizeof(int32_t));
    memcpy(&msg_type, buffer + 2 * sizeof(int32_t), sizeof(int32_t));
    if (size > read_size - sizeof(int32_t))
    {
        fprintf(stderr, "bad rcon packet: size mismatch\n");
        cleanup_return(1);
    }

    int data_len = size - 2 * sizeof(int32_t) - 2;
    if (data_len < 0)
    {
        fprintf(stderr, "bad rcon packet: bad data length\n");
        cleanup_return(1);
    }

    if (buffer[3 * sizeof(int32_t) + data_len] != '\0')
    {
        fprintf(stderr, "bad rcon packet: data not null terminated\n");
        cleanup_return(1);
    }

    const char* data_str = buffer + 3 * sizeof(int32_t);
    if (callback(msg_id, msg_type, data_str, data_len, userdata))
    {
        fprintf(stderr, "packet callback bad return status\n");
        cleanup_return(1);
    }

cleanup:
    free(buffer);
    return ret;
}

int whitelist_add_rcon_response_callback(int msg_id, int msg_type, const char* data_str, int data_len, void* userdata)
{
    cJSON** response_json = userdata;
    *response_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(*response_json, "type", GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE);
    cJSON* response_message_json = cJSON_AddObjectToObject(*response_json, "data");
    cJSON_AddStringToObject(response_message_json, "content", data_str);
    return 0;
}

int handle_command_whitelist_add(struct gateway_websocket_data* data, const char* interaction_id, const char* interaction_token, const cJSON* options, cJSON** response_json)
{
    int ret = 0;
    char* cmd = NULL;

    const char* username = NULL;

    int nopts = cJSON_GetArraySize(options);
    for (int i = 0; i < nopts; ++i)
    {
        const cJSON* opt_json = cJSON_GetArrayItem(options, i);
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(opt_json, "name"));
        if (!name)
        {
            fprintf(stderr, "failed to parse option name\n");
            goto error;
        }

        if (strcmp(name, "username") == 0)
        {
            username = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(opt_json, "value"));
        }
        else
        {
            fprintf(stderr, "unhandled option: %s\n", name);
        }
    }

    if (!username)
    {
        fprintf(stderr, "failed to parse username\n");
        goto error;
    }

    int username_len = strlen(username);

#define cmd_base "/whitelist add "
    cmd = malloc(sizeof(cmd_base) + username_len);
    memcpy(cmd, cmd_base, sizeof(cmd_base));
    strcpy(cmd + sizeof(cmd_base) - 1, username);

    if (rcon_send(data->rcon_socket, 0, RCON_PACKET_COMMAND, cmd, username_len + sizeof(cmd_base) - 1))
    {
        fprintf(stderr, "failed to send whitelist command rcon");
        goto error;
    }
#undef cmd_base

    struct pollfd pollfd = { .fd = data->rcon_socket, .events = POLLIN };
    int pr = poll(&pollfd, 1, whitelist_request_timeout);
    if (pr < 0)
    {
        perror("failed to poll rcon socket");
        goto error;
    }
    else if (pr == 0)
    {
        *response_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(*response_json, "type", GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE);
        cJSON* response_message_json = cJSON_AddObjectToObject(*response_json, "data");
        cJSON_AddStringToObject(response_message_json, "content", "server timed out");
    }
    else
    {
        if (handle_receive_rcon(data->rcon_socket, whitelist_add_rcon_response_callback, response_json))
        {
            fprintf(stderr, "failed to receive whitelist add rcon response\n");
            goto error;
        }
    }

    goto end;

error:
    if (!(*response_json))
    {
        *response_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(*response_json, "type", GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE);
        cJSON* response_message_json = cJSON_AddObjectToObject(*response_json, "data");
        cJSON_AddStringToObject(response_message_json, "content", "error handling command");
    }
    ret = 1;

end:
    free(cmd);
    return ret;
}

int send_interaction_response(const cJSON* response_json, const char* interaction_id, const char* interaction_token)
{
    int ret = 0;
    char* json_string = NULL;
    char* endpoint_url = NULL;
    CURL* c = NULL;
    struct curl_slist* headers = NULL;

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

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        cleanup_return(1);
    }

cleanup:
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
        cJSON* app_cmd_options_json = cJSON_GetObjectItemCaseSensitive(app_cmd_json, "options");

        const char* name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(app_cmd_json, "name"));
        if (!name)
        {
            fprintf(stderr, "failed to parse app command name\n");
            goto end;
        }

        if (strcmp(name, add_whitelist_app_command) == 0)
        {
            if (handle_command_whitelist_add(data, interaction_id, interaction_token, app_cmd_options_json, &response_json))
            {
                fprintf(stderr, "failed to handle whitelist add command\n");
                goto end;
            }
        }
        else
        {
            response_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(response_json, "type", GATEWAY_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE);
            cJSON* response_message_json = cJSON_AddObjectToObject(response_json, "data");
            cJSON_AddStringToObject(response_message_json, "content", "unhandled command");
            fprintf(stderr, "unhandled command: %s\n", name);
        }

        printf("handled command: %s\n", name);
    }

end:
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

    CURLcode code = curl_easy_perform(c);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(code));
        curl_easy_cleanup(c);
        return NULL;
    }

    return c;
}

int update_ws_socket(struct gateway_websocket_data* data)
{
    int ret = 0;

    if (curl_easy_getinfo(data->c, CURLINFO_ACTIVESOCKET, &data->ws_socket) != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_getinfo failed\n");
        cleanup_return(1);
    }

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->ws_socket,
            &(struct epoll_event) {
                .events = EPOLLIN,
                .data.fd = data->ws_socket,
            }) != 0)
    {
        perror("epoll_ctl failed");
        cleanup_return(1);
    }

cleanup:
    return ret;
}

int handle_reconnect(struct gateway_websocket_data* data)
{
    int ret = 0;

    if (data->ws_socket >= 0)
    {
        if (epoll_ctl(data->epoll_fd, EPOLL_CTL_DEL, data->ws_socket, NULL) != 0)
        {
            perror("epoll_ctl failed");
            cleanup_return(1);
        }
    }
    curl_easy_cleanup(data->c);

    const char* gateway_url = data->resume_gateway_url ? data->resume_gateway_url : data->gateway_url;
    printf("attempting to reconnect to gateway url: %s\n", gateway_url);

    data->c = connect_to_gateway(gateway_url);
    if (!data->c)
    {
        fprintf(stderr, "failed to reconnect to gateway\n");
        cleanup_return(1);
    }

    if (update_ws_socket(data))
    {
        fprintf(stderr, "failed to assign websocket to event handler\n");
        cleanup_return(1);
    }

    printf("reconnected to gateway\n");

cleanup:
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
            if (!ret)
            {
                printf("handshake with gateway server completed\n");
            }
        }
        else if (strcmp(event_name, "RESUMED") == 0)
        {
            printf("connection resume completed\n");
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
    else if (op == GATEWAY_RECONNECT)
    {
        if (handle_reconnect(data))
        {
            fprintf(stderr, "failed to handle reconnect event\n");
            cleanup_return(1);
        }
    }
    else if (op == GATEWAY_INVALID_SESSION)
    {
        int resumable = cJSON_IsTrue(event_data_json);
        printf("session invalidated by gateway server. resumable: %d\n", resumable);
        if (!resumable)
        {
            // invalidate session
            free(data->session_id);
            free(data->resume_gateway_url);
            data->session_id = NULL;
            data->resume_gateway_url = NULL;
        }
        if (handle_reconnect(data))
        {
            fprintf(stderr, "failed to reconnect after invalid session\n");
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
    const struct curl_ws_frame* frame = NULL;

    while (1)
    {
        CURLcode r = curl_ws_recv(data->c, data->buffer + data->offset, data->bufsize - data->offset, &rlen, &frame);
        if (r == CURLE_AGAIN)
        {
            break;
        }
        if (r)
        {
            fprintf(stderr, "curl_ws_recv failed: %d (%s)\n", r, curl_easy_strerror(r));
            return 1;
        }

        data->offset += rlen;

        if (frame->bytesleft > data->bufsize - data->offset)
        {
            data->bufsize = data->offset + frame->bytesleft;
            data->buffer = realloc(data->buffer, data->bufsize);
        }

        if (!frame->bytesleft)
        {
            if (frame->flags & CURLWS_CLOSE)
            {
                printf("websocket closed with message: %.*s\n", (int)data->offset, data->buffer);
                if (handle_reconnect(data))
                {
                    fprintf(stderr, "failed to reconnect after received close\n");
                    return 1;
                }
            }
            else if (handle_gateway_payload(data))
            {
                fprintf(stderr, "failed to handle gateway payload\n");
                return 2;
            }

            data->offset = 0;
        }
    }

    return 0;
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
    cJSON* response_json = NULL;

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

    response_json = cJSON_ParseWithLength(buffer_write_data.buffer, buffer_write_data.offset);
    const char* response_name_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response_json, "name"));
    if (!response_name_str)
    {
        fprintf(stderr, "failed to parse app command name from add command response\n");
        cleanup_return(1);
    }

    if (strcmp(response_name_str, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command_json, "name"))) != 0)
    {
        fprintf(stderr, "app command response name does not match requested add command name\n");
        cleanup_return(1);
    }

cleanup:
    cJSON_Delete(response_json);
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
    cJSON_AddStringToObject(command_json, "name", add_whitelist_app_command);
    cJSON_AddNumberToObject(command_json, "type", GATEWAY_APP_CMD_CHAT_INPUT);
    cJSON_AddStringToObject(command_json, "description", "add user to whitelist");
    cJSON* options_json = cJSON_AddArrayToObject(command_json, "options");
    cJSON* username_option_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(username_option_json, "type", GATEWAY_APP_CMD_OPT_STRING);
    cJSON_AddStringToObject(username_option_json, "name", "username");
    cJSON_AddStringToObject(username_option_json, "description", "username to add");
    cJSON_AddBoolToObject(username_option_json, "required", 1);
    cJSON_AddItemToArray(options_json, username_option_json);

    if (create_app_command(data->app_id, data->token, command_json))
    {
        fprintf(stderr, "failed to create command: %s\n", add_whitelist_app_command);
        cleanup_return(1);
    }

cleanup:
    cJSON_Delete(command_json);
    return ret;
}

int run_websocket_client(CURL* c, int rcon_socket, const char* gateway_url)
{
    int ret = 0;
    int heartbeat_timer = -1;
    struct gateway_websocket_data gateway_websocket_data = {
        .c = c,
        .rcon_socket = rcon_socket,
        .ws_socket = -1,
        .epoll_fd = -1,
        .gateway_url = gateway_url,
    };

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

    printf("app commands registered\n");

    gateway_websocket_data.epoll_fd = epoll_create1(0);
    if (gateway_websocket_data.epoll_fd == -1)
    {
        perror("epoll_create1 failed");
        cleanup_return(1);
    }

    if (update_ws_socket(&gateway_websocket_data))
    {
        fprintf(stderr, "failed to assign websocket to event handler\n");
        cleanup_return(1);
    }

    heartbeat_timer = timerfd_create(CLOCK_REALTIME, 0);
    if (heartbeat_timer == -1)
    {
        perror("timerfd_create failed");
        cleanup_return(1);
    }

    if (epoll_ctl(gateway_websocket_data.epoll_fd, EPOLL_CTL_ADD, heartbeat_timer,
            &(struct epoll_event) {
                .events = EPOLLIN,
                .data.fd = heartbeat_timer
            }) != 0)
    {
        perror("epoll_ctl failed");
        cleanup_return(1);
    }
    gateway_websocket_data.heartbeat_timer = heartbeat_timer;

    while (1)
    {
        struct epoll_event event;
        int epr = epoll_wait(gateway_websocket_data.epoll_fd, &event, 1, -1);

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
            else if (event.data.fd == gateway_websocket_data.ws_socket)
            {
                if (event.events & EPOLLIN)
                {
                    int status = gateway_websocket_receive(&gateway_websocket_data);
                    if (status != 0)
                    {
                        fprintf(stderr, "websocket receive failed: poll events: %X\n", event.events);
                        if (status == 1)
                            cleanup_return(1);
                    }
                }
            }
        }
    }

cleanup:
    free(gateway_websocket_data.buffer);
    free(gateway_websocket_data.session_id);
    free(gateway_websocket_data.resume_gateway_url);
    if (heartbeat_timer > -1)
    {
        close(heartbeat_timer);
    }
    if (gateway_websocket_data.epoll_fd > -1)
    {
        close(gateway_websocket_data.epoll_fd);
    }
    curl_easy_cleanup(gateway_websocket_data.c);

    return ret;
}

int rcon_auth_callback(int msg_id, int msg_type, const char* data_str, int data_len, void* userdata)
{
    if (msg_id != rcon_login_id)
    {
        fprintf(stderr, "rcon login failed: %s\n", data_str);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    int ret = 0;
    int rcon_socket = -1;
    char* gateway_url = NULL;
    CURL* c = NULL;

    const char* rcon_host = getenv("RCON_HOST");
    const char* rcon_port = getenv("RCON_PORT");
    const char* rcon_password = getenv("RCON_PASSWORD");
    if (!rcon_host)
    {
        rcon_host = default_rcon_host;
    }
    if (!rcon_port)
    {
        rcon_port = default_rcon_port;
    }
    if (!rcon_password)
    {
        fprintf(stderr, "required environment variable RCON_PASSWORD not set\n");
        cleanup_return(1);
    }

    rcon_socket = rcon_connect(rcon_host, rcon_port, rcon_password);
    if (rcon_socket < 0)
    {
        fprintf(stderr, "failed to connect to rcon\n");
        cleanup_return(1);
    }

    if (rcon_send(rcon_socket, rcon_login_id, RCON_PACKET_LOGIN, rcon_password, strlen(rcon_password)))
    {
        fprintf(stderr, "failed to send rcon login password\n");
        cleanup_return(1);
    }

    struct pollfd pollfd = { .fd = rcon_socket, .events = POLLIN };
    if (poll(&pollfd, 1, 15000) != 1)
    {
        perror("failed to poll rcon socket");
        cleanup_return(1);
    }

    if (handle_receive_rcon(rcon_socket, rcon_auth_callback, NULL))
    {
        fprintf(stderr, "failed to authenticate rcon with server\n");
        cleanup_return(1);
    }

    printf("connected to server rcon\n");

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

    if (run_websocket_client(c, rcon_socket, gateway_url))
    {
        c = NULL;
        fprintf(stderr, "error running websocket client\n");
        cleanup_return(1);
    }
    c = NULL; /* gets freed during run_websocket_client, TODO: refactor to be less messy */

    printf("exiting\n");

cleanup:
    curl_easy_cleanup(c);
    free(gateway_url);
    curl_global_cleanup();
    if (rcon_socket >= 0)
    {
        close(rcon_socket);
    }

    return ret;
}
