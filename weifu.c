/*weifu library - a tiny standalone server 
 * Author - iorilu
 * Date - 2014/3/3
 * Version - 0.01
*/
#include "weifu.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ERR_NO_SUCH_OPTION   "No such option"
#define ERR_SOCK_CREATION    "socket creation error"
#define ERR_SOCK_BIND        "socket bind error"
#define ERR_SOCK_LISTEN      "socket listen error"    

#define IOBUF_RESIZE_MULTIPLIER 2


static const char *default_config_options[] = {
  "listening_port", "8080",
  "document_root", "web_root",
  NULL
};



void iobuf_init(struct iobuf *iobuf, int size) {
  iobuf->len = iobuf->size = 0;
  iobuf->buf = NULL;

  if (size > 0 && (iobuf->buf = (char *) malloc(size)) != NULL) {
    iobuf->size = size;
  }
}

void iobuf_free(struct iobuf *iobuf) {
  if (iobuf != NULL) {
    if (iobuf->buf != NULL) free(iobuf->buf);
    iobuf_init(iobuf, 0);
  }
}

int iobuf_append(struct iobuf *io, const void *buf, int len) {
  static const double mult = IOBUF_RESIZE_MULTIPLIER;
  char *p = NULL;
  int new_len = 0;

  assert(io->len >= 0);
  assert(io->len <= io->size);

  if (len <= 0) {
  } else if ((new_len = io->len + len) < io->size) {
    memcpy(io->buf + io->len, buf, len);
    io->len = new_len;
  } else if ((p = (char *)
              realloc(io->buf, (int) (new_len * mult))) != NULL) {
    io->buf = p;
    memcpy(io->buf + io->len, buf, len);
    io->len = new_len;
    io->size = (int) (new_len * mult);
  } else {
    len = 0;
  }

  return len;
}


void iobuf_remove(struct iobuf *io, int n) {
  if (n >= 0 && n <= io->len) {
    memmove(io->buf, io->buf + n, io->len - n);
    memset(io->buf + (io->len - n), 0, n);
    io->len -= n;
  }
}




static char *weifu_strdup(const char *str) {
  char *copy = (char *) malloc(strlen(str) + 1);
  if (copy != NULL) {
    strcpy(copy, str);
  }
  return copy;
}

static void set_close_on_exec(sock_t sock) {
  fcntl(sock, F_SETFD, FD_CLOEXEC);
}

/*
 *set socket as Non-blocking
*/
static void set_non_blocking(int sock)
{
  int opts;

  opts = fcntl(sock,F_GETFL);
  if (opts < 0) {
    perror("fcntl(F_GETFL)");
    exit(EXIT_FAILURE);
  }
  opts = (opts | O_NONBLOCK);
  if (fcntl(sock,F_SETFL,opts) < 0) {
    perror("fcntl(F_SETFL)");
    exit(EXIT_FAILURE);
  }
  return;
}


static void set_default_option_values(char **opts) {
  const char *value;
  int i;

  for (i = 0; default_config_options[i * 2] != NULL; i++) {
    value = default_config_options[i * 2 + 1];
    if (opts[i] == NULL && value != NULL) {
      opts[i] = weifu_strdup(value);
    }
  }
}

static int get_option_index(const char *name) {
  int i;

for (i = 0; default_config_options[i * 2] != NULL; i++) {
    if (strcmp(default_config_options[i * 2], name) == 0) {
      return i;
    }
  }
  return -1;
}



static void weifu_call(struct weifu_connection *conn, enum weifu_event ev, void *p) {
  if (conn->server->callback) conn->server->callback(conn, ev, p);
}

static void weifu_remove_conn(struct weifu_connection *conn) {
  if (conn->prev == NULL) conn->server->active_connections = conn->next;
  if (conn->prev) conn->prev->next = conn->next;
  if (conn->next) conn->next->prev = conn->prev;
}

static void weifu_close_conn(struct weifu_connection *conn) {
  DBG(("%p %d", conn, conn->flags));
  weifu_call(conn, WEIFU_CLOSE, NULL);
  weifu_remove_conn(conn);
  closesocket(conn->sock);
  iobuf_free(&conn->recv_iobuf);
  iobuf_free(&conn->send_iobuf);
  free(conn);
}

static void weifu_add_conn(struct weifu_server *server, struct weifu_connection *c) {
  c->next = server->active_connections;
  server->active_connections = c;
  c->prev = NULL;
  if (c->next != NULL) c->next->prev = c;
}

static void weifu_add_to_set(sock_t sock, fd_set *set, sock_t *max_fd) {
  if (sock != INVALID_SOCKET) {
    FD_SET(sock, set);
    if (*max_fd == INVALID_SOCKET || sock > *max_fd) {
      *max_fd = sock;
    }
  }
}



static int weifu_is_error(int n) {
  return n == 0 ||
    (n < 0 && errno != EINTR && errno != EINPROGRESS &&
     errno != EAGAIN && errno != EWOULDBLOCK
    );
}

static void weifu_write_to_socket(struct weifu_connection *conn)
{

  struct iobuf *io = &conn->send_iobuf;
  int n = 0;

  n = send(conn->sock, io->buf, io->len, 0);

  DBG(("%p -> %d bytes [%.*s%s]", conn, n, io->len < 40 ? io->len : 40,
       io->buf, io->len < 40 ? "" : "..."));

  if (weifu_is_error(n)) {
    conn->flags |= NSF_CLOSE_IMMEDIATELY;
  } else if (n > 0) {
    iobuf_remove(io, n);
    //conn->num_bytes_sent += n;
  }

  if (io->len == 0 && conn->flags & NSF_FINISHED_SENDING_DATA) {
    conn->flags |= NSF_CLOSE_IMMEDIATELY;
  }

  weifu_call(conn, WEIFU_SEND, NULL);
}


static void weifu_read_from_socket(struct weifu_connection *conn)
{

  char buf[READ_BUF];
  memset(buf, 0 , READ_BUF);
  int n = 0;

  if (conn->flags & NSF_CONNECTING) {
    int ok = 1, ret;
    socklen_t len = sizeof(ok);

    ret = getsockopt(conn->sock, SOL_SOCKET, SO_ERROR, (char *) &ok, &len);
    (void) ret;

    conn->flags &= ~NSF_CONNECTING;
    DBG(("%p ok=%d", conn, ok));
    if (ok != 0) {
      conn->flags |= NSF_CLOSE_IMMEDIATELY;
    }
    weifu_call(conn, WEIFU_CONNECT, &ok);
    return;
  }

    n = recv(conn->sock, buf, sizeof(buf), 0);

 DBG(("%p <- %d bytes [%.*s%s]", conn, n, n < 40 ? n : 40, buf, n < 40 ? "" : "..."));

  if (weifu_is_error(n)) {
    conn->flags |= NSF_CLOSE_IMMEDIATELY;
  } else if (n > 0) {
    iobuf_append(&conn->recv_iobuf, buf, n);
    DBG(("received: %d bytes->%s", n, (conn->recv_iobuf).buf));
    weifu_call(conn, WEIFU_RECV, &n);
  }
}

struct weifu_connection *weifu_add_sock(struct weifu_server *s, sock_t sock, void *p) {
  struct weifu_connection *conn;
  if ((conn = (struct weifu_connection *) malloc(sizeof(*conn))) != NULL) {
    memset(conn, 0, sizeof(*conn));
    set_non_blocking(sock);
    conn->sock = sock;
    conn->connection_data = p;
    conn->server = s;
    conn->last_io_time = time(NULL);
    weifu_add_conn(s, conn);
    DBG(("%p %d", conn, sock));
  }
  return conn;
}

static struct weifu_connection *accept_conn(struct weifu_server *server) {
  struct weifu_connection *c = NULL;
  union socket_address sa;
  socklen_t len = sizeof(sa);
  sock_t sock = INVALID_SOCKET;

  // NOTE(lsm): on Windows, sock is always > FD_SETSIZE
  if ((sock = accept(server->listening_sock, &sa.sa, &len)) == INVALID_SOCKET) {
    closesocket(sock);
  } else if ((c = (struct weifu_connection *) malloc(sizeof(*c))) == NULL ||
             memset(c, 0, sizeof(*c)) == NULL) {
    closesocket(sock);
  } else {
    set_close_on_exec(sock);
    set_non_blocking(sock);
    c->server = server;
    c->sock = sock;
    c->flags |= NSF_ACCEPTED;

    weifu_add_conn(server, c);
    weifu_call(c, WEIFU_ACCEPT, &sa);
  }

  return c;
}

void weifu_server_init(struct weifu_server *s, void *server_data, weifu_callback_t cb) 
{
  memset(s, 0, sizeof(*s));
  s->listening_sock = INVALID_SOCKET;
  s->server_data = server_data;
  memset(&s->lsa, 0, sizeof(s->lsa));
  s->callback = cb;
  // Ignore SIGPIPE signal, so if client cancels the request, it
  // won't kill the whole process.
  signal(SIGPIPE, SIG_IGN);
}


int weifu_send(struct weifu_connection *conn, const void *buf, int len) {
  return iobuf_append(&conn->send_iobuf, buf, len);
}

/*
 * set option parameter for server
*/
const char *weifu_set_option(struct weifu_server *server, const char *name, const char *value)
{
  int ind = get_option_index(name);
  const char *error_msg = NULL;

  if (ind < 0) {
    error_msg = ERR_NO_SUCH_OPTION;
  } else {
    if (server->config_options[ind] != NULL) {
      free(server->config_options[ind]);
    }
    server->config_options[ind] = weifu_strdup(value);
    DBG(("%s [%s]", name, value));
  }

  return error_msg;
}

/*
 *get option name by enum index
*/
const char *get_option_name(int ind)
{
  return default_config_options[ind * 2];
}

/*
 *get an option parameter
 *make sure returned value is not NULL
*/
const char *weifu_get_option(struct weifu_server *server, const char *name) {
  const char **opts = (const char **) server->config_options;
  int i = get_option_index(name);
  return i == -1 ? NULL : opts[i] == NULL ? "" : opts[i];
}

/*
 *create a server instance
*/
struct weifu_server* weifu_create_server(void *server_param, weifu_callback_t cb)
{
  struct weifu_server *server = (struct weifu_server *) calloc(1, sizeof(*server));
  weifu_server_init(server, server_param, cb);
  set_default_option_values(server->config_options);
  return server;
}

int weifu_server_start(struct weifu_server* server)
{
  const char *ascport = weifu_get_option(server, get_option_name(LISTENING_PORT));
  int port = atoi(ascport);
  int reuse_addr = 1;
  int sock; //listening sock

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock < 0){
    perror(ERR_SOCK_CREATION);
    exit(EXIT_FAILURE);
  }


  /* So that we can re-bind to it without TIME_WAIT problems */
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
    sizeof(reuse_addr));

  server->listening_sock = sock;
  set_non_blocking(server->listening_sock);
  (server->lsa).sin.sin_family = AF_INET;
  (server->lsa).sin.sin_addr.s_addr = htonl(INADDR_ANY);
  (server->lsa).sin.sin_port = htons(port);

  if (bind(server->listening_sock, (struct sockaddr *) &server->lsa,
    sizeof(server->lsa)) < 0 ) {
    perror(ERR_SOCK_BIND);
    close(server->listening_sock);
    exit(EXIT_FAILURE);
  }

    /* Set up queue for incoming connections. */
  if (listen(server->listening_sock,LISTENING_QUEUE) != 0) {
    perror(ERR_SOCK_LISTEN);
    exit(EXIT_FAILURE);
  }
    return 0;
}

/*
 *iterate through all active connections on server
*/
void weifu_iterate(struct weifu_server *server, weifu_callback_t cb, void *param) {
  struct weifu_connection *conn, *tmp_conn;

  for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;
    cb(conn, WEIFU_POLL, param);
  }
}

/*
 * poll all connections by select
 * return: number of active connections 
*/
int weifu_poll_server(struct weifu_server *server, int interval)
{
  struct weifu_connection *conn, *tmp_conn;
  struct timeval tv;
  fd_set read_set, write_set;
  int num_active_connections = 0;
  sock_t max_fd = INVALID_SOCKET;
  time_t current_time = time(NULL);

  if (server->listening_sock == INVALID_SOCKET &&
      server->active_connections == NULL) return 0;

  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  weifu_add_to_set(server->listening_sock, &read_set, &max_fd);

  for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;
    weifu_call(conn, WEIFU_POLL, &current_time);
    weifu_add_to_set(conn->sock, &read_set, &max_fd);
    if (conn->flags & NSF_CONNECTING) {
      weifu_add_to_set(conn->sock, &write_set, &max_fd);
    }
    if (conn->send_iobuf.len > 0 && !(conn->flags & NSF_BUFFER_BUT_DONT_SEND)) {
      weifu_add_to_set(conn->sock, &write_set, &max_fd);
    } else if (conn->flags & NSF_CLOSE_IMMEDIATELY) {
      weifu_close_conn(conn);
    }
  }

  tv.tv_sec = interval / 1000;
  tv.tv_usec = (interval % 1000) * 1000;

  if (select((int) max_fd + 1, &read_set, &write_set, NULL, &tv) > 0) {
    // Accept new connections
    if (server->listening_sock != INVALID_SOCKET &&
        FD_ISSET(server->listening_sock, &read_set)) {
      // We're not looping here, and accepting just one connection at
      // a time. The reason is that eCos does not respect non-blocking
      // flag on a listening socket and hangs in a loop.
      if ((conn = accept_conn(server)) != NULL) {
        conn->last_io_time = current_time;
      }
    }

    for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
      tmp_conn = conn->next;
      if (FD_ISSET(conn->sock, &read_set)) {
        conn->last_io_time = current_time;
        weifu_read_from_socket(conn);
      }
      if (FD_ISSET(conn->sock, &write_set)) {
        if (conn->flags & NSF_CONNECTING) {
          weifu_read_from_socket(conn);
        } else if (!(conn->flags & NSF_BUFFER_BUT_DONT_SEND)) {
          conn->last_io_time = current_time;
          weifu_write_to_socket(conn);
        }
      }
    }
  }


  for (conn = server->active_connections; conn != NULL; conn = tmp_conn) {
    tmp_conn = conn->next;
    num_active_connections++;
    if (conn->flags & NSF_CLOSE_IMMEDIATELY) {
      weifu_close_conn(conn);
    }
  }

  DBG(("%d active connections", num_active_connections));

  return num_active_connections;
}

/*
 * destroy server instance
 * return: 
*/
void weifu_destroy_server(struct weifu_server *s)
{
    DBG(("%p", s));

    struct weifu_connection *conn, *tmp_conn;
    if(s == NULL) return;

    //do one last poll
    weifu_poll_server(s, 0);

    if(s->listening_sock != INVALID_SOCKET) closesocket(s->listening_sock);
    s->listening_sock = INVALID_SOCKET;

    for(conn = s->active_connections; conn != NULL; conn = tmp_conn){
      tmp_conn = conn->next;
      weifu_close_conn(conn);
    }

}

struct weifu_connection *weifu_connect(struct weifu_server* server, const char *host,
                                        int port, void *param)
{

  sock_t sock = INVALID_SOCKET;
  struct sockaddr_in sin;
  struct hostent *he = NULL;
  struct weifu_connection *conn = NULL;
  int connect_ret_val;

  if (host == NULL || (he = gethostbyname(host)) == NULL ||
      (sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
    DBG(("gethostbyname(%s) failed: %s", host, strerror(errno)));
    return NULL;
  }

  sin.sin_family = AF_INET;
  sin.sin_port = htons((uint16_t) port);
  sin.sin_addr = * (struct in_addr *) he->h_addr_list[0];
  set_non_blocking(sock);

  connect_ret_val = connect(sock, (struct sockaddr *) &sin, sizeof(sin));
  if (weifu_is_error(connect_ret_val)) {
    closesocket(sock);
    return NULL;
  } else if ((conn = (struct weifu_connection *)
              malloc(sizeof(*conn))) == NULL) {
    closesocket(sock);
    return NULL;
  }

  memset(conn, 0, sizeof(*conn));
  conn->server = server;
  conn->sock = sock;
  conn->connection_data = param;
  conn->flags = NSF_CONNECTING;
  conn->last_io_time = time(NULL);

  weifu_add_conn(server, conn);
  DBG(("%p %s:%d %d", conn, host, port, conn->sock));

  return conn;
}                                        


/*
 *start a new thread with a function , like taking input from stdin
*/
void *weifu_start_thread(void *(*f)(void *), void *p) {
  pthread_t thread_id = (pthread_t) 0;
  pthread_attr_t attr;

  (void) pthread_attr_init(&attr);
  (void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  pthread_create(&thread_id, &attr, f, p);
  pthread_attr_destroy(&attr);

  return (void *) thread_id;
}


/*
 *create a pair sockets , used to monitor like stdin and send to remote
*/
int weifu_socketpair(sock_t sp[2]) {
  struct sockaddr_in sa;
  sock_t sock;
  socklen_t len = sizeof(sa);
  int ret = 0;

  sp[0] = sp[1] = INVALID_SOCKET;

  (void) memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(0);
  sa.sin_addr.s_addr = htonl(0x7f000001);

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) != INVALID_SOCKET &&
      !bind(sock, (struct sockaddr *) &sa, len) &&
      !listen(sock, 1) &&
      !getsockname(sock, (struct sockaddr *) &sa, &len) &&
      (sp[0] = socket(AF_INET, SOCK_STREAM, 6)) != -1 &&
      !connect(sp[0], (struct sockaddr *) &sa, len) &&
      (sp[1] = accept(sock,(struct sockaddr *) &sa, &len)) != INVALID_SOCKET) {
    set_close_on_exec(sp[0]);
    set_close_on_exec(sp[1]);
    ret = 1;
  } else {
    if (sp[0] != INVALID_SOCKET) closesocket(sp[0]);
    if (sp[1] != INVALID_SOCKET) closesocket(sp[1]);
    sp[0] = sp[1] = INVALID_SOCKET;
  }
  closesocket(sock);

  return ret;
}
