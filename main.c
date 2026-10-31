#include "netinet/in.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/socket.h"
#include "unistd.h"
#include <asm-generic/errno-base.h>
#include <asm-generic/socket.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
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

// Returns the reason phrase for the HTTP status codes used by this server.
// Returns NULL for codes this server does not use.
char *get_http_code_message(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 414:
    return "URI Too Long";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return NULL;
  }
}

// parse_start_line parses data buffer containing startline fragment.
// It stores the value results in http_request fields
// and validates their correctnessc according to HTTP RFC
// Returns 0 on success and HTTP error code (int) on error
int parse_start_line(char *startline, HttpRequest *http_request) {
  assert(startline != NULL);
  assert(http_request != NULL);

  http_request->method = startline;
  char *space = strstr(startline, " ");
  if (space == NULL) {
    return 400;
  }
  *space = '\0';

  http_request->target = space + 1;
  space = strstr(http_request->target, " ");
  if (space == NULL) {
    return 400;
  }
  *space = '\0';

  http_request->version = space + 1;

  // Validate method (Only HEAD and GET are implemented)
  if (strcmp(http_request->method, "GET") != 0 &&
      strcmp(http_request->method, "HEAD") != 0) {
    return 501;
  }
  // Validate version (Both HTTP1.0 and 1.1 are supported)
  if (strcmp(http_request->version, "HTTP/1.1") != 0 &&
      strcmp(http_request->version, "HTTP/1.0") != 0) {
    return 505;
  }
  // Validate path. We support only origin-form
  if (http_request->target[0] != '/') {
    return 400;
  }
  if (strlen(http_request->target) > MAX_URI_LENGTH) {
    return 414;
  }

  return 0;
}

// parse_headers parses data buffer containing headers fragment
// It stores value results in http_request.headers data structure
// and validates correctness of host header
// Returns 0 on success and HTTP error code (int) on error
int parse_headers(char *headers, HttpRequest *http_request) {
  assert(headers != NULL);
  assert(http_request != NULL);

  while (1) {
    char *crlf = strstr(headers, "\r\n");
    if (crlf == NULL) {
      break;
    }
    *crlf = '\0';

    char *colon = strstr(headers, ":");
    if (colon == NULL) {
      return 400;
    }
    *colon = '\0';
    char *value = colon + 1;

    if (http_headers_add(&http_request->headers, headers, value) < 0) {
      return 400;
    }

    headers = crlf + 2;
  }

  // Host header validation
  // TODO: Check if host header has valid value
  int host_count = 0;
  for (int i = 0; i < http_request->headers.len; i++) {
    if (strcmp(http_request->headers.entries[i].key, "host") == 0) {
      host_count++;
    }
  }

  if (host_count > 1) {
    return 400;
  }
  if (strcmp(http_request->version, "HTTP/1.1") == 0 && host_count == 0) {
    return 400;
  }

  return 0;
}

// get_mimetype returns the content mimetype based on the filename extension.
// Falls back to application/octet-stream for unknown or missing extensions.
char *get_mimetype(char *filename) {
  assert(filename != NULL);

  char *ext = strrchr(filename, '.');
  if (ext == NULL) {
    return "application/octet-stream";
  }

  if (strcmp(ext, ".html") == 0) {
    return "text/html";
  }
  if (strcmp(ext, ".css") == 0) {
    return "text/css";
  }
  if (strcmp(ext, ".js") == 0) {
    return "application/javascript";
  }

  return "application/octet-stream";
}

void handle_request(int fd, HttpRequest *http_request) {
  // Strip query and fragment
  char *path = strdup(http_request->target);
  if (path == NULL) {
    perror("strdup");
    return;
  }

  char *rest = strpbrk(path, "?#");
  if (rest != NULL) {
    *rest = '\0';
    rest += 1;
  }

  // Resource path
  char full_path[MAX_URI_LENGTH];
  sprintf(full_path, "public%s", path);

  // Transform / into index.html
  if (path[strlen(path) - 1] == '/') {
    strcat(full_path, "index.html");
  }

  int rfd = open(full_path, O_RDONLY);
  if (rfd < 0) {
    if (errno == ENOENT) {
      dprintf(fd, "HTTP/1.1 404 %s\r\n\r\n", get_http_code_message(404));
    } else {
      perror("open");
    }
    return;
  }

  // Get content length from stats
  struct stat st;
  int ret = fstat(rfd, &st);
  if (ret < 0) {
    perror("fstat");
    close(rfd);
    return;
  }

  // Get mimetype
  char *mimetype = get_mimetype(full_path);

  // Send initial response data (startline + headers)
  dprintf(fd,
          "HTTP/1.1 200 %s\r\nContent-Length: %ld\r\nContent-Type: %s; "
          "charset=utf-8\r\n\r\n",
          get_http_code_message(200), st.st_size, mimetype);

  long offset = 0;
  while (offset < st.st_size) {
    int bytes_written = sendfile(fd, rfd, &offset, st.st_size - offset);
    if (bytes_written <= 0) {
      if (bytes_written < 0 && errno == EINTR) {
        continue;
      }
      if (bytes_written < 0) {
        perror("sendfile");
      }
      break;
    }
  }

  close(rfd);
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

      int res = parse_start_line(buf, &http_request);
      if (res > 0) {
        dprintf(cfd, "HTTP/1.1 %d %s\r\n\r\n", res, get_http_code_message(res));
        close(cfd);
        return;
      }
    }

    if (headers && !body) {
      char *crlf = strstr(headers, "\r\n\r\n");
      if (crlf == NULL) {
        // No headers edge case
        crlf = strstr(headers, "\r\n");
        if (headers != crlf) {
          continue;
        }
      } else {
        crlf += 2;
      }

      *crlf = '\0';
      body = crlf + 2;

      int res = parse_headers(headers, &http_request);
      if (res > 0) {
        dprintf(cfd, "HTTP/1.1 %d %s\r\n\r\n", res, get_http_code_message(res));
        close(cfd);
        return;
      }
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

  handle_request(cfd, &http_request);
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
