#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <uv.h>

#define PORT 80
#define CHUNK 1024

#define OK_RESPONSE \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: %s/%s\r\n" \
    "\r\n"

#define NOT_FOUND_RESPONSE \
    "HTTP/1.1 404 NOT FOUND\r\n" \
    "\r\n"


typedef enum {
    TEXT,
    IMAGE,
    VIDEO
} mime;

typedef struct http_req_s {
    char *method;
    char *path;
    char __gap__[1];
    char *ext;
    mime mime;
    const uv_buf_t *buf;
} http_req_t;

typedef struct http_lines_s {
    char * content;
    struct http_lines_s * next;
} http_lines_t;

typedef struct http_inflight_s {
    char *current;
    http_lines_t *head;
    http_lines_t *tail;
    const uv_buf_t *buf;
} http_inflight_t;

void free_inflight(http_inflight_t *inflight) {
    http_lines_t *line = inflight->head;
    
    while(line) {
        http_lines_t *to_free = line;
        line = to_free->next;
        free(to_free);
    }
    
    free(inflight->buf->base);
    free(inflight);
}

void free_req(http_req_t *req) {
    free(req->method);
    free(req->path);
    free(req->ext);
    free(req);
}

http_req_t *http_req_new() {
    http_req_t *val = malloc(sizeof(http_req_t));
    val->method = NULL;
    val->path = NULL;
    val->ext = NULL;

    return val;
}

http_lines_t * http_lines_new(char *line) {
    http_lines_t *next_line = malloc(sizeof(http_lines_t));
    next_line->content = line;
    next_line->next = NULL;

    return next_line;
}

void http_inflight_append(http_inflight_t *inflight, http_lines_t *line) {
    if (inflight->head) {
        if(inflight->tail) {
            inflight->tail->next = line;
        } else {
            inflight->head->next = line;
        }
        inflight->tail = line;
    } else {
        inflight->head = line;
    }
}

http_req_t * http_inflight_parse(http_inflight_t *inflight) {
    http_lines_t *line = inflight->head;

    int i; mime mime; char *token, *brkt, *method, *path = "", *ext;
    for(i = 0, token = strtok_r(line->content, " \t\r\n", &brkt); token; i++, token = strtok_r(NULL, " \t\r\n", &brkt)) {
        switch(i) {
            case 0:
                method = token;
                break;
            case 1:
                path = token;
                break;
            case 2:
                break;
            default:
                fprintf(stderr, "Unexpeted request string in http request: %s\n", line->content);
                exit(1);
        }
    }

    
    char *target_path;
    if (strlen(path) < 2) {
        target_path = "/index.html";
    } else {
        target_path = path;
    }
    path = calloc(strlen(path) + 1, 0);
    *path = '.';
    strcpy(path + 1, target_path);

    char *tokens = strdup(path);
    for(token = strtok_r(tokens, ".", &brkt); token; token = strtok_r(NULL, ".", &brkt)) {
        ext = token;
    }
    ext = strstr(path, ext);
    free(tokens);

    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "png") == 0 || strcmp(ext, "gif") == 0 || strcmp(ext, "gif") == 0) {
        mime = IMAGE;
    } else if(strcmp(ext, "htm") == 0 || strcmp(ext, "html") == 0 || strcmp(ext, "css") == 0) {
        mime = TEXT;
    } else if(strcmp(ext, "mkv") == 0 || strcmp(ext, "avi") == 0 || strcmp(ext, "mp4") == 0) {
        mime = VIDEO;
    } else {
        mime = TEXT;
    }

    http_req_t *req = http_req_new();
    req->method = strdup(method);
    req->path = path;
    req->mime = mime;
    req->ext = strdup(ext);
    req->buf = inflight->buf;

    // printf("GOT REQUEST:\tM: %s; P: %s; E: %s; M: %i\n", req->method, req->path, req->ext, req->mime);
    
    free_inflight(inflight);

    return req;
}

http_inflight_t * http_inflight_new(const uv_buf_t *buf) {
    http_inflight_t *val = malloc(sizeof(http_inflight_t));
    val->current = buf->base;
    val->head = NULL;
    val->tail = NULL;
    val->buf = buf;

    char *line, *brkt;
    for(line = strtok_r(val->current, "\r\n", &brkt); line; line = strtok_r(NULL, "\r\n", &brkt)) {
        http_lines_t *next_line = http_lines_new(line);

        if (line && strlen(line)) {
            http_inflight_append(val, next_line);
        }
    }

    return val;
}

#define AS_HANDLE(HANDLE) ((uv_handle_t *) (HANDLE))
#define AS_STREAM(STREAM) ((uv_stream_t *) (STREAM))
#define AS_WRITE(WRITE) ((uv_write_t *) (WRITE))
#define WRITE_REQ(WRITE) ((write_req_t *) (WRITE))
#define AS_FS(FS) ((uv_fs_t *) (FS))
#define FS_REQ(FS) ((fs_req_t *)(FS))

typedef struct {
    http_req_t *req;
    uv_stream_t *client;
    ssize_t file;
    uv_buf_t buf;
    char reserved[CHUNK];
} serve_t;

typedef struct {
    uv_fs_t fs;
    uv_stream_t *client;
    http_req_t *req;
} serve_open_t;

typedef struct {
    uv_fs_t fs;
    serve_open_t *open;
    uv_buf_t buf;
    char reserved[CHUNK];
} serve_read_t;

typedef struct {
    uv_fs_t fs;
    serve_t *serve;
} fs_req_t;

typedef struct {
    uv_write_t write;
    serve_t *serve;
    bool done;
} write_req_t;

uv_loop_t *loop;
uv_tcp_t server;
struct sockaddr_in addr;

void on_read(uv_fs_t *req);


void free_serve(serve_t *serve) {
    free_req(serve->req);
//    free(serve->client);
    free(serve);
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

void on_close(uv_handle_t *client) {
    free(client);
}

void on_send(uv_write_t *res, int status) {
    write_req_t *write = WRITE_REQ(res);
    serve_t *serve = write->serve;
    if (status) {
        fprintf(stderr, "Write error %s\n", uv_strerror(status));
    }

    if (write->done) {
        uv_close(AS_HANDLE(serve->client), on_close);
        free_serve(serve);
    } else {
        serve->buf.len = sizeof(serve->reserved);

        fs_req_t *read = malloc(sizeof(fs_req_t));
        read->serve = write->serve;
        uv_fs_read(loop, AS_FS(read), serve->file, &serve->buf, 1, -1, on_read);
    }

    free(write);
}

void on_read(uv_fs_t *res) {
    fs_req_t *fs = FS_REQ(res);

    serve_t *serve = fs->serve;
    uv_fs_req_cleanup(res);
    
    bool done = false;

    if (res->result < 0) {
        fprintf(stderr, "Read error: %s\n", uv_strerror(res->result));
        serve->buf.len = 0;
        done = true;
    } else if (res->result == 0) {
        serve->buf.len = 0;
        done = true;
        uv_fs_close(loop, res, serve->file, NULL); // synchronous
    } else if (res->result > 0) {
        serve->buf.len = res->result;
    }

    write_req_t *write = malloc(sizeof(write_req_t));
    write->serve = serve;
    write->done = done;
    uv_write(AS_WRITE(write), serve->client, &serve->buf, 1, on_send);
    
    uv_fs_req_cleanup(res);
    free(res);
}

void on_open(uv_fs_t *res) {
    fs_req_t *fs = FS_REQ(res);

    serve_t *serve = fs->serve;
    http_req_t *http = serve->req;
    uv_fs_req_cleanup(res);
    free(fs);

    bool done = false;
    ssize_t len = 0;

    if (res->result >= 0) {
        serve->file = res->result;

        const char *type;

        switch(http->mime) {
            case TEXT:
                type = "text";
                break;
            case IMAGE:
                type = "image";
                break;
            case VIDEO:
                type = "video";
                break;
            default:
                type = "text";
        }

        sprintf(serve->reserved, OK_RESPONSE, type, http->ext);
        serve->buf.len = (strlen(OK_RESPONSE) - 4) + strlen(type) + strlen(http->ext);
    } else {
        fprintf(stderr, "error opening file: %s\n", uv_strerror((int)res->result));
        sprintf(serve->reserved, NOT_FOUND_RESPONSE);
        serve->buf.len = strlen(NOT_FOUND_RESPONSE);
        done = true;
    }

    write_req_t *write = malloc(sizeof(write_req_t));
    write->serve = serve;
    write->done = done;

    uv_write(AS_WRITE(write), serve->client, &serve->buf, 1, on_send);
}

void on_receive(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        serve_t * serve = malloc(sizeof(serve_t));
        serve->req = http_inflight_parse(http_inflight_new(buf));
        serve->client = client;
        serve->buf = uv_buf_init(serve->reserved, sizeof(serve->reserved));

        fs_req_t *fs = malloc(sizeof(fs_req_t));
        fs->serve = serve;
        
        uv_fs_open(loop, AS_FS(fs), serve->req->path, O_RDONLY, 0, on_open);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
        }
        uv_close(AS_HANDLE(client), on_close);
        free(client);
    }
}

void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error: %s\n", uv_strerror(status));
        return;
    }

    uv_tcp_t *client = malloc(sizeof(uv_tcp_t));
    uv_tcp_init(loop, client);
    
    if (uv_accept(server, AS_STREAM(client)) == 0) {
        uv_read_start(AS_STREAM(client), alloc_buffer, on_receive);
    } else {
        uv_close(AS_HANDLE(client), on_close);
    }
}

int main() {
    loop = uv_default_loop();

    uv_tcp_init(loop, &server);

    uv_ip4_addr("0.0.0.0", PORT, &addr);
    uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);

    int err = uv_listen(AS_STREAM(&server), 128, on_new_connection);
    if(err) {
        fprintf(stderr, "Listen error: %s\n", uv_strerror(err));
        return 1;
    }

    return uv_run(loop, UV_RUN_DEFAULT);
}
