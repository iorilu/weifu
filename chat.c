/*
 * chat server running on android 
 * Author - iorilu
 * Date - 2014/3/3
 * Version - 0.01 
 * Compile: cc -Wall -g -o chat chat.c weifu.c -lpthread
*/


#include "weifu.h"

#define SERVER_POLL_INTERVAL 2000


static void *stdin_thread(void *param) {
  int ch, sock = * (int *) param;
  while ((ch = getchar()) != EOF) {
    unsigned char c = (unsigned char) ch;
    send(sock, &c, 1, 0);  // Forward all types characters to the socketpair
  }
  return NULL;
}

static void broadcast(struct weifu_connection *conn, enum weifu_event ev, void *p) {
  if (ev == WEIFU_POLL) {
    struct iobuf *io = (struct iobuf *) p;
    weifu_send(conn, io->buf, io->len);
  }
}

static void server_handler(struct weifu_connection *conn, enum weifu_event ev, void *p) {

  if(ev == WEIFU_RECV){
      struct iobuf *io = &conn->recv_iobuf;
      weifu_iterate(conn->server, broadcast, io);
      iobuf_remove(io, io->len);        // Discard message from recv buffer
  }
}

static void client_handler(struct weifu_connection *conn, enum weifu_event ev,
                           void *p) {
  struct iobuf *io = &conn->recv_iobuf;
  (void) p;

  if (ev == WEIFU_CONNECT) {
    if (conn->flags & NSF_CLOSE_IMMEDIATELY) {
      printf("%s\n", "Error connecting to server!");
      exit(EXIT_FAILURE);
    }
    printf("%s\n", "Connected to server. Type a message and press enter.");
  } else if (ev == WEIFU_RECV) {
    if (conn->flags & NSF_USER_1) {
      // Received data from the stdin, forward it to the server
      struct weifu_connection *c = (struct weifu_connection *) conn->connection_data;
      weifu_send(c, io->buf, io->len);
      iobuf_remove(io, io->len);
    } else {
      // Received data from server connection, print it
      fwrite(io->buf, io->len, 1, stdout);
      iobuf_remove(io, io->len);
    }
  } else if (ev == WEIFU_CLOSE) {
    // Connection has closed, most probably cause server has stopped
    exit(EXIT_SUCCESS);
  }
}


int main(int argc, char *argv[]) {

  struct weifu_server server;
  char *server_port = NULL;
  char *type = NULL;

  if(argc != 3){
    fprintf(stderr, "Usage: %s <port> <client|server>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  server_port = argv[1];
  type = argv[2];

  if(strcmp(type, "client") == 0){

    int fds[2];
    struct weifu_connection *ioconn, *server_conn;

    weifu_server_init(&server, NULL, client_handler);

    // Connect to the pubsub server
    server_conn = weifu_connect(&server, "127.0.0.1", atoi(server_port), 0);
    if (server_conn == NULL) {
      fprintf(stderr, "Cannot connect to port %s\n", server_port);
      exit(EXIT_FAILURE);
    }

    // Create a socketpair and give one end to the thread that reads stdin
    weifu_socketpair(fds);
    weifu_start_thread(stdin_thread, &fds[1]);

    // The other end of a pair goes inside the server
    ioconn = weifu_add_sock(&server, fds[0], NULL);
    ioconn->flags |= NSF_USER_1;    // Mark this so we know this is a stdin
    ioconn->connection_data = server_conn;

  }else if(strcmp(type, "server") == 0){
    weifu_server_init(&server, NULL, server_handler);
    weifu_set_option(&server, get_option_name(LISTENING_PORT), server_port);
    //weifu_set_option(server, "document_root", "web_root");
    weifu_server_start(&server);
    printf("Starting on port %s\n", weifu_get_option(&server, get_option_name(LISTENING_PORT)));
  }

  for (;;) {
    weifu_poll_server(&server, SERVER_POLL_INTERVAL);
  }

  weifu_destroy_server(&server);

  return 0;
}
