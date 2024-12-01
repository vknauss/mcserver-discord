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

#define discord_get_gateway_url "https://discord.com/api/v10/gateway"

#define gateway_url_max_size 255

struct gateway_url_write_data
{
    char* buffer;
    int offset;
};

size_t gateway_url_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    struct gateway_url_write_data* data = userdata;
    size_t count = gateway_url_max_size - 1 - data->offset;
    count = count > nmemb ? nmemb : count;
    memcpy(data->buffer, ptr, count);
    data->offset += count;
    data->buffer[data->offset] = 0;
    return count;
}

int get_gateway_url(char* buffer, size_t bufsize, const char* queryparams)
{
    int ret = 0;
    CURL* c = NULL;
    cJSON* response_json = NULL;

    c = curl_easy_init();
    if (!c)
    {
        fprintf(stderr, "curl_easy_init failed\n");
        cleanup_return(1);
    }

    char gateway_url_results[gateway_url_max_size + 1];
    struct gateway_url_write_data gateway_url_write_data = { gateway_url_results, 0 };

    curl_easy_setopt(c, CURLOPT_URL, discord_get_gateway_url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, gateway_url_write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &gateway_url_write_data);
    if (curl_easy_perform(c) != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed\n");
        cleanup_return(1);
    }

    response_json = cJSON_Parse(gateway_url_results);
    cJSON* gateway_url_json = cJSON_GetObjectItemCaseSensitive(response_json, "url");
    char* gateway_url = cJSON_GetStringValue(gateway_url_json);
    if (!gateway_url)
    {
        fprintf(stderr, "failed to parse gateway url query results\n");
        cleanup_return(1);
    }

    if (queryparams)
    {
        snprintf(buffer, bufsize, "%s/?%s", gateway_url, queryparams);
    }
    else
    {
        strncpy(buffer, gateway_url, bufsize);
    }

cleanup:
    cJSON_Delete(response_json);
    curl_easy_cleanup(c);
    return ret;
}

struct gateway_websocket_data
{
    CURL* c;
    int heartbeat_timer;
    size_t bufsize;
    size_t offset;
    char* buffer;
};

int handle_gateway_payload(struct gateway_websocket_data* data)
{
    int ret = 0;
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

    int op = op_code_json->valueint;
    printf("opcode: %d\n", op);

    if (op == 10) /* hello event */
    {
        cJSON* event_data_json = cJSON_GetObjectItemCaseSensitive(gateway_payload_json, "d");
        cJSON* heartbeat_interval_json = cJSON_GetObjectItemCaseSensitive(event_data_json, "heartbeat_interval");
        if (!cJSON_IsNumber(heartbeat_interval_json))
        {
            fprintf(stderr, "failed to parse heartbeat interval\n");
            cleanup_return(1);
        }

        printf("setting heartbeat_timer: %d\n", heartbeat_interval_json->valueint);
        if (timerfd_settime(data->heartbeat_timer, 0, &(struct itimerspec) {
                    .it_value.tv_sec = heartbeat_interval_json->valueint / 1000,
                    .it_value.tv_nsec = (heartbeat_interval_json->valueint % 1000) * 1000000,
                }, NULL) != 0)
        {
            perror("timerfd_settime failed");
            cleanup_return(1);
        }
    }

cleanup:
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
    curl_easy_setopt(c, CURLOPT_VERBOSE, 1);
    if (curl_easy_perform(c) != CURLE_OK)
    {
        fprintf(stderr, "curl_easy_perform failed\n");
        curl_easy_cleanup(c);
        return NULL;
    }

    return c;
}

int run_websocket_client(CURL* c)
{
    int ret = 0;
    int efd = -1;
    int heartbeat_timer = -1;
    struct gateway_websocket_data gateway_websocket_data = { .c = c };

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
                    // TODO: send gateway heartbeat
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
    CURL* c = NULL;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        fprintf(stderr, "curl_global_init failed\n");
        cleanup_return(1);
    }

    char gateway_url[gateway_url_max_size + 1];
    if (get_gateway_url(gateway_url, sizeof(gateway_url), "v=10&encoding=json"))
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
    curl_global_cleanup();

    return ret;
}
