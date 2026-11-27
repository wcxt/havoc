#include "netinet/in.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/socket.h"
#include "unistd.h"
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stddef.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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
  case 408:
    return "Request Timeout";
  case 413:
    return "Payload Too Large";
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

void handle_request(int fd, HttpRequest *http_request, char *dir_path) {
  char *rest = strpbrk(http_request->target, "?#");
  if (rest != NULL) {
    *rest = '\0';
  }

  // Resource path
  char full_path[MAX_URI_LENGTH + PATH_MAX + sizeof("/index.html")];
  sprintf(full_path, "%s%s", dir_path, http_request->target);

  // Transform / into index.html
  if (http_request->target[strlen(http_request->target) - 1] == '/') {
    strcat(full_path, "index.html");
  }

  char actualpath[PATH_MAX];
  if (realpath(full_path, actualpath) == NULL) {
    dprintf(fd,
            "HTTP/1.1 404 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            get_http_code_message(404));
    return;
  }

  if (!(strcmp(dir_path, "/") == 0)) {
    if (strncmp(actualpath, dir_path, strlen(dir_path)) != 0 ||
        (actualpath[strlen(dir_path)] != '\0' &&
         actualpath[strlen(dir_path)] != '/')) {
      dprintf(
          fd,
          "HTTP/1.1 404 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
          get_http_code_message(404));
      return;
    }
  }

  int rfd = open(actualpath, O_RDONLY);
  if (rfd < 0) {
    if (errno == ENOENT) {
      dprintf(
          fd,
          "HTTP/1.1 404 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
          get_http_code_message(404));
    } else {
      perror("open");
      dprintf(
          fd,
          "HTTP/1.1 500 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
          get_http_code_message(500));
    }
    return;
  }

  // Get content length from stats
  struct stat st;
  int ret = fstat(rfd, &st);
  if (ret < 0) {
    perror("fstat");
    close(rfd);
    dprintf(fd,
            "HTTP/1.1 500 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            get_http_code_message(500));
    return;
  }

  // Only regular files are sendable
  if (!S_ISREG(st.st_mode)) {
    close(rfd);
    dprintf(fd,
            "HTTP/1.1 404 %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            get_http_code_message(404));
    return;
  }

  // Get mimetype
  char *mimetype = get_mimetype(actualpath);

  // Send initial response data (startline + headers)
  dprintf(fd,
          "HTTP/1.1 200 %s\r\nContent-Length: %ld\r\nContent-Type: %s; "
          "charset=utf-8\r\nConnection: Close\r\n\r\n",
          get_http_code_message(200), st.st_size, mimetype);

  if (strcmp(http_request->method, "HEAD") == 0) {
    close(rfd);
    return;
  }

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

// read request reads data from fd to the buffer of specified
// capacity and writes length of read data to the len pointer.
// read request also append null byte to buffer.
// It reads data until either \r\n\r\n terminator.
// It will also return error when capacity was excedded.
//
// Additionally read_request can fail on either timeoout from read operation
// Or reading request after the deadline has been exceeded
int read_request(int fd, char *buffer, int cap, int *len) {
  time_t deadline = time(NULL);
  deadline += 8;

  int read_bytes = 0;
  while (1) {

    if (read_bytes >= cap - 1) {
      return 413;
    }

    time_t current = time(NULL);
    double diff = difftime(deadline, current);
    if (diff <= 0) {
      return 408;
    }

    ssize_t n = read(fd, buffer + read_bytes, cap - read_bytes - 1);
    if (n == 0) {
      // Client aborted sending request
      return -1;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 408;
      }
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      return -1;
    }

    int prev = read_bytes;
    read_bytes += n;
    buffer[read_bytes] = '\0';

    // prevents issue where separator would be split between different reads
    int from = prev > 3 ? prev - 3 : 0;
    char *end = strstr(buffer + from, "\r\n\r\n");
    if (end != NULL) {
      *len = read_bytes;
      break;
    }
  }

  return 0;
}

// Logic for how to handle incoming client
void handle_client(int cfd, char *dir_path) {
  char req_buf[MAX_REQUEST_SIZE];
  int req_len = 0;

  HttpRequest http_request = {
      .method = NULL, .target = NULL, .version = NULL, .headers.len = 0};

  int ret = read_request(cfd, req_buf, MAX_REQUEST_SIZE, &req_len);
  if (ret != 0) {
    if (ret > 0) {
      dprintf(
          cfd,
          "HTTP/1.1 %d %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
          ret, get_http_code_message(ret));
    }
    close(cfd);
    return;
  }
  // Full request buffer ready cut at

  char *startline_end = strstr(req_buf, "\r\n");
  if (startline_end == NULL) {
    dprintf(cfd,
            "HTTP/1.1 %d %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            400, get_http_code_message(400));
    close(cfd);
    return;
  }
  *startline_end = '\0';
  // Startline becomes cut by inserting \0 at \r\n

  ret = parse_start_line(req_buf, &http_request);
  if (ret > 0) {
    dprintf(cfd,
            "HTTP/1.1 %d %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            ret, get_http_code_message(ret));
    close(cfd);
    return;
  }
  // Parse startline results into http requests

  char *headers_start = startline_end + 2;
  char *headers_end = strstr(headers_start, "\r\n\r\n");

  if (headers_end != NULL) {
    headers_end += 2;
  } else if (headers_start[1] == '\n' && headers_start[0] == '\r') {
    headers_end = headers_start;
  } else {
    dprintf(cfd,
            "HTTP/1.1 %d %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            500, get_http_code_message(500));
    close(cfd);
    return;
  }
  *headers_end = '\0';

  ret = parse_headers(headers_start, &http_request);
  if (ret > 0) {
    dprintf(cfd,
            "HTTP/1.1 %d %s\r\nConnection: Close\r\nContent-Length: 0\r\n\r\n",
            ret, get_http_code_message(ret));
    close(cfd);
    return;
  }

  handle_request(cfd, &http_request, dir_path);
  close(cfd);
  return;
}

int main(int argc, char *argv[]) {
  long int port = -1;
  char directory[PATH_MAX];
  directory[0] = '\0';

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
      if (port != -1) {
        fprintf(stderr, "duplicate -p / --port flag\n");
        return EXIT_FAILURE;
      }

      if (argc == i + 1) {
        fprintf(stderr, "missing value for flag -p / --port\n");
        return EXIT_FAILURE;
      }
      i++;

      char *endptr;
      errno = 0;
      long int new_port = strtol(argv[i], &endptr, 10);
      if (endptr == argv[i] || *endptr != '\0') {
        fprintf(stderr, "flag -p / --port value is not a valid digit\n");
        return EXIT_FAILURE;
      }
      if (errno != 0) {
        perror("flag -p / --port value conversion");
        return EXIT_FAILURE;
      }
      if (new_port < 0 || new_port > 65535) {
        fprintf(stderr, "flag -p / --port value is out of range\n");
        return EXIT_FAILURE;
      }
      port = new_port;
      continue;
    }
    if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--directory") == 0) {
      if (directory[0] != '\0') {
        fprintf(stderr, "duplicate -d / --directory flag\n");
        return EXIT_FAILURE;
      }

      if (argc == i + 1) {
        fprintf(stderr, "missing value for flag -d / --directory\n");
        return EXIT_FAILURE;
      }
      i++;

      char temp_path[PATH_MAX];
      if (realpath(argv[i], temp_path) == NULL) {
        perror("flag -d / --directory value");
        return EXIT_FAILURE;
      }

      struct stat sb;
      int ret = stat(temp_path, &sb);
      if (ret < 0) {
        perror("flag -d / --directory value");
        return EXIT_FAILURE;
      }
      if (!S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "flag -d / --directory value is not a directory\n");
        return EXIT_FAILURE;
      }

      snprintf(directory, sizeof(directory), "%s", temp_path);
      continue;
    }
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printf("\033[1mUsage:\033[0m %s -d <directory> [-p <port>]\n"
             "\n"
             "A simple HTTP server that serves static files from a "
             "directory.\n"
             "\n"
             "\033[1mOptions:\033[0m\n"
             "  -d, --directory <path>  Directory to serve files from "
             "(required).\n"
             "  -p, --port <port>       TCP port to listen on, 0-65535 "
             "(default: 8080).\n"
             "  -h, --help              Show this help message and exit.\n",
             argv[0]);
      return EXIT_SUCCESS;
    }
    fprintf(stderr, "unknown argument / flag: %s\n", argv[i]);
    return EXIT_FAILURE;
  }

  if (port == -1) {
    port = 8080;
  }
  if (directory[0] == '\0') {
    fprintf(stderr, "flag -d / --directory is required\n");
    return EXIT_FAILURE;
  }

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
                             .sin_port = htons(port),
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

  // Basic TCP accept loopq
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
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    handle_client(cfd, directory);
  }
  close(fd);
  return EXIT_SUCCESS;
}
