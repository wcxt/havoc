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

typedef struct {
  char *key;
  char *value;
} Header;

typedef struct {
  // Start-line
  char *method;
  char *path;
  char *version;

  Header headers[MAX_REQUEST_HEADERS];
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

// Logic for how to handle incoming client
void handle_client(int cfd) {
  char buf[MAX_REQUEST_SIZE];
  char *headers = NULL;
  char *body = NULL;

  HttpRequest http_request = {.method = NULL, .path = NULL, .version = NULL};

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
      http_request.path = space + 1;

      char *space2 = strstr(http_request.path, " ");
      if (space2 == NULL) {
        fprintf(stderr, "handle_client: Invalid start-line format");
        close(cfd);
        return;
      }
      *space2 = '\0';
      http_request.version = space2 + 1;
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
        value = trim(value);

        for (int i = 0; i < strlen(rest); i++) {
          rest[i] = tolower(rest[i]);
        }

        if (header_len >= MAX_REQUEST_HEADERS) {
          fprintf(stderr,
                  "handle_client: Exceeded limit of MAX_REQUEST_HEADERS");
          close(cfd);
          return;
        }

        Header header = {.key = rest, .value = value};
        http_request.headers[header_len] = header;
        header_len++;

        printf("%s: %s\n", rest, value);
        if (strcmp(rest, "content-length") == 0) {
          errno = 0;
          content_len = strtol(value, NULL, 10);
          if (errno != 0) {
            perror("strtol");
            close(cfd);
            return;
          }
        }

        if (fcrlf == NULL) {
          break;
        }
        rest = fcrlf + 2;
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
