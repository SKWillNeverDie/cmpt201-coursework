#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_MSG_LEN 1024
#define MAX_CLIENTS 256

struct client_info {
  int sock;
  struct sockaddr_in addr;
  int finished;
};

static struct client_info clients[MAX_CLIENTS];
static size_t client_count = 0;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t finished_count = 0;
static int expected_clients = 0;
static int server_listen_sock = -1;

static void broadcast_raw(const char *buf, size_t len) {
  pthread_mutex_lock(&clients_lock);
  for (size_t i = 0; i < client_count; ++i) {
    if (clients[i].sock >= 0) {
      ssize_t off = 0;
      while (off < (ssize_t)len) {
        ssize_t w = send(clients[i].sock, buf + off, len - off, 0);
        if (w <= 0) {
          if (errno == EINTR)
            continue;
          break;
        }
      }
    }
  }
  pthread_mutex_unlock(&clients_lock);
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

static void remove_client_by_index(size_t idx) {
  if (idx >= client_count)
    return;
  close(clients[idx].sock);
  for (size_t j = idx; j + 1 < client_count; ++j) {
    clients[j] = clients[j + 1];
  }
  client_count--;
}

static void *client_handler(void *arg) {
  int sock = *(int *)arg;
  free(arg);
  char buf[1 + 4 + 2 + MAX_MSG_LEN + 4];

  size_t my_index = SIZE_MAX;
  struct sockaddr_in my_addr = {0};

  pthread_mutex_lock(&clients_lock);
  for (size_t i = 0; i < client_count; ++i) {
    if (clients[i].sock == sock) {
      my_index = i;
      my_addr = clients[i].addr;
      break;
    }
  }
  pthread_mutex_unlock(&clients_lock);

  if (my_index == SIZE_MAX) {
    close(sock);
    return NULL;
  }

  while (1) {
    ssize_t n = recv_until_nl(sock, buf, sizeof(buf));
    if (n <= 0)
      break;

    uint8_t type = (uint8_t)buf[0];
    if (type != 0 && type != 1) {
      if (buf[0] == '0' || buf[0] == '1')
        type = (uint8_t)(buf[0] - '0');
    }

    if (type == 0) {
      uint32_t ip_net = my_addr.sin_addr.s_addr;
      uint16_t port_net = htons((uint16_t)ntohs(my_addr.sin_port));
      size_t payload_len = (size_t)(n - 1);

      char outbuf[1 + 4 + 2 + MAX_MSG_LEN + 2];
      size_t pos = 0;
      outbuf[pos++] = (char)0; // type 0
      memcpy(outbuf + pos, &ip_net, 4);
      pos += 4;
      memcpy(outbuf + pos, &port_net, 2);
      pos += 2;
      if (payload_len > MAX_MSG_LEN)
        payload_len = MAX_MSG_LEN;
      memcpy(outbuf + pos, buf + 1, payload_len);
      pos += payload_len;
      if (outbuf[pos - 1] != '\n') {
        outbuf[pos++] = '\n';
      }

      broadcast_raw(outbuf, pos);
    } else if (type == 1) {
      uint8_t t = 1;
      send(sock, &t, 1, 0);
      send(sock, "\n", 1, 0);

      pthread_mutex_lock(&clients_lock);
      if (my_index < client_count && clients[my_index].sock == sock &&
          !clients[my_index].finished) {
        clients[my_index].finished = 1;
        finished_count++;
      }
      size_t finished = finished_count;
      size_t expected = expected_clients;
      pthread_mutex_unlock(&clients_lock);

      if (finished >= expected && expected > 0) {
        char endmsg[2] = {(char)1, '\n'};
        broadcast_raw(endmsg, 2);
        if (server_listen_sock >= 0) {
          shutdown(server_listen_sock, SHUT_RD);
          close(server_listen_sock);
          server_listen_sock = -1;
        }

        pthread_exit(NULL);
      } else {
      }
    }
  }

  pthread_mutex_lock(&clients_lock);
  size_t idx = SIZE_MAX;
  for (size_t i = 0; i < client_count; ++i) {
    if (clients[i].sock == sock) {
      idx = i;
      break;
    }
  }
  if (idx != SIZE_MAX)
    remove_client_by_index(idx);
  pthread_mutex_unlock(&clients_lock);

  close(sock);
  return NULL;
}

static int make_listener(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return -1;
  int opt = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(s);
    return -1;
  }
  if (listen(s, 128) < 0) {
    close(s);
    return -1;
  }
  return s;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <port> <#clients>\n", argv[0]);
    return 1;
  }
  int port = atoi(argv[1]);
  expected_clients = atoi(argv[2]);
  if (expected_clients < 1 || expected_clients > MAX_CLIENTS) {
    fprintf(stderr, "invalid #clients\n");
    return 1;
  }

  server_listen_sock = make_listener(port);
  if (server_listen_sock < 0) {
    perror("bind/listen");
    return 1;
  }

  while (1) {
    if ((int)client_count >= expected_clients)
      break;

    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int client_sock =
        accept(server_listen_sock, (struct sockaddr *)&cli_addr, &cli_len);
    if (client_sock < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    pthread_mutex_lock(&clients_lock);
    if (client_count < MAX_CLIENTS) {
      clients[client_count].sock = client_sock;
      clients[client_count].addr = cli_addr;
      clients[client_count].finished = 0;
      client_count++;
    } else {
      close(client_sock);
      pthread_mutex_unlock(&clients_lock);
      continue;
    }
    pthread_mutex_unlock(&clients_lock);

    pthread_t t;
    int *psock = malloc(sizeof(int));
    *psock = client_sock;
    pthread_create(&t, NULL, client_handler, psock);
    pthread_detach(t);
  }

  while (1) {
    pthread_mutex_lock(&clients_lock);
    size_t finished = finished_count;
    size_t expected = expected_clients;
    pthread_mutex_unlock(&clients_lock);
    if (finished >= expected)
      break;
    usleep(10000);
  }

  {
    char endmsg[2] = {(char)1, '\n'};
    broadcast_raw(endmsg, 2);
  }

  pthread_mutex_lock(&clients_lock);
  for (size_t i = 0; i < client_count; ++i) {
    if (clients[i].sock >= 0) {
      close(clients[i].sock);
      clients[i].sock = -1;
    }
  }
  pthread_mutex_unlock(&clients_lock);

  if (server_listen_sock >= 0) {
    close(server_listen_sock);
    server_listen_sock = -1;
  }
  return 0;
}
