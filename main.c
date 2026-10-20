#include "netinet/in.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/socket.h"
#include "unistd.h"
#include <asm-generic/socket.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_REQUEST_SIZE 65536
#define MAX_REQUEST_HEADERS 64
#define MAX_URI_LENGTH 1024

typedef struct {
  char *key;
  char *value;
} HttpHeader;

typedef struct {
  HttpHeader entries[MAX_REQUEST_HEADERS];
  int len;
} HttpHeaders;

typedef struct {
  // Start-line
  char *method;
  char *target;
  char *version;

  HttpHeaders headers;
} HttpRequest;

// Removes string leading and trailing whitespaces.
// Zero-copy version
char *trim(char *s) {
  assert(s != NULL);

  while (*s == ' ') {
    s++;
  }

  int slen = strlen(s);
  while (slen > 0) {
    if (s[slen - 1] == ' ') {
      s[slen - 1] = '\0';
      slen--;
      continue;
    }

    break;
  }

  return s;
}

int http_headers_add(HttpHeaders *http_headers, char *key, char *value) {
  assert(http_headers != NULL);
  assert(key != NULL);
  assert(value != NULL);

  value = trim(value);

  for (int i = 0; i < strlen(key); i++) {
    key[i] = tolower(key[i]);
  }

  if (http_headers->len >= MAX_REQUEST_HEADERS) {
    return -1;
  }

  HttpHeader header = {.key = key, .value = value};
  http_headers->entries[http_headers->len] = header;
  http_headers->len++;

  return 0;
}

char *http_headers_get(HttpHeaders *http_headers, char *key) {
  assert(http_headers != NULL);
  assert(key != NULL);

  // INFO: Maybe try changing to a just array buffer
  // instead of allocating on the heap when duplicating string.
  //
  // Duplicate string so original key won't be modified
  char *key_copy = strdup(key);
  if (key_copy == NULL) {
    return NULL;
  }

  for (int i = 0; i < strlen(key_copy); i++) {
    key_copy[i] = tolower(key_copy[i]);
  }

  for (int i = 0; i < http_headers->len; i++) {
    if (strcmp(http_headers->entries[i].key, key_copy) != 0) {
      continue;
    }

    free(key_copy);
    return http_headers->entries[i].value;
  }

  free(key_copy);
  return NULL;
}

// Logic for how to handle incoming client
void handle_client(int cfd) {
  char buf[MAX_REQUEST_SIZE];
  char *headers = NULL;
  char *body = NULL;

  HttpRequest http_request = {
      .method = NULL, .target = NULL, .version = NULL, .headers.len = 0};

  long content_len = 0;
  int total_n = 0;

  while (1) {
    // If buffer is full
    if (total_n >= MAX_REQUEST_SIZE - 1) {
      fprintf(stderr, "handle_client: buffer reached MAX_REQUEST_SIZE");
      close(cfd);
      return;
    }

    ssize_t n = read(cfd, buf + total_n, (MAX_REQUEST_SIZE - total_n) - 1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }

      perror("read");
      close(cfd);
      return;
    }
    if (n == 0) {
      close(cfd);
      return;
    }

    total_n += n;

    if (!headers) {
      char *crlf = strstr(buf, "\r\n");
      if (crlf == NULL) {
        continue;
      }
      *crlf = '\0';
      headers = crlf + 2;

      // Parse Start-line
      http_request.method = buf;
      char *space = strstr(buf, " ");
      if (space == NULL) {
        fprintf(stderr, "handle_client: Invalid start-line format");
        close(cfd);
        return;
      }
      *space = '\0';
      http_request.target = space + 1;

      char *space2 = strstr(http_request.target, " ");
      if (space2 == NULL) {
        fprintf(stderr, "handle_client: Invalid start-line format");
        close(cfd);
        return;
      }
      *space2 = '\0';
      http_request.version = space2 + 1;
    }

    // Validate method (Only HEAD and GET are implemented)
    if (strcmp(http_request.method, "GET") != 0 &&
        strcmp(http_request.method, "HEAD") != 0) {
      dprintf(cfd, "HTTP/1.1 501 Not Implemented\r\n\r\n");
      close(cfd);
      return;
    }
    // Validate version (Both HTTP1.0 and 1.1 are supported)
    if (strcmp(http_request.version, "HTTP/1.1") != 0 &&
        strcmp(http_request.version, "HTTP/1.0") != 0) {
      dprintf(cfd, "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n");
      close(cfd);
      return;
    }
    // Validate path. We support only origin-form
    if (http_request.target[0] != '/') {
      dprintf(cfd, "HTTP/1.1 400 Bad Request\r\n\r\n");
      close(cfd);
      return;
    }
    if (strlen(http_request.target) > MAX_URI_LENGTH) {
      dprintf(cfd, "HTTP/1.1 414 URI Too Long\r\n\r\n");
      close(cfd);
      return;
    }

    if (headers && !body) {
      char *hcrlf = strstr(headers, "\r\n\r\n");
      if (hcrlf == NULL) {
        continue;
      }
      *hcrlf = '\0';
      body = hcrlf + 4;

      // Parse headers
      char *fcrlf = NULL;
      char *rest = headers;
      int header_len = 0;

      while (1) {
        fcrlf = strstr(rest, "\r\n");
        if (fcrlf != NULL) {
          *fcrlf = '\0';
        }

        char *value = NULL;

        char *colon = strstr(rest, ":");
        if (colon == NULL) {
          fprintf(stderr, "handle_client: Invalid headers format");
          close(cfd);
          return;
        }
        *colon = '\0';
        value = colon + 1;

        if (http_headers_add(&http_request.headers, rest, value) < 0) {
          fprintf(
              stderr,
              "http_request_add_header: Exceeded limit of MAX_REQUEST_HEADERS");
          close(cfd);
          return;
        }

        if (fcrlf == NULL) {
          break;
        }
        rest = fcrlf + 2;
      }
    }

    // Host header validation
    // TODO: Check if valid value
    int host_count = 0;
    for (int i = 0; i < http_request.headers.len; i++) {
      if (strcmp(http_request.headers.entries[i].key, "host") == 0) {
        host_count++;
      }
    }
    if (host_count > 1) {
      dprintf(cfd, "HTTP/1.1 400 Bad Request\r\n\r\n");
      close(cfd);
      return;
    }
    if (strcmp(http_request.version, "HTTP/1.1") == 0 && host_count == 0) {
      dprintf(cfd, "HTTP/1.1 400 Bad Request\r\n\r\n");
      close(cfd);
      return;
    }

    char *content_len_raw =
        http_headers_get(&http_request.headers, "Content-length");
    if (content_len_raw) {
      errno = 0;
      content_len = strtol(content_len_raw, NULL, 10);
      if (errno != 0) {
        perror("strtol");
        close(cfd);
        return;
      }
    }
    // Guarranted headers + body at this stage
    if (headers && body && content_len > 0) {
      printf("Reading body size = %ld\n", content_len);

      size_t body_read = total_n - (headers - buf);
      if (body_read < content_len) {
        continue;
      }
      buf[total_n] = '\0';
    }
    break;
  };

  // strip query and fragment
  char *path = strdup(http_request.target);
  if (path == NULL) {
    perror("strdup");
    close(cfd);
    return;
  }

  char *rest = strpbrk(path, "?#");
  if (rest != NULL) {
    *rest = '\0';
    rest += 1;
  }

  // Send response
  char res_body[512];
  sprintf(res_body, "path: %s\n", path);
  dprintf(cfd,
          "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: text/plain; "
          "charset=utf-8\r\n\r\n%s",
          strlen(res_body), res_body);

  close(cfd);
  return;
}

int main() {
  // Opens an endpoint/file descriptor for communication.
  // 0 chooses TCP protocol automatically
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  int opt = 1;
  int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (ret < 0) {
    perror("setsockopt");
    close(fd);
    return EXIT_FAILURE;
  }

  // man 7 ip for info
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(8000),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

  ret = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0) {
    perror("bind");
    close(fd);
    return EXIT_FAILURE;
  }

  printf("PID: %d | Socket FD: %d\n", getpid(), fd);

  ret = listen(fd, SOMAXCONN);
  if (ret < 0) {
    perror("listen");
    close(fd);
    return EXIT_FAILURE;
  }

  // Basic TCP accept loop
  // accept -> read data -> process data -> close connection
  while (1) {
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
      // Aborted by client or interrupted by signal
      if (errno == ECONNABORTED || errno == EINTR) {
        continue;
      }
      // Resource exhaustation
      if (errno == EMFILE || errno == ENFILE || errno == ENOMEM ||
          errno == ENOBUFS) {
        perror("accept");
        sleep(1);
        continue;
      }
      perror("accept");
      break;
    }

    handle_client(cfd);
  }
  close(fd);
  return EXIT_SUCCESS;
}
