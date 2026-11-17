#include "net.h"
#include <enet/enet.h>
#include <string.h>
#include <stdint.h>

#define NMAXPEERS 8
static ENetHost *host;
static ENetPeer *peers[NMAXPEERS];
static ENetPacket *held;      // packet handed out by the last net_poll, freed on the next call
static bool inited, isServer;

static bool init(void) {
    if (inited) return true;
    if (enet_initialize() != 0) return false;
    inited = true;
    return true;
}
bool net_host(int port, int maxPeers) {
    if (!init()) return false;
    ENetAddress a = {ENET_HOST_ANY, (enet_uint16)port};
    if (maxPeers > NMAXPEERS) maxPeers = NMAXPEERS;
    host = enet_host_create(&a, maxPeers, 2, 0, 0);
    isServer = host != NULL;
    memset(peers, 0, sizeof(peers));
    return host != NULL;
}
bool net_connect(const char *name, int port) {
    if (!init()) return false;
    host = enet_host_create(NULL, 1, 2, 0, 0);
    if (!host) return false;
    ENetAddress a;
    if (enet_address_set_host(&a, name) != 0) { enet_host_destroy(host); host = NULL; return false; }
    a.port = (enet_uint16)port;
    memset(peers, 0, sizeof(peers));
    isServer = false;
    peers[0] = enet_host_connect(host, &a, 2, 0);
    return peers[0] != NULL;
}
void net_stop(void) {
    if (held) { enet_packet_destroy(held); held = NULL; }
    if (host) {
        for (int i = 0; i < NMAXPEERS; i++) if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
        enet_host_flush(host);
        enet_host_destroy(host);
        host = NULL;
    }
    memset(peers, 0, sizeof(peers));
    if (inited) { enet_deinitialize(); inited = false; }
}
bool net_active(void) { return host != NULL; }
bool net_poll(NetEvent *ev) {
    if (held) { enet_packet_destroy(held); held = NULL; }
    if (!host) return false;
    ENetEvent e;
    if (enet_host_service(host, &e, 0) <= 0) return false;
    memset(ev, 0, sizeof(*ev));
    int slot = 0;
    if (isServer) {
        if (e.type == ENET_EVENT_TYPE_CONNECT) {
            slot = -1;
            for (int i = 0; i < NMAXPEERS; i++) if (!peers[i]) { slot = i; break; }
            if (slot < 0) { enet_peer_disconnect_now(e.peer, 0); return net_poll(ev); }
            peers[slot] = e.peer; e.peer->data = (void *)(intptr_t)(slot + 1);
        } else slot = (int)(intptr_t)e.peer->data - 1;
    }
    ev->peer = slot;
    switch (e.type) {
    case ENET_EVENT_TYPE_CONNECT: ev->type = NET_CONNECT; break;
    case ENET_EVENT_TYPE_DISCONNECT:
        ev->type = NET_DISCONNECT;
        if (isServer && slot >= 0) peers[slot] = NULL; else peers[0] = NULL;
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        ev->type = NET_RECEIVE; ev->channel = e.channelID; ev->data = e.packet->data; ev->len = (int)e.packet->dataLength;
        held = e.packet;
        break;
    default: ev->type = NET_NONE; break;
    }
    return true;
}
void net_send(int peer, int channel, bool reliable, const void *data, int len) {
    if (!host) return;
    int i = isServer ? peer : 0;
    if (i < 0 || i >= NMAXPEERS || !peers[i]) return;
    ENetPacket *p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (p) enet_peer_send(peers[i], (enet_uint8)channel, p);
}
void net_broadcast(int channel, bool reliable, const void *data, int len) {
    if (!host) return;
    for (int i = 0; i < NMAXPEERS; i++) if (peers[i]) net_send(i, channel, reliable, data, len);
}
void net_flush(void) { if (host) enet_host_flush(host); }
