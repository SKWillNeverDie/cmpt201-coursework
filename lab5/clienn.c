#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_MSG_LEN 1024

struct client_args {
  const char *host;
  int port;
  int nmsgs;
  const char *logpath;
  int sock;
  FILE *logf;
  pthread_t sender;
  pthread_t receiver;
  int done_sending;
};

static int open_connection(const char *host, int port) {
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);
  if (getaddrinfo(host, portstr, &hints, &res) != 0)
    return -1;
  int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s < 0) {
    freeaddrinfo(res);
    return -1;
  }

  int opt = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
    close(s);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  return s;
}

static int convert_hex(const uint8_t *buf, ssize_t buf_size, char *str,
                       ssize_t str_size) {
  if (!buf || !str || buf_size <= 0 || str_size < buf_size * 2 + 1)
    return -1;
  for (ssize_t i = 0; i < buf_size; ++i) {
    sprintf(str + i * 2, "%02X", buf[i]);
  }
  str[buf_size * 2] = '\0';
  return 0;
}

static int send_message(int sock, uint8_t type, const char *payload,
                        size_t len) {
  size_t total = 1 + len + 1;
  if (total > (1 + MAX_MSG_LEN + 1))
    return -1;
  char buf[1 + MAX_MSG_LEN + 1];
  buf[0] = (char)type;
  if (len > 0)
    memcpy(buf + 1, payload, len);
  buf[1 + len] = '\n';
  ssize_t off = 0;
  while (off < (ssize_t)total) {
    ssize_t w = send(sock, buf + off, total - off, 0);
    if (w <= 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    off += w;
  }
  return 0;
}

static void *sender_thread(void *arg) {
  struct client_args *c = arg;
  for (int i = 0; i < c->nmsgs; ++i) {
    uint8_t rnd[16];
    if (getentropy(rnd, sizeof(rnd)) != 0) {
      FILE *f = fopen("/dev/urandom", "rb");
      if (!f)
        break;
      fread(rnd, 1, sizeof(rnd), f);
      fclose(f);
    }
    char hex[sizeof(rnd) * 2 + 1];
    convert_hex(rnd, sizeof(rnd), hex, sizeof(hex));
    if (send_message(c->sock, 0, hex, strlen(hex)) != 0)
      break;
    usleep(1000);
  }
  send_message(c->sock, 1, "", 0);
  c->done_sending = 1;
  return NULL;
}

static ssize_t recv_until_nl(int fd, char *buf, size_t maxlen) {
  size_t off = 0;
  while (off < maxlen - 1) {
    ssize_t r = recv(fd, buf + off, 1, 0);
    if (r == 0)
      return 0;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    off += 1;
    if (buf[off - 1] == '\n')
      break;
  }
  buf[off] = '\0';
  return (ssize_t)off;
}

static void *receiver_thread(void *arg) {
  struct client_args *c = arg;
  char buf[1 + 4 + 2 + MAX_MSG_LEN + 4];
  while (1) {
    ssize_t n = recv_until_nl(c->sock, buf, sizeof(buf));
    if (n <= 0)
      break;
    uint8_t type = (uint8_t)buf[0];
    if (type != 0 && type != 1) {
      if (buf[0] == '0' || buf[0] == '1')
        type = buf[0] - '0';
    }
    if (type == 0) {
      if (n < 1 + 4 + 2)
        continue;
      uint32_t ip_net;
      uint16_t port_net;
      memcpy(&ip_net, buf + 1, 4);
      memcpy(&port_net, buf + 1 + 4, 2);
      char ipstr[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &ip_net, ipstr, sizeof(ipstr));
      uint16_t port = ntohs(port_net);
      char *payload = buf + 1 + 4 + 2;

      size_t payload_len = n - (1 + 4 + 2) - 1;
      printf("%-15s%-10u%.*s\n", ipstr, (unsigned)port, (int)payload_len,
             payload);
      if (c->logf) {
        fprintf(c->logf, "%-15s%-10u%.*s\n", ipstr, (unsigned)port,
                (int)payload_len, payload);
        fflush(c->logf);
      }
    } else if (type == 1) {
      break;
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: %s <IP> <port> <#messages> <logfile>\n", argv[0]);
    return 1;
  }
  struct client_args c = {0};
  c.host = argv[1];
  c.port = atoi(argv[2]);
  c.nmsgs = atoi(argv[3]);
  c.logpath = argv[4];

  int sock = open_connection(c.host, c.port);
  if (sock < 0) {
    perror("connect");
    return 1;
  }
  c.sock = sock;

  c.logf = fopen(c.logpath, "w");
  if (!c.logf) {
    perror("log");
    close(sock);
    return 1;
  }

  pthread_create(&c.receiver, NULL, receiver_thread, &c);
  pthread_create(&c.sender, NULL, sender_thread, &c);

  pthread_join(c.sender, NULL);
  pthread_join(c.receiver, NULL);

  fclose(c.logf);
  close(sock);
  return 0;
}
