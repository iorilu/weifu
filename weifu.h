
#ifndef WEIFU_HEADER_INCLUDED
#define  WEIFU_HEADER_INCLUDED

#define WEIFU_VERSION "0.01"


#include <stdio.h>      // required for FILE
#include <stddef.h>     // required for size_t
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>  // For inet_pton() when NS_ENABLE_IPV6 is defined
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

#define closesocket(x) close(x)
#define INVALID_SOCKET (-1)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))
#define DBG(x) do { printf("%-20s ", __func__); printf x; putchar('\n'); \
  fflush(stdout); } while(0)
#define LISTENING_QUEUE 5
#define READ_BUF 2048
typedef int sock_t;


//enum for all options , NUM_OPTIONS at the bottom to track total number of options
 enum{
 	LISTENING_PORT,
 	DOCUMENT_ROOT,
 	NUM_OPTIONS
 };

 union socket_address {
  struct sockaddr sa;
  struct sockaddr_in sin;
};

struct iobuf {
  char *buf;
  int len;
  int size;
};

enum weifu_event {WEIFU_POLL, WEIFU_ACCEPT, WEIFU_CONNECT, WEIFU_RECV, WEIFU_SEND, WEIFU_CLOSE };


void iobuf_init(struct iobuf *, int initial_size);
void iobuf_free(struct iobuf *);
int iobuf_append(struct iobuf *, const void *data, int data_size);
void iobuf_remove(struct iobuf *, int data_size);

struct weifu_connection
{
	struct weifu_connection *prev, *next;
	struct weifu_server *server;
	void *connection_data;
	time_t last_io_time;
	sock_t sock;
	struct iobuf recv_iobuf;
	struct iobuf send_iobuf;
	unsigned int flags;
#define NSF_FINISHED_SENDING_DATA   (1 << 0)
#define NSF_BUFFER_BUT_DONT_SEND    (1 << 1)
#define NSF_SSL_HANDSHAKE_DONE      (1 << 2)
#define NSF_CONNECTING              (1 << 3)
#define NSF_CLOSE_IMMEDIATELY       (1 << 4)
#define NSF_ACCEPTED                (1 << 5)
#define NSF_USER_1                  (1 << 6)
#define NSF_USER_2                  (1 << 7)
#define NSF_USER_3                  (1 << 8)
#define NSF_USER_4                  (1 << 9)
};

typedef void (*weifu_callback_t)(struct weifu_connection *, enum weifu_event, void *);

struct weifu_server {
  void *server_data;
  union socket_address lsa;   // Listening socket address
  weifu_callback_t callback;
  char *config_options[NUM_OPTIONS];
  struct weifu_connection *active_connections;
  char local_ip[48];
  sock_t listening_sock;
};


int weifu_send(struct weifu_connection *, const void *buf, int len);
struct weifu_server* weifu_create_server(void *server_param, weifu_callback_t cb);
void weifu_destroy_server(struct weifu_server *);
const char *weifu_set_option(struct weifu_server *server, const char *name, const char *value);
const char *weifu_get_option(const struct weifu_server *server, const char *name);
int weifu_server_start(struct weifu_server*);
const char *get_option_name(int ind);
int weifu_poll_server(struct weifu_server *server, int interval);
void weifu_iterate(struct weifu_server *, weifu_callback_t cb, void *param);


#endif