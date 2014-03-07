/*
 * chat server running on android 
 * Author - iorilu
 * Date - 2014/3/3
 * Version - 0.01 
 * Compile: cc -Wall -g -o chat chat.c weifu.c
*/


#include "weifu.h"

#define SERVER_PORT "8080"
#define SERVER_POLL_INTERVAL 2000
#define LISTENING_QUEUE 5

static void ev_handler(struct weifu_connection *conn, enum weifu_event ev, void *p) {
  struct iobuf *io = &conn->recv_iobuf;
  (void) p;

  switch (ev) {
    case WEIFU_RECV:
      weifu_send(conn, io->buf, io->len);  // Echo message back
      iobuf_remove(io, io->len);        // Discard message from recv buffer
    default:
      break;
  }
}

int main(void) {
  struct weifu_server *server = weifu_create_server(NULL, ev_handler);
  weifu_set_option(server, get_option_name(LISTENING_PORT), SERVER_PORT);
  //weifu_set_option(server, "document_root", "web_root");
  weifu_server_start(server);
  printf("Starting on port %s\n", weifu_get_option(server, get_option_name(LISTENING_PORT)));
  for (;;) {
    weifu_poll_server(server, SERVER_POLL_INTERVAL);
  }
  weifu_destroy_server(server);

  return 0;
}
