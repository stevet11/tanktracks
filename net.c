#include "net.h"
#include <enet/enet.h>
#include <string.h>
#include <stdio.h>
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
void net_lan_serve_stop(void);
void net_stop(void) {
    net_lan_serve_stop();
    if (held) { enet_packet_destroy(held); held = NULL; }
    if (host) {
        for (int i = 0; i < NMAXPEERS; i++) if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
        enet_host_flush(host);
        enet_host_destroy(host);
        host = NULL;
    }
    memset(peers, 0, sizeof(peers));
    net_lan_scan_stop();
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

// ---------------------------------------------------------------- LAN discovery
// probe: "TTK1?"    reply: "TTK1!<gamePort>,<players>,<maxPlayers>"
#define DMAGIC "TTK1"
static ENetSocket srvSock = ENET_SOCKET_NULL, scanSock = ENET_SOCKET_NULL;
static int srvGamePort, srvMax;
static NetLanGame found[16]; static double foundSeen[16]; static int nfound;
static double lastProbe;
static double nowSec(void) { return (double)enet_time_get() / 1000.0; }

static ENetSocket dgram(int bind_port, bool reuse) {
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return s;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    if (reuse) enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
    ENetAddress a = {ENET_HOST_ANY, (enet_uint16)bind_port};
    if (enet_socket_bind(s, &a) < 0) { enet_socket_destroy(s); return ENET_SOCKET_NULL; }
    return s;
}
bool net_lan_serve(int gamePort, int maxPlayers) {
    if (!init()) return false;
    net_lan_serve_stop();
    srvSock = dgram(NET_DISCO_PORT, true);
    srvGamePort = gamePort; srvMax = maxPlayers;
    return srvSock != ENET_SOCKET_NULL;   // failure only means the game won't be discoverable
}
void net_lan_serve_update(int players) {
    if (srvSock == ENET_SOCKET_NULL) return;
    for (int n = 0; n < 16; n++) {   // bounded work per frame
        char in[32]; ENetAddress from; ENetBuffer b = {in, sizeof in};
        int r = enet_socket_receive(srvSock, &from, &b, 1);
        if (r <= 0) break;
        if (r < 5 || memcmp(in, DMAGIC "?", 5) != 0) continue;
        char out[48];
        int len = snprintf(out, sizeof out, DMAGIC "!%d,%d,%d", srvGamePort, players, srvMax);
        ENetBuffer ob = {out, (size_t)len};
        enet_socket_send(srvSock, &from, &ob, 1);
    }
}
void net_lan_serve_stop(void) {
    if (srvSock != ENET_SOCKET_NULL) { enet_socket_destroy(srvSock); srvSock = ENET_SOCKET_NULL; }
}
bool net_lan_scan_start(void) {
    if (!init()) return false;
    net_lan_scan_stop();
    scanSock = dgram(0, false);
    nfound = 0; lastProbe = -10;
    return scanSock != ENET_SOCKET_NULL;
}
void net_lan_scan_stop(void) {
    if (scanSock != ENET_SOCKET_NULL) { enet_socket_destroy(scanSock); scanSock = ENET_SOCKET_NULL; }
    nfound = 0;
}
int net_lan_scan_update(NetLanGame *out, int max) {
    if (scanSock == ENET_SOCKET_NULL) return 0;
    double t = nowSec();
    if (t - lastProbe >= 1.0) {
        lastProbe = t;
        ENetBuffer pb = {(void *)(DMAGIC "?"), 5};
        ENetAddress to = {ENET_HOST_BROADCAST, NET_DISCO_PORT};
        enet_socket_send(scanSock, &to, &pb, 1);
        to.host = ENET_HOST_TO_NET_32(0x7f000001);   // a host on this same machine
        enet_socket_send(scanSock, &to, &pb, 1);
    }
    for (int n = 0; n < 32; n++) {
        char in[64]; ENetAddress from; ENetBuffer b = {in, sizeof in - 1};
        int r = enet_socket_receive(scanSock, &from, &b, 1);
        if (r <= 0) break;
        in[r] = 0;
        int gp, pl, mx;
        if (r < 6 || memcmp(in, DMAGIC "!", 5) != 0 || sscanf(in + 5, "%d,%d,%d", &gp, &pl, &mx) != 3) continue;
        char ip[48];
        if (enet_address_get_host_ip(&from, ip, sizeof ip) != 0) continue;
        int k = -1;
        for (int i = 0; i < nfound; i++) if (found[i].port == gp && !strcmp(found[i].ip, ip)) { k = i; break; }
        if (k < 0) { if (nfound >= 16) continue; k = nfound++; }
        snprintf(found[k].ip, sizeof found[k].ip, "%s", ip);
        found[k].port = gp; found[k].players = pl; found[k].maxPlayers = mx; foundSeen[k] = t;
    }
    int w = 0;   // drop stale entries
    for (int i = 0; i < nfound; i++) if (t - foundSeen[i] < 4.0) { found[w] = found[i]; foundSeen[w] = foundSeen[i]; w++; }
    nfound = w;
    int m = 0;   // a same-machine host answers both the broadcast and the loopback probe: list it once, under its LAN address
    for (int i = 0; i < nfound && m < max; i++) {
        bool dup = false;
        if (!strcmp(found[i].ip, "127.0.0.1"))
            for (int j = 0; j < nfound; j++) if (j != i && found[j].port == found[i].port && strcmp(found[j].ip, "127.0.0.1")) dup = true;
        if (!dup) out[m++] = found[i];
    }
    return m;
}
