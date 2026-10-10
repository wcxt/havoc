#include "ctype.h"
#include "netinet/in.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/socket.h"
#include "unistd.h"
#include <assert.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_REQUEST_SIZE 65536

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

int main(int argc, char *argv[]) {
  // Opens an endpoint/file descriptor for communication.
  // 0 chooses TCP protocol automatically
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  // man 7 ip for info
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(8000),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

  int result = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
  if (result != 0) {
    return -1;
  }

  printf("PID: %d | Socket FD: %d\n", getpid(), fd);
  result = listen(fd, SOMAXCONN);
  if (result != 0) {
    return -1;
  }

  // Basic TCP accept loop
  // accept -> read data -> process data -> close connection
  while (1) {
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
      return -1;
    }

    // TODO: Handle unreliable read
    // e.g. When read returns only half of http message
    char buf[MAX_REQUEST_SIZE];
    char *headers = NULL;
    char *body = NULL;

    char *method = NULL;
    char *path = NULL;
    char *version = NULL;
    long content_len = 0;
    int total_n = 0;

    while (1) {
      ssize_t n = read(cfd, buf + total_n, (MAX_REQUEST_SIZE - total_n) - 1);
      if (n <= 0) {
        break;
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
        method = buf;
        char *space = strstr(buf, " ");
        if (space == NULL) {
          return -1;
          close(cfd);
        }
        *space = '\0';
        path = space + 1;

        char *space2 = strstr(path, " ");
        if (space2 == NULL) {
          return -1;
          close(cfd);
        }
        *space2 = '\0';
        version = space2 + 1;
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

        while ((fcrlf = strstr(rest, "\r\n"))) {
          *fcrlf = '\0';

          char *value = NULL;

          char *colon = strstr(rest, ":");
          if (colon == NULL) {
            close(cfd);
            return -1;
          }
          *colon = '\0';
          value = colon + 1;
          value = trim(value);

          for (int i = 0; i < strlen(rest); i++) {
            rest[i] = tolower(rest[i]);
          }

          printf("%s: %s\n", rest, value);
          if (strcmp(rest, "content-length") == 0) {
            content_len = strtol(value, NULL, 10);
          }
          rest = fcrlf + 2;
        }

        // Final header
        char *value = NULL;

        char *colon = strstr(rest, ":");
        if (colon == NULL) {
          close(cfd);
          return -1;
        }
        *colon = '\0';
        value = colon + 1;
        value = trim(value);
        for (int i = 0; i < strlen(rest); i++) {
          rest[i] = tolower(rest[i]);
        }

        printf("%s: %s\n", rest, value);
        if (strcmp(rest, "content-length") == 0) {
          content_len = strtol(value, NULL, 10);
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

    // char *headers = NULL;
    // char *body = NULL;
    //
    // // separate start-line from rest
    // // zero-copy version
    // char *crlf = strstr(buf, "\r\n");
    // if (crlf == NULL) {
    //   return -1;
    //   close(cfd);
    // }
    // *crlf = '\0';
    // headers = crlf + 2;
    //
    // // Parse start-line
    // char *method = NULL;
    // char *path = NULL;
    // char *version = NULL;
    //
    // method = buf;
    //
    // char *space = strstr(buf, " ");
    // if (space == NULL) {
    //   return -1;
    //   close(cfd);
    // }
    // *space = '\0';
    // path = space + 1;
    //
    // char *space2 = strstr(path, " ");
    // if (space2 == NULL) {
    //   return -1;
    //   close(cfd);
    // }
    // *space2 = '\0';
    // version = space2 + 1;
    //
    printf("Method:%s\n", method);
    printf("Path:%s\n", path);
    printf("Version:%s\n", version);
    printf("Body:%s\n\n", body);
    //
    // char *hcrlf = strstr(headers, "\r\n\r\n");
    // if (hcrlf == NULL) {
    //   return -1;
    //   close(cfd);
    // }
    // *hcrlf = '\0';
    // body = hcrlf + 4;
    //
    // // Parse headers
    // char *fcrlf = NULL;
    // char *rest = headers;
    //
    // while ((fcrlf = strstr(rest, "\r\n"))) {
    //   *fcrlf = '\0';
    //
    //   char *value = NULL;
    //
    //   char *colon = strstr(rest, ":");
    //   if (colon == NULL) {
    //     close(cfd);
    //     return -1;
    //   }
    //   *colon = '\0';
    //   value = colon + 1;
    //   value = trim(value);
    //   printf("%s: %s\n", rest, value);
    //
    //   rest = fcrlf + 2;
    // }
    //
    // // Final header
    // char *value = NULL;
    //
    // char *colon = strstr(rest, ":");
    // if (colon == NULL) {
    //   close(cfd);
    //   return -1;
    // }
    // *colon = '\0';
    // value = colon + 1;
    // value = trim(value);
    // printf("%s: %s\n", rest, value);
    //
    // printf("%s END\n", body);
    //
    close(cfd);
  }
  sleep(60);
  return 0;
}
