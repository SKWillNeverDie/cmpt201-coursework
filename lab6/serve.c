// server.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_MSG_SIZE 1024
#define BACKLOG 16

typedef struct client {
  int fd;
  struct sockaddr_in addr;
  int id;
  int sent_type1; // whether this client sent type 1
  struct client *next;
} client_t;

typedef struct queued_msg {
  uint8_t type;  // 0 or 1
  int sender_fd; // >=0 => send only to this fd; -1 => broadcast to all (global
                 // commit)
  uint32_t ip;   // network byte order (used for type 0)
  uint16_t port; // network byte order (used for type 0)
  size_t payload_len;
  char payload[MAX_MSG_SIZE];
  struct queued_msg *next;
} queued_msg_t;

// Globals
client_t *clients = NULL;
pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;
queued_msg_t *q_head = NULL, *q_tail = NULL;
pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;

int expected_clients = 0;
int received_type1_count = 0;
int server_running = 1;
int listen_fd = -1;

void add_client(client_t *c) {
  pthread_mutex_lock(&clients_mtx);
  c->next = clients;
  clients = c;
  pthread_mutex_unlock(&clients_mtx);
}

void remove_client_by_fd(int fd) {
  pthread_mutex_lock(&clients_mtx);
  client_t **pp = &clients;
  while (*pp) {
    if ((*pp)->fd == fd) {
      client_t *to = *pp;
      *pp = to->next;
      close(to->fd);
      free(to);
      break;
    }
    pp = &(*pp)->next;
  }
  pthread_mutex_unlock(&clients_mtx);
}

int client_fd_exists(int fd) {
  int exists = 0;
  pthread_mutex_lock(&clients_mtx);
  client_t *it = clients;
  while (it) {
    if (it->fd == fd) {
      exists = 1;
      break;
    }
    it = it->next;
  }
  pthread_mutex_unlock(&clients_mtx);
  return exists;
}

void enqueue_msg(queued_msg_t *m) {
  pthread_mutex_lock(&q_mtx);
  m->next = NULL;
  if (!q_tail)
    q_head = q_tail = m;
  else {
    q_tail->next = m;
    q_tail = m;
  }
  pthread_cond_signal(&q_cv);
  pthread_mutex_unlock(&q_mtx);
}

queued_msg_t *dequeue_msg() {
  pthread_mutex_lock(&q_mtx);
  while (!q_head && server_running) {
    pthread_cond_wait(&q_cv, &q_mtx);
  }
  queued_msg_t *m = q_head;
  if (m) {
    q_head = m->next;
    if (!q_head)
      q_tail = NULL;
  }
  pthread_mutex_unlock(&q_mtx);
  return m;
}

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

// broadcaster thread: takes messages from queue and sends to clients in order
void *broadcaster(void *arg) {
  (void)arg;
  while (server_running) {
    queued_msg_t *m = dequeue_msg();
    if (!m)
      continue;

    if (m->type == 0) {
      // type 0: broadcast to all clients: [1 byte type=0][4 bytes ip][2 bytes
      // port][payload up to '\n']
      size_t total =
          1 + 4 + 2 + m->payload_len; // payload already includes trailing '\n'
      char *buf = malloc(total);
      if (!buf) {
        free(m);
        continue;
      }
      buf[0] = 0;
      memcpy(buf + 1, &m->ip, 4);
      memcpy(buf + 5, &m->port, 2);
      memcpy(buf + 7, m->payload, m->payload_len);

      pthread_mutex_lock(&clients_mtx);
      client_t *c = clients;
      while (c) {
        if (robust_send(c->fd, buf, total) < 0) {
          int fd = c->fd;
          c = c->next;
          remove_client_by_fd(fd);
          continue;
        }
        c = c->next;
      }
      pthread_mutex_unlock(&clients_mtx);
      free(buf);

    } else if (m->type == 1) {
      // Two possible semantics:
      //  - If sender_fd >= 0: echo back to that client only, then close that
      //  client.
      //  - If sender_fd == -1: broadcast to all and shutdown server.
      if (m->sender_fd == -1) {
        // global broadcast and shutdown
        char buf[2] = {1, '\n'};
        pthread_mutex_lock(&clients_mtx);
        client_t *c = clients;
        while (c) {
          if (robust_send(c->fd, buf, 2) < 0) {
            int fd = c->fd;
            c = c->next;
            remove_client_by_fd(fd);
            continue;
          }
          c = c->next;
        }
        pthread_mutex_unlock(&clients_mtx);

        // shutdown server
        server_running = 0;
        if (listen_fd >= 0)
          close(listen_fd);

      } else if (m->sender_fd >= 0) {
        // echo only to the sender (if still present)
        int fd = m->sender_fd;
        char buf[2] = {1, '\n'};
        if (client_fd_exists(fd)) {
          if (robust_send(fd, buf, 2) < 0) {
            // send failed: remove that client entry
            remove_client_by_fd(fd);
          } else {
            // successful echo — now close that client
            remove_client_by_fd(fd);
          }
        } else {
          // client already gone; nothing to do
        }
      }
    }

    free(m);
  }
  return NULL;
}

void handle_client_read(client_t *c) {
  // read until '\n' for each message; handle partial reads
  char buf[MAX_MSG_SIZE + 16];
  size_t bufused = 0;
  while (1) {
    ssize_t n = recv(c->fd, buf + bufused, sizeof(buf) - bufused, 0);
    if (n <= 0) {
      // client closed or error
      remove_client_by_fd(c->fd);
      return;
    }
    bufused += (size_t)n;
    // process all complete messages in buffer
    size_t pos = 0;
    while (pos < bufused) {
      // need at least 1 byte for type
      if (pos + 1 > bufused)
        break;
      uint8_t type = (uint8_t)buf[pos];
      // find '\n' from pos+1 onwards
      size_t i = pos + 1;
      int found = 0;
      for (; i < bufused; ++i) {
        if (buf[i] == '\n') {
          found = 1;
          break;
        }
      }
      if (!found)
        break;                           // wait for more data
      size_t msglen = i - (pos + 1) + 1; // includes '\n'
      if (type == 0) {
        // payload is from pos+1 to i inclusive
        if (msglen > MAX_MSG_SIZE)
          msglen = MAX_MSG_SIZE; // truncate if necessary
        queued_msg_t *qm = calloc(1, sizeof(queued_msg_t));
        if (!qm) {
          pos = i + 1;
          continue;
        }
        qm->type = 0;
        qm->sender_fd = -2;               // unused for type 0
        qm->ip = c->addr.sin_addr.s_addr; // network order
        qm->port = c->addr.sin_port;      // network order
        qm->payload_len = msglen;
        memcpy(qm->payload, buf + pos + 1, msglen);
        enqueue_msg(qm);

      } else if (type == 1) {
        // client signals it's done sending
        pthread_mutex_lock(&clients_mtx);
        if (!c->sent_type1) {
          c->sent_type1 = 1;
          received_type1_count++;
        }
        int done = (received_type1_count >= expected_clients);
        pthread_mutex_unlock(&clients_mtx);

        // enqueue per-client echo type-1 so broadcaster will echo and close
        // this client
        queued_msg_t *qm_echo = calloc(1, sizeof(queued_msg_t));
        if (qm_echo) {
          qm_echo->type = 1;
          qm_echo->sender_fd = c->fd; // echo to this client only
          enqueue_msg(qm_echo);
        }

        // if all clients signalled, enqueue a global type-1 broadcast
        // (sender_fd = -1)
        if (done) {
          queued_msg_t *qm_global = calloc(1, sizeof(queued_msg_t));
          if (qm_global) {
            qm_global->type = 1;
            qm_global->sender_fd = -1; // indicate global broadcast + shutdown
            enqueue_msg(qm_global);
          }
        }
        // IMPORTANT: do NOT close the client socket here. Let broadcaster echo
        // and then remove it.
      } else {
        // ignore unknown types
      }
      pos = i + 1;
    }
    // shift remaining bytes to front
    if (pos < bufused) {
      memmove(buf, buf + pos, bufused - pos);
      bufused -= pos;
    } else {
      bufused = 0;
    }
  }
}

void *client_thread(void *arg) {
  client_t *c = (client_t *)arg;
  handle_client_read(c);
  return NULL;
}

void sigint_handler(int sig) {
  (void)sig;
  server_running = 0;
  if (listen_fd >= 0)
    close(listen_fd);
  pthread_cond_broadcast(&q_cv);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <port> <# of clients>\n", argv[0]);
    return EXIT_FAILURE;
  }
  signal(SIGINT, sigint_handler);
  int port = atoi(argv[1]);
  expected_clients = atoi(argv[2]);
  if (expected_clients <= 0) {
    fprintf(stderr, "Expected clients must be > 0\n");
    return EXIT_FAILURE;
  }

  // start broadcaster thread
  pthread_t bth;
  pthread_create(&bth, NULL, broadcaster, NULL);

  // create listening socket
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY; // bind to all addresses
  sin.sin_port = htons(port);

  if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    perror("bind");
    return EXIT_FAILURE;
  }
  if (listen(listen_fd, BACKLOG) < 0) {
    perror("listen");
    return EXIT_FAILURE;
  }

  fprintf(stderr, "Server listening on port %d, expecting %d clients\n", port,
          expected_clients);

  int client_id = 0;
  while (server_running) {
    struct sockaddr_in cliaddr;
    socklen_t addrlen = sizeof(cliaddr);
    int fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &addrlen);
    if (fd < 0) {
      if (!server_running)
        break;
      if (errno == EINTR)
        continue;
      perror("accept");
      break;
    }
    // create client structure
    client_t *c = calloc(1, sizeof(client_t));
    if (!c) {
      close(fd);
      continue;
    }
    c->fd = fd;
    c->addr = cliaddr;
    c->id = client_id++;
    c->sent_type1 = 0;
    add_client(c);
    // spawn handler thread
    pthread_t th;
    pthread_create(&th, NULL, client_thread, c);
    pthread_detach(th);
  }

  // wake broadcaster if waiting and join
  pthread_mutex_lock(&q_mtx);
  pthread_cond_signal(&q_cv);
  pthread_mutex_unlock(&q_mtx);
  pthread_join(bth, NULL);

  // cleanup: close all clients
  pthread_mutex_lock(&clients_mtx);
  client_t *it = clients;
  while (it) {
    close(it->fd);
    client_t *next = it->next;
    free(it);
    it = next;
  }
  clients = NULL;
  pthread_mutex_unlock(&clients_mtx);

  fprintf(stderr, "Server exiting\n");
  return EXIT_SUCCESS;
}
