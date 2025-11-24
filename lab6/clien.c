// client.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_MSG_SIZE 1024

// convert random bytes to hex string (provided in prompt)
int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size) {
  if (buf == NULL || str == NULL || buf_size <= 0 ||
      str_size < (buf_size * 2 + 1)) {
    return -1;
  }

  for (int i = 0; i < buf_size; i++)
    sprintf(str + i * 2, "%02X", buf[i]);
  str[buf_size * 2] = '\0';

  return 0;
}

// helper: try getentropy, fallback to /dev/urandom
int get_random_bytes(uint8_t *buf, size_t n) {
#ifdef SYS_getrandom
  // prefer getentropy if available
#endif
  // try getentropy if present
#if defined(__linux__) || defined(__APPLE__)
  // getentropy(2) typically limited to 256 bytes per call on Linux; let's use
  // getrandom or read from /dev/urandom
#endif
  // attempt getentropy if declared
#if defined(__GLIBC__) || defined(__linux__) || defined(__APPLE__)
  extern int getentropy(void *, size_t) __attribute__((weak));
  if (getentropy) {
    if (getentropy(buf, n) == 0)
      return 0;
  }
#endif
  // fallback: read /dev/urandom
  FILE *f = fopen("/dev/urandom", "rb");
  if (!f)
    return -1;
  size_t r = fread(buf, 1, n, f);
  fclose(f);
  return (r == n) ? 0 : -1;
}

int sockfd;
int messages_to_send;
char *log_prefix;
FILE *logfile;
pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

ssize_t robust_send(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t left = len;
  while (left > 0) {
    ssize_t n = send(fd, p, left, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    left -= n;
    p += n;
  }
  return (ssize_t)len;
}

void *receiver_thread(void *arg) {
  (void)arg;
  // read messages from server, parse type
  char buf[4096];
  size_t used = 0;
  while (1) {
    ssize_t n = recv(sockfd, buf + used, sizeof(buf) - used, 0);
    if (n <= 0) {
      if (n == 0)
        break;
      if (errno == EINTR)
        continue;
      break;
    }
    used += (size_t)n;
    // process complete messages ended by '\n'
    size_t pos = 0;
    while (pos < used) {
      if (pos + 1 > used)
        break;
      uint8_t type = (uint8_t)buf[pos];
      // find newline
      size_t i = pos + 1;
      int found = 0;
      for (; i < used; ++i) {
        if (buf[i] == '\n') {
          found = 1;
          break;
        }
      }
      if (!found)
        break;
      size_t msglen = i - (pos + 1) + 1; // includes '\n'
      if (type == 0) {
        // need at least 6 bytes for ip+port
        if (msglen < 6) {
          // malformed; skip
        } else {
          uint32_t ipnet;
          uint16_t portnet;
          memcpy(&ipnet, buf + pos + 1, 4);
          memcpy(&portnet, buf + pos + 5, 2);
          size_t chatlen = msglen - 6;
          char chat[MAX_MSG_SIZE + 1];
          if (chatlen > MAX_MSG_SIZE)
            chatlen = MAX_MSG_SIZE;
          memcpy(chat, buf + pos + 7, chatlen);
          chat[chatlen] = '\0';
          // remove trailing newline if present
          if (chatlen > 0 && chat[chatlen - 1] == '\n')
            chat[chatlen - 1] = '\0';
          struct in_addr ina;
          ina.s_addr = ipnet;
          char ipstr[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &ina, ipstr, sizeof(ipstr));
          uint16_t port = ntohs(portnet);
          // log: "%-15s%-10u%s"
          pthread_mutex_lock(&log_mtx);
          if (logfile) {
            fprintf(logfile, "%-15s%-10u%s\n", ipstr, (unsigned)port, chat);
            fflush(logfile);
          } else {
            printf("%-15s%-10u%s\n", ipstr, (unsigned)port, chat);
            fflush(stdout);
          }
          pthread_mutex_unlock(&log_mtx);
        }
      } else if (type == 1) {
        // server end-of-execution: exit
        return NULL;
      } else {
        // ignore unknown
      }
      pos = i + 1;
    }
    // shift remaining
    if (pos < used) {
      memmove(buf, buf + pos, used - pos);
      used -= pos;
    } else {
      used = 0;
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: %s <IP> <port> <# of messages> <log file path>\n",
            argv[0]);
    return EXIT_FAILURE;
  }
  char *ip = argv[1];
  int port = atoi(argv[2]);
  messages_to_send = atoi(argv[3]);
  char *logpath = argv[4];

  // open log file
  logfile = fopen(logpath, "w");
  if (!logfile) {
    perror("fopen log");
    return EXIT_FAILURE;
  }

  // connect
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &sin.sin_addr) <= 0) {
    perror("inet_pton");
    return EXIT_FAILURE;
  }
  if (connect(sockfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    perror("connect");
    return EXIT_FAILURE;
  }

  // start receiver thread
  pthread_t rth;
  pthread_create(&rth, NULL, receiver_thread, NULL);

  // send messages
  for (int i = 0; i < messages_to_send; ++i) {
    // generate ~16 random bytes -> 32 hex chars
    uint8_t rnd[16];
    if (get_random_bytes(rnd, sizeof(rnd)) != 0) {
      fprintf(stderr, "random failure\n");
      break;
    }
    char hex[16 * 2 + 1];
    if (convert(rnd, sizeof(rnd), hex, sizeof(hex)) != 0) {
      fprintf(stderr, "convert failure\n");
      break;
    }
    // build message: [1 byte type=0][hex string][\n]
    size_t payload_len = strlen(hex) + 1; // include '\n'
    if (payload_len > MAX_MSG_SIZE)
      payload_len = MAX_MSG_SIZE;
    size_t total = 1 + payload_len;
    char *buf = malloc(total);
    if (!buf)
      break;
    buf[0] = 0;
    memcpy(buf + 1, hex, payload_len - 1);
    buf[total - 1] = '\n';
    if (robust_send(sockfd, buf, total) < 0) {
      free(buf);
      break;
    }
    free(buf);
    usleep(1000); // small delay to avoid blasting (not required)
  }

  // send type 1 to server indicating done
  char endmsg[2] = {1, '\n'};
  robust_send(sockfd, endmsg, sizeof(endmsg));

  // wait for receiver to get type1 from server (or until socket closes)
  pthread_join(rth, NULL);

  fclose(logfile);
  close(sockfd);
  return EXIT_SUCCESS;
}
