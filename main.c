#include <stdio.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#define DEBUG
#ifdef DEBUG
#define LOG(info, data, ...) { printf("[%s] ", (info)); printf((data), __VA_ARGS__); printf("\n"); fflush(stdout); }
#else
#define LOG(info, data, ...) {}
#endif

int make_server_socket(int port) {
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    struct sockaddr_in addr;
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, &addr, sizeof(addr)) < 0) return -errno;
    return fd;
}

struct http_header {
    const char *name, *value;
};

struct http_response {
    int connfd, databuf_size, data_size;
    void* databuf;
};

struct http_response *new_http_response(int connfd) {
    struct http_response *result = malloc(sizeof(struct http_response));
    result->connfd = connfd;
    result->databuf_size = 1024;
    result->data_size = 0;
    result->databuf = malloc(1024);
    return result;
}

typedef int (*http_listener)(const char *method, const char *url, const char *protocol,
        const struct http_header *headers, int header_count, const struct http_response* response);

struct socket_listener_args {
    int connfd;
    http_listener listener;
};

void http_status(struct http_response *response, int code, const char* desc) {
    char* line;
    asprintf(&line, "HTTP/1.1 %d %s\n", code, desc);
    write(response->connfd, line, strlen(line));
}

void http_header(struct http_response *response, const char* name, const char* val) {
    char* line;
    asprintf(&line, "%s: %s\n", name, val);
    write(response->connfd, line, strlen(line));
}

void http_data(struct http_response *response, void* buf, int len) {
    if (response->data_size + len > response->databuf_size) {
        while (response->databuf_size <= response->data_size + len) {
            response->databuf_size *= 2;
        }
        void* newbuf = malloc(response->databuf_size);
        memcpy(newbuf, response->databuf, response->data_size);
        response->databuf = newbuf;
    }
    memcpy(response->databuf + response->data_size, buf, len);
    response->data_size += len;
}

void http_close(struct http_response *response) {
    char* line;
    asprintf(&line, "Content-Length: %d\n\n", response->data_size);
    write(response->connfd, line, strlen(line));
    write(response->connfd, response->databuf, response->data_size);
    LOG("HTTP", "close %d", response->connfd);
    close(response->connfd);
}

typedef void (*server_socket_callback)(const struct socket_listener_args *);

int run_server_socket(server_socket_callback callback, http_listener listener, int port, int max_conns) {
    int errcode;
    int connfd = -1;
    int fd = make_server_socket(port);
    if (fd < 0) {
        errcode = -fd;
        goto error;
    }
    if (listen(fd, max_conns) < 0) {
        errcode = errno;
        goto error;
    }
    while (1) {
        connfd = accept(fd, 0, 0);
        pthread_t pid;
        struct socket_listener_args *args = malloc(sizeof(struct socket_listener_args));
        args->connfd = connfd;
        args->listener = listener;
        errcode = pthread_create(&pid, 0, callback, args);
        if (errcode) goto error;
    }
    error:
    if (fd > 0) close(fd);
    if (connfd > 0) close(connfd);
    return errcode;
}

struct server_socket_settings {
    server_socket_callback callback;
    http_listener listener;
    int port, max_conns;
};

void *run_server_socket_pthread(void *arg) {
    struct server_socket_settings settings = *(struct server_socket_settings*) arg;
    int result = run_server_socket(settings.callback, settings.listener, settings.port, settings.max_conns);
    int* ret = malloc(sizeof(int));
    *ret = result;
    return ret;
}

int start_socket_server(pthread_t* tid, server_socket_callback callback,
                        http_listener listener, int port, int max_conns) {
    struct server_socket_settings* args = malloc(sizeof(struct server_socket_settings));
    args->callback = callback; args->port = port; args->max_conns = max_conns;
    args->listener = listener;
    int res = pthread_create(tid, 0, run_server_socket_pthread, args);
    return res;
}

void error(const char* msg, int code) {
    if (msg) {
        fprintf(stderr, "%s: ", msg);
    }
    fprintf(stderr, "%s (code %d)\n", strerror(code), code);
    fflush(stderr);
    exit(code);
}

int sockets_count = 0;

int socket_listener(const struct socket_listener_args* args) {
    int connfd = args->connfd;
    int id = __atomic_fetch_add(&sockets_count, 1, __ATOMIC_SEQ_CST);
    LOG("SOCK", "Open socket %d", id)
    int size = 1024;
    char* data = malloc(size+1);
    int read_size;
    int ptr = 0;
    int read_ptr = 0;
    char* lines[128];
    int line_ptr = 0;
    while ((read_size = recv(connfd, data + read_ptr, size - read_ptr, 0)) > 0) {
        if (read_size == size - read_ptr) {
            size *= 2;
            char* new_data = malloc(size + 1);
            memcpy(new_data, data, size/2);
            data = new_data;
        }
        read_ptr += read_size;
        while (ptr < read_ptr) {
            int start = ptr;
            while (ptr < read_ptr && data[ptr] != '\r' && data[ptr] != '\n') {
                ptr++;
            }
            if (data[ptr] == '\r' || data[ptr] == '\n') {
                if (start == ptr) {
                    goto end_recv_loop;
                } else {
                    int len = ptr - start;
                    if (data[ptr] == '\r') ptr++;
                    ptr++;
                    lines[line_ptr] = malloc(len + 1);
                    memcpy(lines[line_ptr], data + start, len);
                    lines[line_ptr][len] = 0;
                    LOG("SOCK", "add line: '%s'", lines[line_ptr])
                    line_ptr++;
                }
            } else {
                ptr = start;
                break;
            }
        }
        //write(connfd, data, read_size);
        //data[size] = 0;
        LOG("SOCK", "data: '%s', ptr: %d", data, read_ptr)
    }
    end_recv_loop:
    if (read_size < 0) return errno;
    int method_end = 0;
    while (lines[0][method_end] && lines[0][method_end] != ' ') method_end++;
    char* method = malloc(method_end + 1);
    memcpy(method, lines[0], method_end);
    method[method_end] = 0;
    if (strcmp(method, "GET") != 0) {
        LOG("SOCK", "Got method '%s', only GET is supported", method)
        goto close_socket;
    }
    int url_start = method_end;
    while (lines[0][url_start] == ' ') url_start++;
    int url_end = url_start;
    while (lines[0][url_end] && lines[0][url_end] != ' ') url_end++;
    char* url = malloc(url_end - url_start + 1);
    memcpy(url, lines[0] + url_start, url_end - url_start);
    url[url_end - url_start] = 0;
    if (url_start == url_end || url[0] != '/') {
        LOG("SOCK", "Got a bad URL '%s'", url)
        goto close_socket;
    }
    int protocol_start = url_end;
    while (lines[0][protocol_start] == ' ') protocol_start++;
    int protocol_end = protocol_start;
    while (lines[0][protocol_end] && lines[0][protocol_end] != ' ') protocol_end++;
    char* protocol = malloc(protocol_end - protocol_start + 1);
    memcpy(protocol, lines[0] + protocol_start, protocol_end - protocol_start);
    protocol[protocol_end - protocol_start] = 0;
    if (strcmp(protocol, "HTTP/1.1") != 0) {
        LOG("SOCK", "Got protocol '%s', only HTTP/1.1 is supported", protocol)
        goto close_socket;
    }
    struct http_header* headers = malloc(128 * sizeof(struct http_header));
    for (int i = 0; i < line_ptr-1; i++) {
        char* line = lines[i+1];
        int len = strlen(line);
        int name_end = 0;
        while (line[name_end] && line[name_end] != ':') name_end++;
        if (line[name_end] != ':' || line[name_end+1] != ' ') {
            LOG("SOCK", "Bad line '%s', expected $HEADER_NAME$: $HEADER_VALUE$", line)
            goto close_socket;
        }
        char* name = malloc(name_end + 1);
        memcpy(name, line, name_end);
        name[name_end] = 0;
        char* value = malloc(len - name_end - 1);
        strcpy(value, line + name_end + 2);
        headers[i].name = name;
        headers[i].value = value;
    }
    LOG("SOCK", "Invoke http listener", 0)
    return args->listener(method, url, protocol, headers, line_ptr-1, new_http_response(connfd));
    close_socket:
    close(connfd);
    LOG("SOCK", "Close socket %d", id)
    return 0;
}

int start_http_server(pthread_t *tid, http_listener listener, int port, int max_conns) {
    return start_socket_server(tid, socket_listener, listener, port, max_conns);
}

// GET / HTTP/1.1
// Host: localhost:8080
// User-Agent: curl/7.64.1
// Accept: */*

char* base_path = 0;

int my_http_listener(const char *method, const char *url, const char *protocol,
                     const struct http_header *headers, int header_count,
                             struct http_response *response) {
    int size = 2048;
    char* reply = malloc(size);
    snprintf(reply, size, "Method: %s, url: '%s', protocol: %s, header count: %d\n", method, url, protocol, header_count);
    int end = strlen(reply);
    for (int i = 0; i < header_count; i++) {
        snprintf(reply + end, size - end, "header %d: '%s'->'%s' ", i+1, headers[i].name, headers[i].value);
        end = strlen(reply);
    }
    LOG("HTTP", "http request: %s", reply)
    //write(connfd, reply, end);
    char* path;
    asprintf(&path, "%s%s", base_path, url);
    LOG("HTTP", "open %s", path)
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        http_status(response, 200, "OK");
        int buf_size = 1024;
        char *buf = malloc(buf_size);
        int fread_size;
        while ((fread_size = read(fd, buf, buf_size)) > 0) {
            http_data(response, buf, fread_size);
            LOG("HTTP", "read %d bytes", fread_size)
        }
        free(buf);
    } else {
        http_status(response, 403, "Forbidden");
        char* errmsg;
        asprintf(&errmsg, "%s: %s", "File open error", strerror(errno));
        LOG("HTTP", "Error: %s", errmsg)
        http_data(response, errmsg, strlen(errmsg));
    }
    http_close(response);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        base_path = argv[1];
    } else {
        base_path = malloc(1024);
        getcwd(base_path, 1024);
    }
    LOG("SERV", "Base path: %s", base_path);
    pthread_t tid;
    int res = start_http_server(&tid, my_http_listener, 8080, 10);
    if (res) error("Socket process error", res);
    printf("Hello, World!\n");
    int *err = malloc(sizeof(int));
    pthread_join(tid, &err);
    if (*err) {
        error("Socket error", *err);
    }
    return 0;
}
