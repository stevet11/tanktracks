// Thin wrapper around ENet so main.c never has to include enet.h (its socket headers clash with raylib on Windows).
#ifndef TT_NET_H
#define TT_NET_H
#include <stdbool.h>
enum { NET_NONE = 0, NET_CONNECT, NET_DISCONNECT, NET_RECEIVE };
typedef struct { int type; int peer; int channel; const unsigned char *data; int len; } NetEvent;
bool net_host(int port, int maxPeers);                 // listen for up to maxPeers clients
bool net_connect(const char *host, int port);          // start connecting to a host (non-blocking)
void net_stop(void);
bool net_poll(NetEvent *ev);                           // next pending event; ev->data is valid until the next call
// peer: on the host the peer slot (0..maxPeers-1); ignored on a client (always the server)
void net_send(int peer, int channel, bool reliable, const void *data, int len);
void net_broadcast(int channel, bool reliable, const void *data, int len);
void net_flush(void);
bool net_active(void);
#endif
