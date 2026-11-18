// TANK TRACKS - drive a tank through a city and shoot everything. C + raylib 5.5. Windows & Linux.
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "net.h"

// ---------------------------------------------------------------- world constants
#define N 9                    // blocks per side
#define S 70.0f                // road-to-road spacing
#define ROAD 14.0f
#define BLK (S - ROAD)         // block size (56)
#define HALFW (N * S / 2)      // half extent of the road grid
#define MAXOBJ 9000
#define MAXCAR 60
#define MAXPROJ 256
#define MAXPART 7000
#define MAXEMIT 96
#define GRAV 32.0f
#define SHELL_V 120.0f
#define TANK_R 2.3f

typedef enum { O_HOUSE, O_BLDG, O_TREE, O_FENCE, O_LAMP, O_FUEL, O_HYDRANT, O_MAILBOX, O_WATER, O_BILL, O_NTYPES } OType;
static const char *oname[O_NTYPES] = {"HOUSE", "BUILDING", "TREE", "FENCE", "LAMP POST", "FUEL TANK", "HYDRANT", "MAILBOX", "WATER TOWER", "BILLBOARD"};
static const int oscore[O_NTYPES] = {100, 400, 10, 5, 15, 200, 20, 10, 250, 75};
static const bool osolid[O_NTYPES] = {true, true, false, false, false, true, false, false, true, false};

typedef struct {
    unsigned char type, face;
    bool alive;
    float x, z, ax, az, h, hp, lw, ld, fall, tiltA;
    Color c;
    int seen;
} Obj;
static Obj objs[MAXOBJ];
static int nobj;

typedef struct { const char *name; float spd, sx, sy, sz; int hp; float rad, hgt; int score; } CarType;   // the ironroad cars + big vehicles
enum { CT_BUS = 3, CT_TRUCK = 4, CT_TANKER = 5 };
static const CarType ctypes[6] = {
    {"BRAWLER", 1.00f, 1.00f, 1.00f, 1.00f, 40, 2.0f, 2.4f, 150},
    {"HORNET", 1.15f, 0.94f, 0.88f, 1.04f, 30, 2.0f, 2.4f, 150},
    {"CRUSHER", 0.85f, 1.12f, 1.18f, 0.97f, 70, 2.2f, 2.6f, 150},
    {"BUS", 0.70f, 1, 1, 1, 150, 3.9f, 3.6f, 250},
    {"TRUCK", 0.80f, 1, 1, 1, 110, 3.3f, 4.2f, 200},
    {"FUEL TANKER", 0.75f, 1, 1, 1, 90, 3.3f, 3.6f, 300},
};
static const Color carPalette[8] = {{40,120,255,255},{230,50,50,255},{235,200,40,255},{40,200,120,255},{255,130,20,255},{160,70,210,255},{235,235,235,255},{70,70,80,255}};
typedef struct {
    bool alive, crushed;
    float go, blockT, shoved;
    float x, z, h, hp, spd, panic, roll, fireT, boomT, bt, t;
    int type, i, j, d;
    Color col;
} Car;
static Car cars[MAXCAR];

typedef struct { bool on; int type; Vector3 p, v; float life; int owner; } Proj;   // owner 0 player, 1 enemy;   // type 0 shell, 1 machine gun
static Proj proj[MAXPROJ];
typedef struct { Vector3 p, v; float life, max, size, g, grow; Color c; bool smoke; } Part;
static Part parts[MAXPART];
static int partHead;
typedef struct { Vector3 p; float t, max; int kind; } Emit;   // kind 0 fire+smoke, 1 water, 2 smoke
static Emit emits[MAXEMIT];
typedef struct { Vector3 p; float t, r; } Flash;
static Flash flashes[24];
typedef struct { float x, z, r; } Crater;
static Crater craters[120];
static int ncrater;

typedef struct { float x, z, h, spd, tur, pitch, hp, reload, mgT, odo, recoil; bool dead, mgOn; } Tank;
// up to 4 tanks: players[]. `cur` is the player being processed right now (the code below says `tank` for it);
// `me` is the player sitting at this keyboard.
#define MAXP 4
static Tank players[MAXP];
static bool pactive[MAXP];
static float respawnT[MAXP];
static int cur, me;
#define tank (players[cur])
typedef struct { float thr, steer; bool brake, cannon, mg; Vector3 aim; } Input;
static Input inp[MAXP];
#define aimPt (inp[cur].aim)
static const Color pcol[MAXP] = {{78, 92, 56, 255}, {62, 92, 150, 255}, {160, 128, 50, 255}, {128, 70, 140, 255}};
#define TANK_HP 600
#define ET_HP 220
#define MAXET 8
typedef struct { Tank t; bool used, counted; int i, j, d; float tt, alert, bt; } ETank;
static ETank ets[MAXET];
static bool scoreOn = true;   // false while resolving enemy fire: no points for what they break
static int etKills;
// chain reactions: anything destroyed by a secondary explosion (a tanker, fuel tank, train tank car...) pays a bonus
static int chainLevel;   // how many secondary explosions deep we are right now
static int chainMult(void) { return chainLevel <= 0 ? 1 : (chainLevel >= 3 ? 4 : chainLevel + 1); }
static const char *chainTag(void) { return chainLevel <= 0 ? "" : TextFormat("  CHAIN x%d", chainMult()); }

// trains run along two east-west streets, right down the middle of the road
#define NTSEG 7
#define TLEN 10.5f
#define TPER (2 * (HALFW + 40))
typedef struct { bool alive, moving; float hp, wx, wz, u, bt; int kind; } Seg;   // kind 0 loco, 1 boxcar, 2 tank car, 3 container flat
typedef struct { float z, dir, s, spd, hornT; Seg seg[NTSEG]; } Train;
static Train trains[2];
static float trainHitT;
static float segX(Train *t, int k) {   // wrecks (dead segments) keep their own world position and can be shoved around
    Seg *g = &t->seg[k];
    if (!g->alive) return g->wx;
    float u = fmodf(g->u, TPER);
    if (u < 0) u += TPER;
    return t->dir * (u - (HALFW + 40));
}
static float segZ(Train *t, int k) { return t->seg[k].alive ? t->z : t->seg[k].wz; }

static int score, kills[O_NTYPES], carKills;
static char msgs[6][64];
static float msgT[6];
static float shake, gameT;
static float camYaw, camOff, camDist = 26;
static bool paused, audioOK;
static unsigned char blockKind[N][N];
static Sound sHorn, sCannon, sMG, sExp, sCrunch, sEng, sPick;


// ---------------------------------------------------------------- network plumbing (protocol is in the networking section below)
// One machine hosts: it runs the whole simulation. Clients send their controls and draw what the host reports.
enum { M_SINGLE, M_HOST, M_CLIENT };
static int netMode = M_SINGLE;
#define isClient (netMode == M_CLIENT)
#define isHost (netMode == M_HOST)
static bool netApplying;   // a client is replaying a host event (explosions etc. only happen on a client then)
#define NET_VER 1
#define NET_PORT 7777
enum { P_HELLO = 1, P_INPUT = 2, P_WELCOME = 10, P_SNAPA = 11, P_SNAPB = 12, P_SNAPC = 13, P_REJECT = 14, P_EVENT = 20 };
enum { EV_EXPLODE = 1, EV_OBJKILL, EV_EMIT, EV_MSG, EV_FIRE, EV_RESTART };
typedef struct { unsigned char *b; int n, cap; } Wr;
static void w8(Wr *w, int v) { if (w->n < w->cap) w->b[w->n++] = (unsigned char)v; }
static void w16(Wr *w, int v) { w8(w, v & 255); w8(w, (v >> 8) & 255); }
static void w32(Wr *w, unsigned v) { w16(w, (int)(v & 0xffff)); w16(w, (int)(v >> 16)); }
static void wf(Wr *w, float f) { unsigned u; memcpy(&u, &f, 4); w32(w, u); }
typedef struct { const unsigned char *b; int n, len; } Rd;
static int r8(Rd *r) { return r->n < r->len ? r->b[r->n++] : 0; }
static int r16(Rd *r) { int a = r8(r); int b = r8(r); return a | (b << 8); }
static unsigned r32(Rd *r) { unsigned a = (unsigned)r16(r); unsigned b = (unsigned)r16(r); return a | (b << 16); }
static float rdf(Rd *r) { unsigned u = r32(r); float f; memcpy(&f, &u, 4); return f; }
#define EVBEGIN(t) unsigned char evb[128]; Wr ew = {evb, 0, sizeof evb}; w8(&ew, P_EVENT); w8(&ew, t)
#define EVSEND net_broadcast(1, true, evb, ew.n)
static void evExplode(float x, float y, float z, float r, bool shell) { EVBEGIN(EV_EXPLODE); wf(&ew, x); wf(&ew, y); wf(&ew, z); wf(&ew, r); w8(&ew, shell); EVSEND; }
static void evObjKill(int idx, float fx, float fz) { EVBEGIN(EV_OBJKILL); w16(&ew, idx); wf(&ew, fx); wf(&ew, fz); EVSEND; }
static void evEmit(Vector3 p, float dur, int kind) { EVBEGIN(EV_EMIT); wf(&ew, p.x); wf(&ew, p.y); wf(&ew, p.z); wf(&ew, dur); w8(&ew, kind); EVSEND; }
static void evMsg(const char *m) { EVBEGIN(EV_MSG); int n = (int)strlen(m); if (n > 60) n = 60; w8(&ew, n); for (int i = 0; i < n; i++) w8(&ew, m[i]); EVSEND; }
static void evFire(int kind, Vector3 m, float yaw) { EVBEGIN(EV_FIRE); w8(&ew, kind); wf(&ew, m.x); wf(&ew, m.y); wf(&ew, m.z); wf(&ew, yaw); EVSEND; }
static void evRestart(void) { EVBEGIN(EV_RESTART); EVSEND; }

// ---------------------------------------------------------------- helpers
static float rf(float a, float b) { return a + (b - a) * (GetRandomValue(0, 10000) / 10000.0f); }
static float angDiff(float a, float b) {
    float d = fmodf(b - a, 2 * PI);
    if (d > PI) d -= 2 * PI;
    if (d < -PI) d += 2 * PI;
    return d;
}
static Color shade(Color c, float k) {
    float r = c.r * k, g = c.g * k, b = c.b * k;
    return (Color){(unsigned char)fminf(255, r), (unsigned char)fminf(255, g), (unsigned char)fminf(255, b), c.a};
}
static void addMsg(const char *fmt, ...) {
    for (int i = 5; i > 0; i--) { memcpy(msgs[i], msgs[i - 1], 64); msgT[i] = msgT[i - 1]; }
    va_list a; va_start(a, fmt); vsnprintf(msgs[0], 64, fmt, a); va_end(a);
    msgT[0] = 4;
    if (isHost) evMsg(msgs[0]);
}
static float nodeX(int i) { return (i - N / 2.0f) * S; }
static float blockC(int i) { return (i + 0.5f - N / 2.0f) * S; }

// ---------------------------------------------------------------- audio (all synthesized, no files)
static Sound synth(float f0, float f1, float dur, float noise, float decay, int saw) {
    int n = (int)(22050 * dur);
    short *d = malloc(n * sizeof(short));
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / n, f = f0 + (f1 - f0) * t;
        ph += f / 22050.0f;
        float w = saw ? (2 * (ph - floorf(ph)) - 1) : sinf(ph * 6.2831853f);
        float nz = (float)GetRandomValue(-1000, 1000) / 1000.0f;
        d[i] = (short)((w * (1 - noise) + nz * noise) * expf(-decay * t) * 14000);
    }
    Wave w = {n, 22050, 16, 1, d};
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}
static Sound engineLoop(void) {
    int n = 22050;
    short *d = malloc(n * sizeof(short));
    for (int i = 0; i < n; i++) {
        float t = (float)i / 22050.0f, s = 0;
        for (int h = 1; h <= 6; h++) s += sinf(t * 6.2831853f * 30 * h) / h;
        d[i] = (short)(s * 4500);
    }
    Wave w = {n, 22050, 16, 1, d};
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}
static void snd(Sound s, float vol, float pitch) {
    if (!audioOK) return;
    SetSoundVolume(s, vol); SetSoundPitch(s, pitch); PlaySound(s);
}
static float distToTank(float x, float z) { return hypotf(x - players[me].x, z - players[me].z); }   // distance to the local player
static float vol(float x, float z) { return Clamp(1.2f - distToTank(x, z) / 220.0f, 0.05f, 1.0f); }

// ---------------------------------------------------------------- particles
static void part(Vector3 p, Vector3 v, float life, float size, Color c, float g, float grow, bool smoke) {
    Part *q = &parts[partHead++ % MAXPART];
    *q = (Part){p, v, life, life, size, g, grow, c, smoke};
}
static void flash(Vector3 p, float r) {
    for (int i = 0; i < 24; i++) if (flashes[i].t <= 0) { flashes[i] = (Flash){p, 0.3f, r}; return; }
}
static void debris(float x, float y, float z, int n, Color c, float spd) {
    for (int i = 0; i < n; i++) {
        Vector3 v = {rf(-1, 1), rf(0.3f, 1.4f), rf(-1, 1)};
        v = Vector3Scale(Vector3Normalize(v), rf(spd * 0.3f, spd));
        part((Vector3){x, y, z}, v, rf(0.8f, 2.0f), rf(0.2f, 0.7f), shade(c, rf(0.6f, 1.1f)), GRAV * 0.7f, 0, false);
    }
}
static void addEmit(Vector3 p, float dur, int kind) {
    if (isClient && !netApplying) return;   // the host tells us when to start a fire
    if (isHost) evEmit(p, dur, kind);
    for (int i = 0; i < MAXEMIT; i++) if (emits[i].t <= 0) { emits[i] = (Emit){p, dur, dur, kind}; return; }
}

// ---------------------------------------------------------------- spatial grid for static objects
#define GC 32.0f
#define GN 24
static int *cellList[GN][GN];
static int cellCnt[GN][GN];
static int queryId;
static int qbuf[2048];
static int cellOf(float v) { int c = (int)floorf((v + GN * GC / 2) / GC); return c < 0 ? 0 : (c >= GN ? GN - 1 : c); }
static void buildGrid(void) {
    for (int a = 0; a < GN; a++) for (int b = 0; b < GN; b++) { free(cellList[a][b]); cellList[a][b] = NULL; cellCnt[a][b] = 0; }
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < GN; a++) for (int b = 0; b < GN; b++) {
            if (pass == 0) continue;
            cellList[a][b] = malloc(sizeof(int) * (cellCnt[a][b] + 1)); cellCnt[a][b] = 0;
        }
        for (int k = 0; k < nobj; k++) {
            Obj *o = &objs[k];
            int x0 = cellOf(o->x - o->ax), x1 = cellOf(o->x + o->ax), z0 = cellOf(o->z - o->az), z1 = cellOf(o->z + o->az);
            for (int a = x0; a <= x1; a++) for (int b = z0; b <= z1; b++) {
                if (pass == 0) cellCnt[a][b]++; else cellList[a][b][cellCnt[a][b]++] = k;
            }
        }
    }
}
static int query(float x, float z, float r) {   // objects whose cell touches the circle; deduped
    int n = 0; queryId++;
    for (int a = cellOf(x - r); a <= cellOf(x + r); a++) for (int b = cellOf(z - r); b <= cellOf(z + r); b++)
        for (int k = 0; k < cellCnt[a][b]; k++) {
            int id = cellList[a][b][k];
            if (objs[id].seen == queryId || n >= 2048) continue;
            objs[id].seen = queryId; qbuf[n++] = id;
        }
    return n;
}
static float distToBox(Obj *o, float x, float z) {
    float dx = fmaxf(fabsf(x - o->x) - o->ax, 0), dz = fmaxf(fabsf(z - o->z) - o->az, 0);
    return hypotf(dx, dz);
}

// ---------------------------------------------------------------- damage
static void explode(float x, float y, float z, float radius, float dmg, bool shell);
static void damageET(ETank *e, float d);
static void damageSeg(Train *t, int k, float d);
static const float ohp[O_NTYPES] = {140, 450, 25, 12, 18, 40, 15, 10, 150, 60};

static void killObj(Obj *o, float fx, float fz) {
    if (isHost) evObjKill((int)(o - objs), fx, fz);
    o->alive = false; o->fall = 0;
    o->tiltA = atan2f(fz, fx);
    if (scoreOn && !isClient) {
        int pts = oscore[o->type] * chainMult();
        score += pts; kills[o->type]++;
        if (pts >= 100) addMsg("+%d %s%s", pts, oname[o->type], chainTag());
    }
    Vector3 c = {o->x, o->h * 0.4f, o->z};
    switch (o->type) {
    case O_HOUSE: case O_BLDG: {
        int n = o->type == O_BLDG ? 90 : 40;
        debris(o->x, o->h * 0.5f, o->z, n, o->c, 14);
        for (int i = 0; i < n; i++)
            part((Vector3){o->x + rf(-o->ax, o->ax), rf(0.5f, o->h * 0.7f), o->z + rf(-o->az, o->az)}, (Vector3){rf(-2, 2), rf(1, 5), rf(-2, 2)},
                 rf(2, 4.5f), rf(2, 5), (Color){150, 145, 135, 150}, 0, 1.2f, true);
        snd(sExp, 0.7f * vol(o->x, o->z), 0.6f);
        shake += 0.4f * vol(o->x, o->z);
        break;
    }
    case O_TREE:
        debris(o->x, o->h * 0.6f, o->z, 14, (Color){50, 130, 50, 255}, 7);
        snd(sCrunch, 0.5f * vol(o->x, o->z), rf(0.8f, 1.3f));
        break;
    case O_FENCE: debris(o->x, 0.6f, o->z, 8, o->c, 6); snd(sCrunch, 0.3f * vol(o->x, o->z), 1.6f); break;
    case O_LAMP: debris(o->x, 3, o->z, 6, (Color){90, 90, 95, 255}, 6); snd(sCrunch, 0.4f * vol(o->x, o->z), 1.0f); break;
    case O_HYDRANT: addEmit((Vector3){o->x, 0.9f, o->z}, 8, 1); break;
    case O_MAILBOX: debris(o->x, 1, o->z, 6, (Color){40, 80, 200, 255}, 6); break;
    case O_WATER:   // the tank bursts: a wall of water
        for (int i = 0; i < 160; i++) {
            float a = rf(0, 2 * PI), sp = rf(5, 20);
            part((Vector3){o->x, rf(9, 14), o->z}, (Vector3){cosf(a) * sp, rf(0, 8), sinf(a) * sp}, rf(1, 2.2f), rf(0.5f, 1.1f), (Color){140, 195, 255, 200}, GRAV * 0.6f, 0, false);
        }
        debris(o->x, 8, o->z, 30, (Color){120, 125, 130, 255}, 12);
        addEmit((Vector3){o->x, 6, o->z}, 5, 1);
        snd(sExp, 0.6f * vol(o->x, o->z), 0.5f); shake += 0.5f * vol(o->x, o->z);
        break;
    case O_BILL: debris(o->x, 6, o->z, 24, (Color){200, 200, 200, 255}, 10); snd(sCrunch, 0.5f * vol(o->x, o->z), 0.7f); break;
    case O_FUEL: {
        float k = o->lw / 6.0f;
        explode(o->x, 2, o->z, 10 + 16 * k, 120 + 160 * k, false);
        addEmit((Vector3){o->x, 1, o->z}, 12, 0);
        break;
    }
    }
    (void)c;
}
static void dmgObj(Obj *o, float d, float fx, float fz) {
    if (!o->alive) return;
    o->hp -= d;
    if (o->hp <= 0) killObj(o, fx, fz);
}
static void killCar(Car *c, bool crushed) {
    if (!c->alive) return;
    c->alive = false; c->crushed = crushed;
    c->roll = rf(-0.5f, 0.5f);
    if (scoreOn && !isClient) { int pts = ctypes[c->type].score * chainMult(); score += pts; carKills++; addMsg("+%d %s %s%s", pts, ctypes[c->type].name, crushed ? "CRUSHED" : "DESTROYED", chainTag()); }
    debris(c->x, 1, c->z, 25, c->col, 12);
    snd(sExp, 0.6f * vol(c->x, c->z), 1.2f);
    c->fireT = GetRandomValue(0, 10) < 7 ? rf(8, 22) : 0;
    c->boomT = (c->fireT > 0 && GetRandomValue(0, 10) < 5) ? rf(2.5f, 6) : 0;
    if (c->type == CT_TANKER) { c->fireT = 18; c->boomT = rf(0.3f, 1.0f); }
    flash((Vector3){c->x, 1.2f, c->z}, 4);
    for (int i = 0; i < 10; i++)
        part((Vector3){c->x, 1, c->z}, (Vector3){rf(-3, 3), rf(2, 7), rf(-3, 3)}, rf(1, 2.5f), rf(1, 2), (Color){255, 140, 20, 220}, 0, -0.3f, true);
}
static void dmgCar(Car *c, float d) {
    if (!c->alive) return;
    c->hp -= d; c->panic = 6;
    if (c->hp <= 0) killCar(c, false);
}
static void damageTank(float d) {
    if (tank.dead) return;
    tank.hp -= d; shake += 0.2f;
    if (tank.hp <= 0) { tank.dead = true; tank.spd = 0; respawnT[cur] = 6; explode(tank.x, 1.5f, tank.z, 10, 0, false); addEmit((Vector3){tank.x, 1.5f, tank.z}, 20, 0); }
}
static void explode(float x, float y, float z, float radius, float dmg, bool shell) {
    if (isClient && !netApplying) return;   // clients only show explosions the host reports
    if (isHost) evExplode(x, y, z, radius, shell);
    int n = (int)(18 + radius * 3);
    for (int i = 0; i < n; i++) {
        Vector3 v = Vector3Scale(Vector3Normalize((Vector3){rf(-1, 1), rf(0.1f, 1.3f), rf(-1, 1)}), rf(3, 10 + radius));
        Color col = i % 3 == 0 ? YELLOW : (i % 3 == 1 ? ORANGE : RED);
        part((Vector3){x, y, z}, v, rf(0.4f, 1.1f), rf(0.5f, 1.4f), col, 0, -0.8f, true);
    }
    for (int i = 0; i < n / 2; i++)
        part((Vector3){x + rf(-1, 1), y, z + rf(-1, 1)}, (Vector3){rf(-3, 3), rf(2, 7), rf(-3, 3)}, rf(1.2f, 2.8f), rf(1, 2.2f), (Color){60, 60, 60, 210}, 0, 1.5f, true);
    flash((Vector3){x, y, z}, radius * 0.7f);
    snd(sExp, (0.5f + radius * 0.03f) * vol(x, z), Clamp(1.4f - radius * 0.04f, 0.5f, 1.2f));
    shake += Clamp(radius * 0.03f, 0.1f, 1.0f) * vol(x, z);
    if (y < 4 && radius >= 6) { craters[ncrater++ % 120] = (Crater){x, z, radius * 0.45f}; }
    if (dmg <= 0) return;
    if (!shell) chainLevel++;   // everything this blast destroys counts as a chain reaction
    int cnt = query(x, z, radius + 30);
    for (int k = 0; k < cnt; k++) {
        Obj *o = &objs[qbuf[k]];
        if (!o->alive) continue;
        float d = distToBox(o, x, z);
        if (d < radius) dmgObj(o, dmg * (1 - d / radius * 0.7f), o->x - x, o->z - z);
    }
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &cars[i];
        float d = hypotf(c->x - x, c->z - z);
        if (d < 70) c->panic = 8;
        if (c->alive && d < radius + ctypes[c->type].rad) dmgCar(c, dmg * (1 - d / (radius + ctypes[c->type].rad) * 0.6f));
    }
    if (dmg > 0) for (int ti = 0; ti < 2; ti++) for (int k = 0; k < NTSEG; k++) {
        Train *tr = &trains[ti];
        if (!tr->seg[k].alive) continue;
        float ds = hypotf(segX(tr, k) - x, segZ(tr, k) - z);
        if (ds < radius + 3) damageSeg(tr, k, dmg * (1 - ds / (radius + 3) * 0.6f));
    }
    if (dmg > 0) {
        int sv = cur;
        for (int p = 0; p < MAXP; p++) {
            if (!pactive[p] || players[p].dead) continue;
            cur = p;
            float d = hypotf(x - tank.x, z - tank.z);
            if (d < radius + 2) damageTank(dmg * ((shell && !scoreOn) ? 0.3f : 0.15f) * (1 - d / (radius + 2)));
        }
        cur = sv;
    }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        if (!e->used || e->t.dead) continue;
        float de = hypotf(e->t.x - x, e->t.z - z);
        if (de < radius + 2 && (!shell || scoreOn)) damageET(e, dmg * (shell ? 0.5f : 0.3f) * (1 - de / (radius + 2)));
    }
    if (!shell) chainLevel--;
}

// ---------------------------------------------------------------- world generation
static void addObj(OType type, float x, float z, float lw, float ld, float h, int face, Color c) {
    if (nobj >= MAXOBJ) return;
    Obj *o = &objs[nobj++];
    memset(o, 0, sizeof(*o));
    o->type = type; o->face = face; o->alive = true; o->x = x; o->z = z; o->lw = lw; o->ld = ld; o->h = h; o->c = c;
    bool odd = face & 1;
    o->ax = (odd ? ld : lw) / 2; o->az = (odd ? lw : ld) / 2;
    o->hp = ohp[type];
    if (type == O_HOUSE) o->hp = 90 + lw * 4;
    if (type == O_BLDG) o->hp = 200 + h * 9;
    if (type == O_FUEL) o->hp = 25 + lw * 6;
}
static const Color houseCols[] = {{205,180,140,255},{190,90,80,255},{225,225,215,255},{120,150,190,255},{210,190,90,255},{150,175,130,255}};
static const Color bldgCols[] = {{150,155,165,255},{110,125,150,255},{170,150,125,255},{125,140,130,255},{185,185,190,255}};
static void tree(float x, float z) { addObj(O_TREE, x, z, 3.0f, 3.0f, rf(5.5f, 9), 0, (Color){(unsigned char)GetRandomValue(30, 70), (unsigned char)GetRandomValue(110, 160), (unsigned char)GetRandomValue(30, 60), 255}); }
static void lotFence(float lx, float lz, float half, int gateFace) {
    float step = 2 * half / 5;
    for (int side = 0; side < 4; side++) for (int s = 0; s < 5; s++) {
        float u = -half + step * (s + 0.5f);
        if (side == gateFace && s == 2) continue;   // gate
        float x = lx, z = lz; int face;
        switch (side) {   // 0:+z 1:+x 2:-z 3:-x
        case 0: x += u; z += half; face = 0; break;
        case 1: x += half; z += u; face = 1; break;
        case 2: x += u; z -= half; face = 0; break;
        default: x -= half; z += u; face = 1; break;
        }
        addObj(O_FENCE, x, z, step * 0.96f, 0.3f, 1.3f, face, (Color){235, 232, 220, 255});
    }
}
static void spawnPlayer(int p) {
    Tank *t = &players[p];
    memset(t, 0, sizeof(*t));
    t->x = nodeX(4) + (p % 2 ? 3.5f : -3.5f); t->z = nodeX(1) - (p / 2) * 10.0f; t->hp = TANK_HP;
    respawnT[p] = 0;
}
static void genWorld(void) {
    nobj = 0; memset(cars, 0, sizeof(cars)); memset(proj, 0, sizeof(proj)); memset(parts, 0, sizeof(parts));
    memset(emits, 0, sizeof(emits)); memset(flashes, 0, sizeof(flashes)); ncrater = 0; memset(craters, 0, sizeof(craters));
    SetRandomSeed(4242);
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        float cx = blockC(i), cz = blockC(j);
        int dc = abs(i - N / 2) > abs(j - N / 2) ? abs(i - N / 2) : abs(j - N / 2);
        int kind = 0;   // 0 suburb 1 downtown 2 park 3 depot 4 gas 5 midrise
        if (dc <= 1) kind = 1;
        else if (dc == 2) kind = GetRandomValue(0, 1) ? 5 : 0;
        if ((i == 1 && j == 1) || (i == 7 && j == 7) || (i == 7 && j == 3)) kind = 2;
        if (i == 7 && j == 1) kind = 3;
        if ((i == 1 && j == 6) || (i == 6 && j == 7)) kind = 4;
        blockKind[i][j] = kind;
        if ((kind == 0 || kind == 5) && GetRandomValue(0, 100) < 14) {   // billboard along the sidewalk
            int side = GetRandomValue(0, 3);
            float off = rf(-10, 10);
            float bx = side < 2 ? cx + off : cx + (side == 2 ? 27.4f : -27.4f), bz = side < 2 ? cz + (side == 0 ? 27.4f : -27.4f) : cz + off;
            addObj(O_BILL, bx, bz, 11, 0.7f, 11.2f, side < 2 ? 0 : 1, WHITE);
        }
        // lamps on the corners
        for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2)
            addObj(O_LAMP, cx + a * 27, cz + b * 27, 0.5f, 0.5f, 7.5f, 0, (Color){80, 80, 85, 255});
        for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) {
            float lx = cx + sx * 14, lz = cz + sz * 14;
            int face = GetRandomValue(0, 1) ? (sx > 0 ? 1 : 3) : (sz > 0 ? 0 : 2);
            if (kind == 1 || kind == 5) {
                float w = kind == 1 ? rf(18, 24) : rf(14, 21), d = kind == 1 ? rf(18, 24) : rf(14, 21);
                float h = kind == 1 ? rf(18, 46) : rf(8, 19);
                if (kind == 1 && abs(i - N / 2) + abs(j - N / 2) == 0) h += 16;
                addObj(O_BLDG, lx, lz, w, d, h, 0, bldgCols[GetRandomValue(0, 4)]);
                if (kind == 5) { tree(lx + sx * 12.5f, lz - sz * 12.5f); }
            } else if (kind == 2) {
                for (int t = 0; t < 8; t++) tree(lx + rf(-12, 12), lz + rf(-12, 12));
                addObj(O_HYDRANT, lx + rf(-3, 3), lz + rf(-3, 3), 0.9f, 0.9f, 1.2f, 0, (Color){210, 40, 40, 255});
            } else if (kind == 3) {
                for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2)
                    addObj(O_FUEL, lx + a * 6.5f, lz + b * 6.5f, 6, 6, 6, 0, (Color){215, 215, 210, 255});
            } else {
                if (kind == 4 && sx < 0 && sz < 0) {   // gas station: shop, pumps, one big tank
                    addObj(O_HOUSE, lx, lz + 6, 12, 8, 4.5f, 0, (Color){230, 230, 235, 255});
                    for (int p = 0; p < 3; p++) addObj(O_FUEL, lx - 6 + p * 6, lz - 6, 1.6f, 1.6f, 2.2f, 0, (Color){200, 40, 40, 255});
                    addObj(O_FUEL, lx + 9, lz - 9, 5, 5, 5, 0, (Color){220, 220, 220, 255});
                    continue;
                }
                if (kind == 0 && GetRandomValue(0, 100) < 4) {
                    addObj(O_WATER, lx, lz, 6.8f, 6.8f, 17, 0, WHITE);
                    lotFence(lx, lz, 12, face);
                    tree(lx + 9, lz - 9); tree(lx - 9, lz + 9);
                    continue;
                }
                bool houseHere = GetRandomValue(0, 100) > 10;
                float hw = rf(10, 14), hd = rf(9, 12);
                if (houseHere) addObj(O_HOUSE, lx, lz, hw, hd, rf(6, 8.5f), face, houseCols[GetRandomValue(0, 5)]);
                if (GetRandomValue(0, 100) < 65) lotFence(lx, lz, 12, face);
                int nt = GetRandomValue(2, 4);
                for (int t = 0; t < nt; t++) {
                    float tx = lx + rf(-11, 11), tz = lz + rf(-11, 11);
                    if (houseHere && fabsf(tx - lx) < hw / 2 + 2 && fabsf(tz - lz) < hd / 2 + 2) continue;
                    tree(tx, tz);
                }
                static const float fdx[4] = {0, 1, 0, -1}, fdz[4] = {1, 0, -1, 0};
                addObj(O_MAILBOX, lx + fdx[face] * 13 + fdz[face] * 3, lz + fdz[face] * 13 + fdx[face] * 3, 0.6f, 0.6f, 1.2f, 0, (Color){40, 80, 200, 255});
            }
        }
    }
    buildGrid();
    // traffic
    for (int k = 0; k < 52; k++) {
        Car *c = &cars[k];
        c->alive = true; c->type = k < 6 ? CT_BUS : k < 11 ? CT_TRUCK : k < 15 ? CT_TANKER : GetRandomValue(0, 2);
        c->col = carPalette[GetRandomValue(0, 7)];
        if (c->type == CT_BUS) c->col = k % 2 ? (Color){235, 200, 40, 255} : (Color){40, 110, 200, 255};
        if (c->type == CT_TANKER) c->col = (Color){185, 190, 196, 255};
        c->hp = ctypes[c->type].hp;
        c->i = GetRandomValue(0, N); c->j = GetRandomValue(0, N); c->d = GetRandomValue(0, 3);
        for (int g = 0; g < 4; g++) {   // face a valid direction
            static const int dxs[4] = {1, 0, -1, 0}, dzs[4] = {0, 1, 0, -1};
            int ni = c->i + dxs[c->d], nj = c->j + dzs[c->d];
            if (ni >= 0 && ni <= N && nj >= 0 && nj <= N) break;
            c->d = (c->d + 1) % 4;
        }
        c->go = 1; c->t = rf(0, 1); c->spd = rf(9, 14) * ctypes[c->type].spd;
        c->x = nodeX(c->i); c->z = nodeX(c->j);
        c->h = c->d == 0 ? PI / 2 : c->d == 1 ? 0 : c->d == 2 ? -PI / 2 : PI;
    }
    for (int ti = 0; ti < 2; ti++) {
        Train *t = &trains[ti];
        memset(t, 0, sizeof(*t));
        t->z = nodeX(ti == 0 ? 5 : 3); t->dir = ti == 0 ? 1 : -1; t->spd = ti == 0 ? 12 : 10; t->s = rf(0, TPER);
        static const int kinds[NTSEG] = {0, 1, 2, 3, 1, 2, 3};
        for (int k = 0; k < NTSEG; k++) t->seg[k] = (Seg){true, true, k == 0 ? 320 : 170, 0, 0, t->s - k * TLEN, rf(0, 0.1f), kinds[k]};
    }
    memset(players, 0, sizeof(players));
    memset(ets, 0, sizeof(ets)); etKills = 0; scoreOn = true;
    for (int k = 0; k < 5; k++) {
        ETank *e = &ets[k];
        for (;;) {
            e->i = GetRandomValue(0, N); e->j = GetRandomValue(0, N);
            if (hypotf(nodeX(e->i) - nodeX(4), nodeX(e->j) - nodeX(1)) > 170) break;
        }
        static const int dxs[4] = {1, 0, -1, 0}, dzs[4] = {0, 1, 0, -1};
        e->d = GetRandomValue(0, 3);
        for (int g = 0; g < 4; g++) {
            int ni = e->i + dxs[e->d], nj = e->j + dzs[e->d];
            if (ni >= 0 && ni <= N && nj >= 0 && nj <= N) break;
            e->d = (e->d + 1) % 4;
        }
        e->used = true; e->tt = rf(0, 0.8f);
        e->t.x = nodeX(e->i); e->t.z = nodeX(e->j); e->t.hp = ET_HP; e->t.reload = rf(4, 7);
        e->t.h = e->t.tur = e->d == 0 ? PI / 2 : e->d == 1 ? 0 : e->d == 2 ? -PI / 2 : PI;
    }
    for (int p = 0; p < MAXP; p++) { cur = p; spawnPlayer(p); }
    cur = me;
    camYaw = 0; camOff = 0;
    score = 0; carKills = 0; memset(kills, 0, sizeof(kills)); gameT = 0;
}

// ---------------------------------------------------------------- traffic
static void carTarget(Car *c, float *tx, float *tz) {
    static const int dxs[4] = {1, 0, -1, 0}, dzs[4] = {0, 1, 0, -1};
    float ax = nodeX(c->i), az = nodeX(c->j), bx = nodeX(c->i + dxs[c->d]), bz = nodeX(c->j + dzs[c->d]);
    *tx = ax + (bx - ax) * c->t + dzs[c->d] * 3.4f;
    *tz = az + (bz - az) * c->t - dxs[c->d] * 3.4f;
}
static void deadCarFx(Car *c, float dt) {   // burning wrecks: smoke, and the occasional explosion
    if (c->fireT > 0) {
        c->fireT -= dt; c->bt -= dt;
        if (c->bt <= 0) {
            c->bt = 0.07f;
            part((Vector3){c->x + rf(-.8f, .8f), 1.2f, c->z + rf(-.8f, .8f)}, (Vector3){rf(-.5f, .5f), rf(2, 4), rf(-.5f, .5f)}, rf(.6f, 1.2f), rf(.5f, 1), (Color){255, 150, 30, 220}, 0, -.4f, true);
            part((Vector3){c->x, 1.6f, c->z}, (Vector3){rf(-.6f, .6f), rf(2, 3.5f), rf(-.6f, .6f)}, rf(1.5f, 3), rf(.8f, 1.4f), (Color){50, 50, 50, 170}, 0, 1.2f, true);
        }
    }
    if (c->boomT > 0) { c->boomT -= dt; if (c->boomT <= 0) { if (c->type == CT_TANKER) explode(c->x, 1.5f, c->z, 22, 240, false); else explode(c->x, 1.2f, c->z, 9, 90, false); c->fireT = 0; c->roll = rf(-1.2f, 1.2f); } }
}
// is there something solid at this spot that a driver would not want to plough into?
static bool carBlockedAt(Car *self, float px, float pz) {
    for (int p = 0; p < MAXP; p++) {
        if (!pactive[p]) continue;
        float fx = sinf(players[p].h), fz = cosf(players[p].h);
        for (int s = -1; s <= 1; s += 2) if (hypotf(px - (players[p].x + fx * 2.0f * s), pz - (players[p].z + fz * 2.0f * s)) < TANK_R + 1.3f) return true;
    }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        if (!e->used) continue;
        float ex = sinf(e->t.h), ez = cosf(e->t.h);
        for (int s = -1; s <= 1; s += 2) if (hypotf(px - (e->t.x + ex * 2.0f * s), pz - (e->t.z + ez * 2.0f * s)) < TANK_R + 1.3f) return true;
    }
    for (int ti = 0; ti < 2; ti++) for (int k = 0; k < NTSEG; k++) {   // trains (looking ahead of a moving one) and wrecks
        Train *t = &trains[ti];
        float x = segX(t, k), lo = x, hi = x;
        if (t->seg[k].alive && t->seg[k].moving) { float dx = t->dir * t->spd * 1.6f; lo = fminf(x, x + dx); hi = fmaxf(x, x + dx); }
        if (px > lo - 6.1f && px < hi + 6.1f && fabsf(pz - segZ(t, k)) < 3.1f) return true;
    }
    for (int j = 0; j < MAXCAR; j++) {
        Car *o = &cars[j];
        if (o == self || (o->x == 0 && o->z == 0)) continue;
        if ((!o->alive || o->d == self->d) && hypotf(px - o->x, pz - o->z) < ctypes[o->type].rad + 1.0f) return true;
    }
    return false;
}
static void updateCars(float dt) {
    static const int dxs[4] = {1, 0, -1, 0}, dzs[4] = {0, 1, 0, -1};
    for (int k = 0; k < MAXCAR; k++) {
        Car *c = &cars[k];
        if (!c->alive) { deadCarFx(c, dt); continue; }
        c->panic = fmaxf(0, c->panic - dt);
        {   // look down the road and brake for anything in the way; give up and turn around if it stays blocked
            static const float samp[5] = {5, 9, 13, 18, 24}, tgt[5] = {0, 0, 0.35f, 0.6f, 0.85f};
            float fx = sinf(c->h), fz = cosf(c->h), want = 1;
            for (int q = 0; q < 5; q++) if (carBlockedAt(c, c->x + fx * samp[q], c->z + fz * samp[q])) { want = tgt[q]; break; }
            c->go += (want - c->go) * fminf(1, 5 * dt);
            c->blockT = want < 0.05f ? c->blockT + dt : 0;
            if (c->blockT > 10) {
                c->i += dxs[c->d]; c->j += dzs[c->d]; c->d = (c->d + 2) % 4; c->t = 1 - c->t; c->blockT = 0;
            }
        }
        if (c->shoved > 0) { c->shoved -= dt; c->go = 0; continue; }
        float sp = c->spd * (c->panic > 0 ? 1.7f : 1) * c->go;
        c->t += sp * dt / S;
        if (c->t >= 1) {
            c->t -= 1; c->i += dxs[c->d]; c->j += dzs[c->d];
            int opts[4], w[4], n = 0, tot = 0;
            for (int d = 0; d < 4; d++) {
                if (d == (c->d + 2) % 4) continue;
                int ni = c->i + dxs[d], nj = c->j + dzs[d];
                if (ni < 0 || ni > N || nj < 0 || nj > N) continue;
                opts[n] = d; w[n] = d == c->d ? 5 : 2; tot += w[n]; n++;
            }
            if (n == 0) { c->d = (c->d + 2) % 4; }
            else { int r = GetRandomValue(0, tot - 1), a = 0; for (int q = 0; q < n; q++) { a += w[q]; if (r < a) { c->d = opts[q]; break; } } }
        }
        float tx, tz; carTarget(c, &tx, &tz);
        float ease = fminf(1, 6 * dt);
        float px = c->x, pz = c->z;
        c->x += (tx - c->x) * ease; c->z += (tz - c->z) * ease;
        float vx = c->x - px, vz = c->z - pz;
        if (vx * vx + vz * vz > 1e-6f) c->h += angDiff(c->h, atan2f(vx, vz)) * fminf(1, 8 * dt);
        // small cars get squashed by the tank; big vehicles are shoved aside instead (see vehiclesCollide)
        if (c->type < CT_BUS) for (int p = 0; p < MAXP && c->alive; p++) {
            if (!pactive[p] || players[p].dead) continue;
            for (int s = -1; s <= 1; s += 2) {
                float cx = players[p].x + sinf(players[p].h) * 2.0f * s, cz = players[p].z + cosf(players[p].h) * 2.0f * s;
                if (hypotf(c->x - cx, c->z - cz) < TANK_R + ctypes[c->type].rad * 0.95f) { killCar(c, true); break; }
            }
        }
    }
}

// ---------------------------------------------------------------- tank
static Vector3 muzzle(void) {
    return (Vector3){tank.x + sinf(tank.tur) * 4.2f, 2.5f, tank.z + cosf(tank.tur) * 4.2f};
}
static void fireShell(void) {
    Vector3 m = muzzle();
    float d = hypotf(aimPt.x - m.x, aimPt.z - m.z);
    float s = Clamp(GRAV * d / (SHELL_V * SHELL_V), 0, 1);
    float th = 0.5f * asinf(s);
    Vector3 v = {sinf(tank.tur) * cosf(th) * SHELL_V, sinf(th) * SHELL_V, cosf(tank.tur) * cosf(th) * SHELL_V};
    for (int i = 0; i < MAXPROJ; i++) if (!proj[i].on) { proj[i] = (Proj){true, 0, m, v, 6}; break; }
    tank.reload = 1.3f; tank.recoil = 1;
    if (isHost) evFire(0, m, tank.tur);
    snd(sCannon, 0.9f, rf(0.95f, 1.05f));
    shake += 0.25f;
    for (int i = 0; i < 14; i++)
        part(m, (Vector3){sinf(tank.tur) * rf(4, 14) + rf(-3, 3), rf(-1, 3), cosf(tank.tur) * rf(4, 14) + rf(-3, 3)}, rf(.3f, .8f), rf(.4f, 1), i % 2 ? ORANGE : (Color){90, 90, 90, 200}, 0, 1, true);
    tank.spd -= 0.8f;
}
static void fireMG(void) {
    Vector3 m = {tank.x + sinf(tank.tur) * 1.6f, 2.35f, tank.z + cosf(tank.tur) * 1.6f};
    m.x += cosf(tank.tur) * 0.7f; m.z -= sinf(tank.tur) * 0.7f;   // coax mount, right of the barrel
    Vector3 tgt = {aimPt.x, 0.9f, aimPt.z};
    Vector3 dir = Vector3Normalize(Vector3Subtract(tgt, m));
    dir = Vector3Normalize(Vector3Add(dir, (Vector3){rf(-.012f, .012f), rf(-.008f, .008f), rf(-.012f, .012f)}));
    for (int i = 0; i < MAXPROJ; i++) if (!proj[i].on) { proj[i] = (Proj){true, 1, m, Vector3Scale(dir, 230), 1.0f}; break; }
    tank.mgT = 0.075f;
    snd(sMG, 0.25f, rf(0.9f, 1.3f));
}
static void trainCollide(float dt);
static void vehiclesCollide(float dt);
static void updateTank(float dt) {
    Tank *t = &tank;
    if (t->dead) { t->spd *= 1 - 3 * dt; t->mgOn = false; return; }
    float thr = Clamp(inp[cur].thr, -1, 1), st = Clamp(inp[cur].steer, -1, 1);
    if (thr != 0) t->spd += thr * (thr * t->spd < 0 ? 40 : 14) * dt;
    else t->spd -= Clamp(t->spd, -8 * dt, 8 * dt);
    if (inp[cur].brake && thr == 0) t->spd *= 1 - 6 * dt;
    t->spd = Clamp(t->spd, -7, 16);
    t->h += st * 1.35f * dt;
    float fx = sinf(t->h), fz = cosf(t->h);
    t->x += fx * t->spd * dt; t->z += fz * t->spd * dt;
    t->odo += t->spd * dt + fabsf(st) * 2 * dt;
    // collisions: two circles along the hull
    for (int pass = 0; pass < 2; pass++) {
        int cnt = query(t->x, t->z, 8);
        for (int s = -1; s <= 1; s += 2) {
            float cx = t->x + fx * 2.0f * s, cz = t->z + fz * 2.0f * s;
            for (int k = 0; k < cnt; k++) {
                Obj *o = &objs[qbuf[k]];
                if (!o->alive) continue;
                float nx = Clamp(cx, o->x - o->ax, o->x + o->ax), nz = Clamp(cz, o->z - o->az, o->z + o->az);
                float dx = cx - nx, dz = cz - nz, d2 = dx * dx + dz * dz;
                float r = TANK_R + (o->type == O_TREE ? -1.0f : 0);
                if (d2 >= r * r) continue;
                if (osolid[o->type]) {
                    float d = sqrtf(d2), px, pz;
                    if (d < 1e-4f) { px = cx - o->x; pz = cz - o->z; d = hypotf(px, pz) + 1e-4f; dx = px / d; dz = pz / d; d = 0; }
                    else { dx /= d; dz /= d; }
                    float push = r - d;
                    t->x += dx * push; t->z += dz * push; cx += dx * push; cz += dz * push;
                    float closing = fabsf(t->spd);
                    t->spd *= 1 - fminf(1, 5 * dt);
                    if (pass == 0) dmgObj(o, closing * 28 * dt * (o->type == O_FUEL ? 0.3f : 1), dx * -1, dz * -1);
                } else {
                    dmgObj(o, 1e6f, o->x - cx, o->z - cz);   // crushed under the tracks
                    t->spd *= 1 - fminf(1, 0.6f * dt);
                }
            }
        }
    }
    for (int p = 0; p < MAXP; p++) {   // tanks can't drive through each other
        if (p == cur || !pactive[p] || players[p].dead) continue;
        float dx = t->x - players[p].x, dz = t->z - players[p].z, dd = hypotf(dx, dz);
        if (dd < 5.2f && dd > 1e-3f) { t->x += dx / dd * (5.2f - dd) * 0.5f; t->z += dz / dd * (5.2f - dd) * 0.5f; players[p].x -= dx / dd * (5.2f - dd) * 0.5f; players[p].z -= dz / dd * (5.2f - dd) * 0.5f; }
    }
    trainCollide(dt);
    vehiclesCollide(dt);
    float lim = HALFW + ROAD / 2 - 3;
    t->x = Clamp(t->x, -lim, lim); t->z = Clamp(t->z, -lim, lim);
    // turret follows the mouse
    float want = atan2f(aimPt.x - t->x, aimPt.z - t->z);
    float da = angDiff(t->tur, want), mx = 2.6f * dt;
    t->tur += Clamp(da, -mx, mx);
    float d = hypotf(aimPt.x - t->x, aimPt.z - t->z);
    float wp = 0.5f * asinf(Clamp(GRAV * d / (SHELL_V * SHELL_V), 0, 1));
    t->pitch += (wp - t->pitch) * fminf(1, 6 * dt);
    t->reload -= dt; t->mgT -= dt; t->recoil = fmaxf(0, t->recoil - 4 * dt);
    if (inp[cur].cannon && t->reload <= 0) fireShell();
    t->mgOn = inp[cur].mg;
    if (inp[cur].mg && t->mgT <= 0) fireMG();
    // dust
    if (fabsf(t->spd) > 3 && GetRandomValue(0, 3) == 0)
        part((Vector3){t->x - fx * 3.4f * (t->spd > 0 ? 1 : -1) + rf(-1.5f, 1.5f), 0.3f, t->z - fz * 3.4f * (t->spd > 0 ? 1 : -1)}, (Vector3){rf(-.5f, .5f), rf(.5f, 1.5f), rf(-.5f, .5f)}, rf(.6f, 1.2f), rf(.6f, 1.2f), (Color){170, 160, 140, 110}, 0, 1.5f, true);
}


// ---------------------------------------------------------------- enemy tanks
static float enemyCool;   // minimum gap between shots from any enemy, so a group can't volley
static void damageET(ETank *e, float d) {
    if (!e->used || e->t.dead) return;
    e->t.hp -= d; e->alert = 20;
    if (e->t.hp <= 0) {
        e->t.dead = true; e->t.spd = 0;
        if (scoreOn && !isClient) { int pts = 500 * chainMult(); score += pts; etKills++; addMsg("+%d ENEMY TANK DESTROYED%s", pts, chainTag()); }
        explode(e->t.x, 1.5f, e->t.z, 10, 0, false);
        addEmit((Vector3){e->t.x, 1.5f, e->t.z}, 25, 0);
        debris(e->t.x, 2, e->t.z, 30, (Color){70, 80, 60, 255}, 16);
    }
}
static void fireEnemy(ETank *e) {
    Tank *t = &e->t;
    Vector3 m = {t->x + sinf(t->tur) * 4.2f, 2.5f, t->z + cosf(t->tur) * 4.2f};
    float d = hypotf(tank.x - m.x, tank.z - m.z);
    float flight = d / SHELL_V * 0.7f;
    float tx = tank.x + sinf(tank.h) * tank.spd * flight, tz = tank.z + cosf(tank.h) * tank.spd * flight;
    float dd = hypotf(tx - m.x, tz - m.z) * rf(0.9f, 1.1f);
    float th = 0.5f * asinf(Clamp(GRAV * dd / (SHELL_V * SHELL_V), 0, 1));
    float yaw = t->tur + rf(-0.05f, 0.05f);
    Vector3 v = {sinf(yaw) * cosf(th) * SHELL_V, sinf(th) * SHELL_V, cosf(yaw) * cosf(th) * SHELL_V};
    for (int i = 0; i < MAXPROJ; i++) if (!proj[i].on) { proj[i] = (Proj){true, 0, m, v, 6, 1}; break; }
    t->reload = rf(4.5f, 6.5f); t->recoil = 1; enemyCool = 1.8f;
    if (isHost) evFire(1, m, t->tur);
    snd(sCannon, 0.8f * vol(m.x, m.z), rf(0.85f, 0.95f));
    for (int i = 0; i < 10; i++)
        part(m, (Vector3){sinf(t->tur) * rf(4, 12) + rf(-3, 3), rf(-1, 3), cosf(t->tur) * rf(4, 12) + rf(-3, 3)}, rf(.3f, .8f), rf(.4f, 1), i % 2 ? ORANGE : (Color){90, 90, 90, 200}, 0, 1, true);
}
static void updateETanks(float dt) {
    enemyCool -= dt;
    static const int dxs[4] = {1, 0, -1, 0}, dzs[4] = {0, 1, 0, -1};
    for (int k = 0; k < MAXET; k++) {
        ETank *e = &ets[k];
        Tank *t = &e->t;
        if (!e->used) continue;
        if (t->dead) {
            t->recoil = 0;
            continue;
        }
        int sv = cur, tp = -1; float bestD = 1e9f;   // hunt the nearest living player
        for (int p = 0; p < MAXP; p++) {
            if (!pactive[p] || players[p].dead) continue;
            float dp = hypotf(players[p].x - t->x, players[p].z - t->z);
            if (dp < bestD) { bestD = dp; tp = p; }
        }
        if (tp < 0) { tp = 0; for (int p = 0; p < MAXP; p++) if (pactive[p]) { tp = p; break; } }
        cur = tp;
        float dist = hypotf(tank.x - t->x, tank.z - t->z);
        if (dist < 110 && !tank.dead) e->alert = 20;
        e->alert = fmaxf(0, e->alert - dt);
        bool engaged = e->alert > 0 && dist < 190 && !tank.dead;
        bool hold = engaged && dist < 70;
        t->reload -= dt; t->recoil = fmaxf(0, t->recoil - 4 * dt);
        if (!hold) {
            float want = e->alert > 0 ? 9.0f : 6.0f;
            t->spd += (want - t->spd) * fminf(1, 2 * dt);
            e->tt += t->spd * dt / S;
        } else t->spd *= 1 - fminf(1, 4 * dt);
        if (e->tt >= 1) {
            e->tt -= 1; e->i += dxs[e->d]; e->j += dzs[e->d];
            int opts[4], n = 0;
            for (int d = 0; d < 4; d++) {
                if (d == (e->d + 2) % 4) continue;
                int ni = e->i + dxs[d], nj = e->j + dzs[d];
                if (ni < 0 || ni > N || nj < 0 || nj > N) continue;
                opts[n++] = d;
            }
            if (n == 0) e->d = (e->d + 2) % 4;
            else if (e->alert > 0) {   // hunt: take the road that closes on the player
                float best = 1e9f; int bd = opts[0];
                for (int q = 0; q < n; q++) {
                    float dq = hypotf(nodeX(e->i + dxs[opts[q]]) - tank.x, nodeX(e->j + dzs[opts[q]]) - tank.z);
                    if (dq < best) { best = dq; bd = opts[q]; }
                }
                e->d = bd;
            } else e->d = opts[GetRandomValue(0, n - 1)];
        }
        float ax = nodeX(e->i), az = nodeX(e->j), bx = nodeX(e->i + dxs[e->d]), bz = nodeX(e->j + dzs[e->d]);
        float tx = ax + (bx - ax) * e->tt, tz = az + (bz - az) * e->tt;
        float px = t->x, pz = t->z, ease = fminf(1, 5 * dt);
        t->x += (tx - t->x) * ease; t->z += (tz - t->z) * ease;
        float vx = t->x - px, vz = t->z - pz;
        if (vx * vx + vz * vz > 1e-6f) t->h += angDiff(t->h, atan2f(vx, vz)) * fminf(1, 4 * dt);
        t->odo += hypotf(vx, vz);
        // separate from the player's tank
        for (int p = 0; p < MAXP; p++) {
            if (!pactive[p] || players[p].dead) continue;
            float dx = players[p].x - t->x, dz = players[p].z - t->z, dd = hypotf(dx, dz);
            if (dd < 5.5f && dd > 1e-3f) { players[p].x += dx / dd * (5.5f - dd); players[p].z += dz / dd * (5.5f - dd); players[p].spd *= 1 - fminf(1, 4 * dt); }
        }
        // turret: track the player when engaged, otherwise look along the road
        float want = engaged ? atan2f(tank.x - t->x, tank.z - t->z) : t->h;
        float da = angDiff(t->tur, want), mx = 1.1f * dt;
        t->tur += Clamp(da, -mx, mx);
        float wp = 0.5f * asinf(Clamp(GRAV * dist / (SHELL_V * SHELL_V), 0, 1));
        t->pitch += ((engaged ? wp : 0.05f) - t->pitch) * fminf(1, 5 * dt);
        if (engaged && t->reload <= 0 && enemyCool <= 0 && fabsf(da) < 0.04f) fireEnemy(e);
        // squash cars in the way
        for (int c = 0; c < MAXCAR; c++) if (cars[c].alive && hypotf(cars[c].x - t->x, cars[c].z - t->z) < 2.0f + ctypes[cars[c].type].rad) { bool so = scoreOn; scoreOn = false; killCar(&cars[c], true); scoreOn = so; }
        cur = sv;
    }
}

// ---------------------------------------------------------------- trains
static void damageSeg(Train *t, int k, float d) {
    Seg *g = &t->seg[k];
    if (!g->alive) return;
    g->hp -= d;
    if (g->hp > 0) return;
    float x = segX(t, k);
    g->alive = false; g->wx = x; g->wz = t->z; g->moving = false;
    int pts = g->kind == 0 ? 400 : g->kind == 2 ? 300 : 200;
    if (scoreOn && !isClient) { pts *= chainMult(); score += pts; addMsg("+%d %s%s", pts, g->kind == 0 ? "LOCOMOTIVE" : g->kind == 2 ? "TANK CAR" : "TRAIN CAR", chainTag()); }
    debris(x, 2, t->z, 40, g->kind == 1 ? (Color){125, 70, 50, 255} : (Color){70, 70, 78, 255}, 15);
    addEmit((Vector3){x, 1.5f, t->z}, 25, 0);
    if (g->kind == 2) explode(x, 2, t->z, 22, 240, false);
    else explode(x, 2, t->z, g->kind == 0 ? 12 : 8, g->kind == 0 ? 110 : 70, false);
}
static void trainFx(float dt) {   // locomotive smoke and horn (the host and clients both run this)
    for (int ti = 0; ti < 2; ti++) {
        Train *t = &trains[ti];
        t->hornT -= dt;
        float lx = segX(t, 0);
        if (t->seg[0].alive) {
            t->seg[0].bt -= dt;
            if (t->seg[0].bt <= 0) {
                t->seg[0].bt = 0.12f;
                part((Vector3){lx + t->dir * 2.5f, 4.2f, t->z}, (Vector3){rf(-.5f, .5f), rf(3, 5), rf(-.5f, .5f)}, rf(1.5f, 2.8f), rf(.7f, 1.2f), (Color){210, 210, 210, 150}, 0, 1.2f, true);
            }
            if (t->hornT <= 0 && distToTank(lx, t->z) < 170) { snd(sHorn, 0.7f * vol(lx, t->z), t->dir > 0 ? 1.0f : 0.9f); t->hornT = rf(7, 12); }
        }
    }
}
static void updateTrains(float dt) {
    trainHitT -= dt;
    for (int ti = 0; ti < 2; ti++) {
        Train *t = &trains[ti];
        // the engine pulls everything behind it up to the first wreck; once the loco is dead, or a wagon is
        // destroyed, whatever is behind the break is left standing where it is
        bool ok = true;
        for (int k = 0; k < NTSEG; k++) { if (!t->seg[k].alive) ok = false; t->seg[k].moving = ok; }
        if (t->seg[0].alive) {   // blocked by wrecks / stranded wagons on the track ahead?
            float x0 = segX(t, 0);
            for (int j = 1; j < NTSEG; j++) {
                if (t->seg[j].moving) continue;
                float gap = t->dir * (segX(t, j) - x0);
                if (gap > 0 && gap < 11.5f && fabsf(segZ(t, j) - t->z) < 3.8f) { for (int k = 0; k < NTSEG; k++) t->seg[k].moving = false; break; }
            }
        }
        bool run = t->seg[0].moving;
        t->spd += ((run ? (ti == 0 ? 12.0f : 10.0f) : 0) - t->spd) * fminf(1, (run ? 0.8f : 1.6f) * dt);
        for (int k = 0; k < NTSEG; k++) if (t->seg[k].alive && t->seg[k].moving) t->seg[k].u += t->spd * dt;
        if (t->spd < 0.5f) continue;   // standing still: nothing to run over
        for (int k = 0; k < NTSEG; k++) {
            Seg *g = &t->seg[k];
            if (!g->alive || !g->moving) continue;
            float sx = segX(t, k);
            for (int c = 0; c < MAXCAR; c++) {
                Car *cr = &cars[c];
                if (!cr->alive) continue;
                if (fabsf(cr->x - sx) < 4.8f + ctypes[cr->type].rad * 0.6f && fabsf(cr->z - t->z) < 1.8f + ctypes[cr->type].rad * 0.6f) { bool so = scoreOn; scoreOn = false; killCar(cr, true); scoreOn = so; }
            }
            for (int e = 0; e < MAXET; e++) {
                ETank *et = &ets[e];
                if (!et->used || et->t.dead) continue;
                if (fabsf(et->t.x - sx) < 6.8f && fabsf(et->t.z - t->z) < 3.9f) { bool so = scoreOn; scoreOn = false; damageET(et, 90 * dt); scoreOn = so; }
            }
        }
    }
}
// the tank can't pass through a train: it gets shoved aside and takes a hit
static void trainCollide(float dt) {
    float fx = sinf(tank.h), fz = cosf(tank.h);
    for (int ti = 0; ti < 2; ti++) for (int k = 0; k < NTSEG; k++) {
        Train *t = &trains[ti];
        Seg *g = &t->seg[k];
        float sx = segX(t, k), sz = segZ(t, k);
        if (fabsf(sx - tank.x) > 14 || fabsf(sz - tank.z) > 12) continue;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float cx = tank.x + fx * 2.0f * s2, cz = tank.z + fz * 2.0f * s2;
            float nx = Clamp(cx, sx - 4.9f, sx + 4.9f), nz = Clamp(cz, sz - 1.9f, sz + 1.9f);
            float dx = cx - nx, dz = cz - nz, d2 = dx * dx + dz * dz;
            if (d2 >= TANK_R * TANK_R) continue;
            float d = sqrtf(d2);
            if (d < 1e-4f) { dx = 0; dz = tank.z >= sz ? 1 : -1; d = 0; } else { dx /= d; dz /= d; }
            float push = TANK_R - d;
            if (g->alive) {   // an intact wagon is immovable
                tank.x += dx * push; tank.z += dz * push;
                tank.spd *= 1 - fminf(1, 6 * dt);
                if (g->moving && trainHitT <= 0 && !tank.dead) { damageTank(18); trainHitT = 0.7f; }
                if (fabsf(tank.spd) > 6) damageSeg(t, k, fabsf(tank.spd) * 12 * dt);
            } else {          // a wreck: shoulder it out of the way, slowly
                g->wx -= dx * push * 0.45f; g->wz -= dz * push * 0.45f;
                tank.x += dx * push * 0.55f; tank.z += dz * push * 0.55f;
                tank.spd *= 1 - fminf(1, 3.5f * dt);
                if (GetRandomValue(0, 6) == 0)
                    part((Vector3){sx + rf(-3, 3), 0.4f, sz + rf(-1.5f, 1.5f)}, (Vector3){rf(-1, 1), rf(1, 3), rf(-1, 1)}, rf(.4f, .8f), rf(.3f, .5f), (Color){160, 150, 130, 160}, GRAV * .5f, 0, false);
            }
        }
    }
}

// ---------------------------------------------------------------- shoving vehicles with the tank
// Wrecked cars/buses/trucks/tankers and wrecked enemy tanks can be pushed around, with effort.
// Live buses, trucks and tankers are heavy too: they hold their ground and get shoved (small cars are still squashed).
static bool circleVsBox(float cx, float cz, float r, float bx, float bz, float h, float hx, float hz, float *nx, float *nz, float *pen) {
    float c = cosf(h), s = sinf(h);
    float dx = cx - bx, dz = cz - bz;
    float lx = dx * c - dz * s, lz = dx * s + dz * c;
    float qx = Clamp(lx, -hx, hx), qz = Clamp(lz, -hz, hz);
    float ex = lx - qx, ez = lz - qz, d2 = ex * ex + ez * ez;
    if (d2 >= r * r) return false;
    float d = sqrtf(d2);
    if (d < 1e-4f) {   // centre inside the box: leave by the nearest face
        if (hx - fabsf(lx) < hz - fabsf(lz)) { ex = lx >= 0 ? 1 : -1; ez = 0; d = -(hx - fabsf(lx)); }
        else { ex = 0; ez = lz >= 0 ? 1 : -1; d = -(hz - fabsf(lz)); }
    } else { ex /= d; ez /= d; }
    *nx = ex * c + ez * s; *nz = -ex * s + ez * c;
    *pen = r - d;
    return true;
}
static void shoveBody(float *bx, float *bz, float nx, float nz, float pen, float bodyShare, float drag, float dt) {
    *bx -= nx * pen * bodyShare; *bz -= nz * pen * bodyShare;
    tank.x += nx * pen * (1 - bodyShare); tank.z += nz * pen * (1 - bodyShare);
    tank.spd *= 1 - fminf(1, drag * dt);
    if (GetRandomValue(0, 6) == 0)
        part((Vector3){*bx + rf(-2, 2), 0.4f, *bz + rf(-2, 2)}, (Vector3){rf(-1, 1), rf(1, 3), rf(-1, 1)}, rf(.4f, .8f), rf(.3f, .5f), (Color){160, 150, 130, 160}, GRAV * .5f, 0, false);
}
static void vehiclesCollide(float dt) {
    if (tank.dead) return;
    float fx = sinf(tank.h), fz = cosf(tank.h);
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &cars[i];
        if (c->x == 0 && c->z == 0) continue;
        if (c->alive && c->type < CT_BUS) continue;   // squashed elsewhere
        if (fabsf(c->x - tank.x) > 12 || fabsf(c->z - tank.z) > 12) continue;
        const CarType *ct = &ctypes[c->type];
        bool big = c->type >= CT_BUS;
        float hx = (big ? 1.4f : 1.15f) * ct->sx, hz = (big ? (c->type == CT_BUS ? 4.8f : 4.6f) : 2.3f) * ct->sz;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float nx, nz, pen;
            if (!circleVsBox(tank.x + fx * 2.0f * s2, tank.z + fz * 2.0f * s2, TANK_R, c->x, c->z, c->h, hx, hz, &nx, &nz, &pen)) continue;
            shoveBody(&c->x, &c->z, nx, nz, pen, big ? 0.45f : 0.8f, big ? 3.5f : 2.0f, dt);
            if (c->alive) { c->shoved = 1.2f; c->go = 0; }
        }
    }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        if (!e->used || !e->t.dead) continue;
        if (fabsf(e->t.x - tank.x) > 12 || fabsf(e->t.z - tank.z) > 12) continue;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float nx, nz, pen;
            if (!circleVsBox(tank.x + fx * 2.0f * s2, tank.z + fz * 2.0f * s2, TANK_R, e->t.x, e->t.z, e->t.h, 2.2f, 3.7f, &nx, &nz, &pen)) continue;
            shoveBody(&e->t.x, &e->t.z, nx, nz, pen, 0.4f, 4.0f, dt);
        }
    }
}

// ---------------------------------------------------------------- projectiles
static bool projHit(Proj *p) {
    float x = p->p.x, y = p->p.y, z = p->p.z;
    if (p->owner == 1) {
        for (int p = 0; p < MAXP; p++) if (pactive[p] && !players[p].dead && y < 3.2f && hypotf(x - players[p].x, z - players[p].z) < 3.4f) return true;
    } else {
        for (int i = 0; i < MAXET; i++) {
            ETank *e = &ets[i];
            if (!e->used || e->t.dead || y > 3.2f || hypotf(x - e->t.x, z - e->t.z) > 3.4f) continue;
            if (p->type == 0) return true;
            damageET(e, 5);
            for (int q = 0; q < 3; q++) part(p->p, (Vector3){rf(-4, 4), rf(1, 5), rf(-4, 4)}, rf(.2f, .5f), rf(.2f, .4f), (Color){255, 220, 120, 255}, GRAV, 0, false);
            return true;
        }
    }
    if (y < 4.3f) for (int ti = 0; ti < 2; ti++) {
        Train *tr = &trains[ti];
        for (int k = 0; k < NTSEG; k++) {
            if (!tr->seg[k].alive || fabsf(segX(tr, k) - x) > 4.9f || fabsf(tr->z - z) > 2.0f) continue;
            if (p->type == 0) return true;
            damageSeg(tr, k, 4);
            for (int q = 0; q < 3; q++) part(p->p, (Vector3){rf(-4, 4), rf(1, 5), rf(-4, 4)}, rf(.2f, .5f), rf(.2f, .4f), (Color){255, 220, 120, 255}, GRAV, 0, false);
            return true;
        }
    }
    int cnt = query(x, z, 4);
    for (int k = 0; k < cnt; k++) {
        Obj *o = &objs[qbuf[k]];
        if (!o->alive || y > o->h || y < 0) continue;
        float tx = o->ax, tz = o->az;
        if (o->type == O_TREE) { if (y < 2.2f) { tx = tz = 0.5f; } else { tx = tz = 1.7f; } }
        if (o->type == O_LAMP || o->type == O_HYDRANT || o->type == O_MAILBOX) { tx += 0.3f; tz += 0.3f; }
        if (fabsf(x - o->x) > tx || fabsf(z - o->z) > tz) continue;
        if (p->type == 0) return true;
        dmgObj(o, 7, p->v.x, p->v.z);
        for (int i = 0; i < 3; i++) part(p->p, (Vector3){rf(-4, 4), rf(1, 5), rf(-4, 4)}, rf(.2f, .5f), rf(.2f, .4f), (Color){220, 200, 120, 255}, GRAV, 0, false);
        return true;
    }
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &cars[i];
        if (!c->alive || y > ctypes[c->type].hgt) continue;
        if (hypotf(x - c->x, z - c->z) < ctypes[c->type].rad + 0.1f) {
            if (p->type == 0) return true;
            dmgCar(c, 6);
            for (int q = 0; q < 3; q++) part(p->p, (Vector3){rf(-4, 4), rf(1, 5), rf(-4, 4)}, rf(.2f, .5f), rf(.2f, .4f), (Color){255, 220, 120, 255}, GRAV, 0, false);
            return true;
        }
    }
    return false;
}
static void updateProj(float dt) {
    for (int i = 0; i < MAXPROJ; i++) {
        Proj *p = &proj[i];
        if (!p->on) continue;
        scoreOn = p->owner == 0;
        float sp = Vector3Length(p->v);
        int steps = (int)ceilf(sp * dt / 0.6f);
        float sdt = dt / steps;
        for (int s = 0; s < steps && p->on; s++) {
            if (p->type == 0) p->v.y -= GRAV * sdt;
            p->p = Vector3Add(p->p, Vector3Scale(p->v, sdt));
            if (p->p.y <= 0.05f) {
                p->on = false;
                if (p->type == 0) explode(p->p.x, 0.5f, p->p.z, 9, 170, true);
                else for (int q = 0; q < 4; q++) part(p->p, (Vector3){rf(-3, 3), rf(1, 4), rf(-3, 3)}, rf(.3f, .7f), rf(.3f, .6f), (Color){150, 135, 110, 200}, GRAV * .5f, 0, false);
                break;
            }
            if (projHit(p)) {
                p->on = false;
                if (p->type == 0) explode(p->p.x, p->p.y, p->p.z, 9, 170, true);
                break;
            }
        }
        p->life -= dt;
        if (p->life <= 0 && p->on) p->on = false;
        if (p->on && p->type == 0 && GetRandomValue(0, 1) == 0)
            part(p->p, (Vector3){0, 0.5f, 0}, 0.5f, 0.5f, (Color){200, 200, 200, 150}, 0, 0.8f, true);
    }
    scoreOn = true;
}
static void updateFx(float dt) {
    for (int i = 0; i < MAXPART; i++) {
        Part *q = &parts[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        q->p = Vector3Add(q->p, Vector3Scale(q->v, dt));
        q->v.y -= q->g * dt;
        if (q->smoke) q->v = Vector3Scale(q->v, 1 - 1.2f * dt);
        q->size = fmaxf(0.05f, q->size + q->grow * dt);
        if (q->p.y < 0.1f && q->g > 0) { q->p.y = 0.1f; q->v.y *= -0.3f; q->v.x *= 0.6f; q->v.z *= 0.6f; }
    }
    for (int i = 0; i < 24; i++) flashes[i].t -= dt;
    for (int i = 0; i < MAXEMIT; i++) {
        Emit *e = &emits[i];
        if (e->t <= 0) continue;
        e->t -= dt;
        if (e->kind == 1) {
            for (int k = 0; k < 3; k++)
                part(e->p, (Vector3){rf(-1.2f, 1.2f), rf(10, 15), rf(-1.2f, 1.2f)}, rf(.8f, 1.2f), rf(.2f, .35f), (Color){150, 200, 255, 200}, 18, 0, false);
        } else {
            if (GetRandomValue(0, 2) == 0)
                part((Vector3){e->p.x + rf(-2, 2), e->p.y, e->p.z + rf(-2, 2)}, (Vector3){rf(-.5f, .5f), rf(4, 7), rf(-.5f, .5f)}, rf(.5f, 1), rf(1, 2), (Color){255, 140, 30, 210}, 0, -.8f, true);
            if (GetRandomValue(0, 1) == 0)
                part((Vector3){e->p.x + rf(-2, 2), e->p.y + 2, e->p.z + rf(-2, 2)}, (Vector3){rf(-.5f, .5f), rf(3, 5), rf(-.5f, .5f)}, rf(2, 4), rf(1.5f, 2.5f), (Color){45, 45, 45, 180}, 0, 1.5f, true);
        }
    }
    for (int i = 0; i < 6; i++) msgT[i] -= dt;
    for (int i = 0; i < MAXOBJ && i < nobj; i++) { Obj *o = &objs[i]; if (!o->alive && o->fall < 1) o->fall = fminf(1, o->fall + dt * (o->type == O_BLDG ? 0.7f : 1.6f)); }
}

// ---------------------------------------------------------------- drawing
#define V(a, b, c) rlVertex3f(a, b, c)
static void sbox(float x, float y, float z, float w, float h, float d, Color c) {
    float a = w * .5f, b = h * .5f, e = d * .5f;
    rlBegin(RL_QUADS);
    rlColor4ub(c.r, c.g, c.b, c.a);
    V(x - a, y + b, z - e); V(x - a, y + b, z + e); V(x + a, y + b, z + e); V(x + a, y + b, z - e);
#define SC(k) rlColor4ub((unsigned char)(c.r * (k)), (unsigned char)(c.g * (k)), (unsigned char)(c.b * (k)), c.a)
    SC(0.82f); V(x - a, y - b, z + e); V(x + a, y - b, z + e); V(x + a, y + b, z + e); V(x - a, y + b, z + e);
    SC(0.58f); V(x - a, y - b, z - e); V(x - a, y + b, z - e); V(x + a, y + b, z - e); V(x + a, y - b, z - e);
    SC(0.72f); V(x + a, y - b, z - e); V(x + a, y + b, z - e); V(x + a, y + b, z + e); V(x + a, y - b, z + e);
    SC(0.64f); V(x - a, y - b, z - e); V(x - a, y - b, z + e); V(x - a, y + b, z + e); V(x - a, y + b, z - e);
    rlEnd();
}
static void roofTri(Vector3 a, Vector3 b, Vector3 c, Color col) {
    rlBegin(RL_TRIANGLES); rlColor4ub(col.r, col.g, col.b, 255);
    V(a.x, a.y, a.z); V(b.x, b.y, b.z); V(c.x, c.y, c.z); rlEnd();
}
static void drawHouse(Obj *o) {
    float W = o->ax * 2, D = o->az * 2, roofH = 2.6f, wallH = o->h - roofH;
    float dmg = Clamp(o->hp / (90 + o->lw * 4), 0.35f, 1);
    Color wall = shade(o->c, 0.7f + 0.3f * dmg);
    sbox(0, wallH / 2, 0, W, wallH, D, wall);
    float e = 0.5f;
    Vector3 p0 = {-W / 2 - e, wallH, -D / 2 - e}, p1 = {W / 2 + e, wallH, -D / 2 - e}, p2 = {W / 2 + e, wallH, D / 2 + e}, p3 = {-W / 2 - e, wallH, D / 2 + e};
    Vector3 ap = {0, wallH + roofH, 0};
    Color rc = {120, 60, 50, 255};
    roofTri(p3, p2, ap, shade(rc, 0.9f)); roofTri(p0, p1, ap, shade(rc, 0.6f));
    roofTri(p1, p2, ap, shade(rc, 0.75f)); roofTri(p0, p3, ap, shade(rc, 0.68f));
    sbox(W * 0.25f, wallH + roofH * 0.8f, D * 0.1f, 1.1f, roofH, 1.1f, (Color){110, 90, 85, 255});   // chimney
    static const float fdx[4] = {0, 1, 0, -1}, fdz[4] = {1, 0, -1, 0};
    float fx = fdx[o->face], fz = fdz[o->face], off = (fx != 0 ? o->ax : o->az) + 0.06f;
    Color glass = {60, 90, 130, 255};
    for (int s = -1; s <= 1; s++) {
        float u = s * (fx != 0 ? o->az : o->ax) * 0.55f;
        bool door = s == 0;
        float px = fx * off + (fx != 0 ? 0 : u), pz = fz * off + (fx != 0 ? u : 0);
        float w = door ? 1.5f : 1.8f, h = door ? 2.4f : 1.4f, y = door ? 1.2f : 2.5f;
        Color c = door ? (Color){90, 55, 35, 255} : glass;
        if (fx != 0) sbox(px, y, pz, 0.12f, h, w, c); else sbox(px, y, pz, w, h, 0.12f, c);
    }
}
static void drawBuilding(Obj *o) {
    float W = o->ax * 2, D = o->az * 2;
    sbox(0, o->h / 2, 0, W, o->h, D, o->c);
    Color gl = {40, 60, 90, 255};
    for (float y = 3; y < o->h - 2; y += 3.2f) sbox(0, y, 0, W + 0.12f, 1.6f, D + 0.12f, gl);
    sbox(0, o->h + 0.6f, 0, W * 0.4f, 1.2f, D * 0.4f, shade(o->c, 0.7f));
    sbox(0, 1.2f, 0, W + 0.5f, 2.4f, D + 0.5f, shade(o->c, 0.8f));
}
static void drawRubble(Obj *o) {
    Color c = shade(o->c, 0.4f);
    float s = o->type == O_BLDG ? 1.0f : 0.5f;
    for (int i = 0; i < 6; i++) {
        float a = (i * 2.4f + o->x), r = (0.2f + 0.15f * (i % 3));
        sbox(cosf(a) * o->ax * r * 2, 0.5f * s * (1 + i % 3), sinf(a) * o->az * r * 2, o->ax * (0.6f + 0.2f * (i % 3)), s * (1 + i % 3), o->az * (0.6f + 0.15f * (i % 2)), shade(c, 0.8f + 0.1f * (i % 4)));
    }
}
static RenderTexture2D adTex[3];
static void makeAds(void) {
    static const char *l1[3] = {"IRON ROAD", "LANDFALL COLONY", "RED GREEN SOFTWARE"};
    static const char *l2[3] = {"Death Track style combat racing", "Your new home among the stars", "We make games. Seriously."};
    static const Color bg[3] = {{35, 36, 42, 255}, {45, 30, 90, 255}, {20, 20, 20, 255}}, fg[3] = {{255, 140, 30, 255}, {120, 230, 220, 255}, {255, 255, 255, 255}};
    for (int i = 0; i < 3; i++) {
        adTex[i] = LoadRenderTexture(1040, 400);
        BeginTextureMode(adTex[i]);
        ClearBackground(bg[i]);
        if (i == 0) for (int k = 0; k < 14; k++) DrawRectangle(k * 80 - 20, 360, 40, 40, (Color){255, 140, 30, 255});
        if (i == 1) { DrawCircle(985, 60, 36, (Color){255, 200, 120, 255}); DrawCircle(1000, 52, 31, bg[1]); }
        if (i == 2) { DrawRectangle(0, 0, 40, 400, (Color){200, 40, 40, 255}); DrawRectangle(1000, 0, 40, 400, (Color){40, 170, 70, 255}); }
        int sz = i == 2 ? 84 : 110;
        DrawText(l1[i], 520 - MeasureText(l1[i], sz) / 2, 90, sz, fg[i]);
        DrawText(l2[i], 520 - MeasureText(l2[i], 44) / 2, 250, 44, (Color){230, 230, 230, 255});
        EndTextureMode();
        GenTextureMipmaps(&adTex[i].texture); SetTextureFilter(adTex[i].texture, TEXTURE_FILTER_TRILINEAR);
    }
}
static void drawObj(Obj *o) {
    rlPushMatrix();
    rlTranslatef(o->x, 0, o->z);
    float ease = o->alive ? 0 : o->fall * o->fall * (3 - 2 * o->fall);
    switch (o->type) {
    case O_HOUSE: case O_BLDG:
        if (o->alive || o->fall < 1) {
            float sy = 1 - 0.9f * ease;
            rlScalef(1, sy, 1);
            if (!o->alive) rlTranslatef(rf(-.15f, .15f) * (1 - ease), 0, rf(-.15f, .15f) * (1 - ease));
            if (o->type == O_HOUSE) drawHouse(o); else drawBuilding(o);
        }
        if (!o->alive && o->fall > 0.5f) drawRubble(o);
        break;
    case O_TREE: {
        if (!o->alive) rlRotatef(ease * 85, cosf(o->tiltA + PI / 2), 0, sinf(o->tiltA + PI / 2));
        sbox(0, o->h * 0.2f, 0, 0.8f, o->h * 0.4f, 0.8f, (Color){95, 65, 40, 255});
        DrawSphereEx((Vector3){0, o->h * 0.62f, 0}, o->h * 0.33f, 6, 8, o->c);
        DrawSphereEx((Vector3){0.5f, o->h * 0.5f, 0.3f}, o->h * 0.25f, 6, 8, shade(o->c, 0.85f));
        break;
    }
    case O_FENCE:
        if (!o->alive) { rlRotatef(ease * 80, (o->face & 1) ? 0 : 1, 0, (o->face & 1) ? 1 : 0); rlTranslatef(0, 0, 0); }
        sbox(0, 0.65f, 0, o->face & 1 ? o->ld : o->lw, 1.3f, o->face & 1 ? o->lw : o->ld, o->alive ? o->c : shade(o->c, 0.6f));
        break;
    case O_LAMP:
        if (!o->alive) rlRotatef(ease * 88, cosf(o->tiltA + PI / 2), 0, sinf(o->tiltA + PI / 2));
        sbox(0, o->h / 2, 0, 0.3f, o->h, 0.3f, o->c);
        sbox(0.7f, o->h, 0, 1.6f, 0.3f, 0.5f, (Color){70, 70, 75, 255});
        sbox(1.2f, o->h - 0.25f, 0, 0.6f, 0.2f, 0.4f, o->alive ? (Color){255, 245, 190, 255} : (Color){60, 60, 60, 255});
        break;
    case O_FUEL: {
        float r = o->lw / 2;
        if (o->alive) {
            DrawCylinder((Vector3){0, 0, 0}, r, r, o->h, 14, o->c);
            DrawCylinder((Vector3){0, o->h * 0.45f, 0}, r * 1.02f, r * 1.02f, o->h * 0.14f, 14, (Color){210, 50, 40, 255});
            DrawCylinder((Vector3){0, o->h, 0}, r * 0.4f, 0, o->h * 0.12f, 10, shade(o->c, 0.7f));
        } else DrawCylinder((Vector3){0, 0, 0}, r, r, 0.4f, 14, (Color){25, 22, 20, 255});
        break;
    }
    case O_HYDRANT:
        if (o->alive) { DrawCylinder((Vector3){0, 0, 0}, 0.35f, 0.35f, 1.0f, 8, o->c); sbox(0, 0.75f, 0, 1.0f, 0.25f, 0.3f, o->c); }
        else DrawCylinder((Vector3){0, 0, 0}, 0.3f, 0.3f, 0.25f, 8, shade(o->c, 0.5f));
        break;
    case O_WATER: {
        if (!o->alive) rlRotatef(ease * 75, cosf(o->tiltA + PI / 2), 0, sinf(o->tiltA + PI / 2));
        Color steel = {95, 100, 108, 255};
        for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2) sbox(a * 2.2f, 4.5f, b * 2.2f, 0.45f, 9.2f, 0.45f, steel);
        for (float y = 2.5f; y < 9; y += 3) { sbox(0, y, 2.2f, 4.6f, 0.25f, 0.25f, steel); sbox(0, y, -2.2f, 4.6f, 0.25f, 0.25f, steel); sbox(2.2f, y, 0, 0.25f, 0.25f, 4.6f, steel); sbox(-2.2f, y, 0, 0.25f, 0.25f, 4.6f, steel); }
        DrawCylinder((Vector3){0, 9, 0}, 3.3f, 3.3f, 5, 14, (Color){170, 190, 205, 255});
        DrawCylinder((Vector3){0, 10.6f, 0}, 3.36f, 3.36f, 1.0f, 14, (Color){40, 90, 170, 255});
        DrawCylinder((Vector3){0, 14, 0}, 0.1f, 3.5f, 1.9f, 14, (Color){90, 95, 100, 255});
        sbox(0, 16.2f, 0, 0.2f, 1.6f, 0.2f, steel);
        break;
    }
    case O_BILL: {
        bool alongX = !(o->face & 1);
        if (!o->alive) { rlRotatef(ease * 80, alongX ? 1 : 0, 0, alongX ? 0 : 1); }
        float L = o->lw;
        int ad = (int)(fabsf(o->x * 7 + o->z * 3)) % 3;
        for (int s = -1; s <= 1; s += 2) { if (alongX) sbox(s * L * 0.3f, 3.2f, 0, 0.5f, 6.4f, 0.5f, (Color){80, 80, 85, 255}); else sbox(0, 3.2f, s * L * 0.3f, 0.5f, 6.4f, 0.5f, (Color){80, 80, 85, 255}); }
        float bw = L, bh = 4.6f, by = 8.8f;
        Color frame = o->alive ? (Color){230, 230, 230, 255} : (Color){120, 120, 120, 255};
        if (alongX) sbox(0, by, 0, bw, bh, 0.5f, frame); else sbox(0, by, 0, 0.5f, bh, bw, frame);
        for (int f = -1; f <= 1; f += 2) {   // the ad itself, textured, on both faces
            float pw = (bw - 0.6f) / 2, ph = (bh - 0.6f) / 2, off = 0.27f * f;
            float rx = alongX ? f : 0, rz = alongX ? 0 : -f;
            float cx = alongX ? 0 : off, cz = alongX ? off : 0;
            rlSetTexture(adTex[ad].texture.id);
            rlBegin(RL_QUADS);
            Color tc = o->alive ? WHITE : (Color){110, 110, 110, 255};
            rlColor4ub(tc.r, tc.g, tc.b, 255);
            rlTexCoord2f(0, 1); rlVertex3f(cx - rx * pw, by + ph, cz - rz * pw);
            rlTexCoord2f(0, 0); rlVertex3f(cx - rx * pw, by - ph, cz - rz * pw);
            rlTexCoord2f(1, 0); rlVertex3f(cx + rx * pw, by - ph, cz + rz * pw);
            rlTexCoord2f(1, 1); rlVertex3f(cx + rx * pw, by + ph, cz + rz * pw);
            rlEnd();
            rlSetTexture(0);
        }
        break;
    }
    case O_MAILBOX:
        if (!o->alive) rlRotatef(ease * 70, 1, 0, 0);
        sbox(0, 0.5f, 0, 0.15f, 1.0f, 0.15f, (Color){90, 70, 50, 255});
        sbox(0, 1.1f, 0, 0.6f, 0.4f, 0.9f, o->c);
        break;
    }
    rlPopMatrix();
}

static void drawCarModel(Car *c) {
    const CarType *ct = &ctypes[c->type];
    Color body = c->alive ? c->col : shade(c->col, 0.3f);
    Color dark = {25, 25, 28, 255}, glass = c->alive ? (Color){30, 40, 55, 255} : (Color){15, 15, 18, 255};
    rlPushMatrix();
    rlScalef(ct->sx, ct->sy * (c->crushed && !c->alive ? 0.45f : 1), ct->sz);
    if (c->type >= CT_BUS) {
        Color wh = {20, 20, 22, 255};
        for (int ax = -1; ax <= 1; ax += 2) for (float z = -3.4f; z <= 3.5f; z += 3.4f) sbox(ax * 1.25f, 0.55f, z, 0.45f, 1.1f, 1.1f, wh);
        if (c->type == CT_BUS) {
            sbox(0, 1.9f, 0, 2.7f, 2.3f, 9.4f, body);
            sbox(0, 0.7f, 0, 2.5f, 0.5f, 9.2f, shade(body, 0.6f));
            for (int ax = -1; ax <= 1; ax += 2) for (float z = -3.6f; z <= 3.7f; z += 1.8f) sbox(ax * 1.36f, 2.3f, z, 0.08f, 0.9f, 1.4f, glass);
            sbox(0, 2.35f, 4.72f, 2.3f, 1.0f, 0.08f, glass);
            sbox(0, 3.15f, 4.5f, 1.4f, 0.3f, 0.1f, (Color){255, 170, 40, 255});
            sbox(0, 3.1f, 0, 2.5f, 0.15f, 8.8f, shade(body, 1.2f));
            if (c->alive) { sbox(-0.9f, 1.0f, 4.72f, 0.4f, 0.3f, 0.06f, YELLOW); sbox(0.9f, 1.0f, 4.72f, 0.4f, 0.3f, 0.06f, YELLOW); }
        } else {
            Color cab = c->type == CT_TANKER ? (Color){200, 40, 40, 255} : body;
            sbox(0, 1.4f, 3.0f, 2.5f, 2.2f, 2.8f, cab);
            sbox(0, 2.0f, 4.45f, 2.2f, 1.0f, 0.08f, glass);
            sbox(0, 0.6f, 0, 2.2f, 0.5f, 9.0f, dark);
            if (c->type == CT_TRUCK) {
                sbox(0, 2.6f, -1.3f, 2.7f, 3.4f, 6.0f, c->alive ? (Color){225, 225, 220, 255} : shade((Color){225, 225, 220, 255}, 0.3f));
                sbox(0, 2.6f, -1.3f, 2.74f, 0.5f, 6.04f, shade(body, c->alive ? 1 : 0.3f));
            } else {
                DrawCylinderEx((Vector3){0, 2.0f, -4.2f}, (Vector3){0, 2.0f, 1.2f}, 1.4f, 1.4f, 12, c->alive ? (Color){185, 190, 196, 255} : (Color){60, 60, 62, 255});
                DrawCylinderEx((Vector3){0, 2.0f, -1.5f}, (Vector3){0, 2.0f, -0.6f}, 1.45f, 1.45f, 12, c->alive ? (Color){220, 40, 40, 255} : (Color){50, 20, 20, 255});
                sbox(0, 3.5f, -1.5f, 0.6f, 0.3f, 0.6f, dark);
            }
            if (c->alive) { sbox(-0.9f, 0.95f, 4.42f, 0.4f, 0.25f, 0.06f, YELLOW); sbox(0.9f, 0.95f, 4.42f, 0.4f, 0.25f, 0.06f, YELLOW); }
        }
        rlPopMatrix();
        return;
    }
    sbox(0, 0.7f, 0, 2.0f, 0.7f, 4.4f, body);
    sbox(0, 0.36f, 0, 1.9f, 0.1f, 4.2f, dark);
    sbox(0, 1.325f, -0.3f, 1.6f, 0.55f, 2.0f, glass);
    sbox(0, 1.6f, -2.0f, 2.2f, 0.12f, 0.5f, dark);
    sbox(-0.9f, 1.4f, -2.0f, 0.1f, 0.4f, 0.1f, dark); sbox(0.9f, 1.4f, -2.0f, 0.1f, 0.4f, 0.1f, dark);
    for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) sbox(sx * 1.15f, 0.45f, sz * 1.4f, 0.35f, 0.9f, 0.9f, dark);
    if (c->alive) { sbox(-0.6f, 0.75f, 2.22f, 0.4f, 0.15f, 0.05f, YELLOW); sbox(0.6f, 0.75f, 2.22f, 0.4f, 0.15f, 0.05f, YELLOW); }
    if (c->type == 1) { sbox(0, 1.85f, -2.0f, 2.7f, 0.08f, 0.7f, (Color){20, 20, 24, 255}); sbox(0, 0.4f, 2.25f, 1.9f, 0.08f, 0.5f, (Color){20, 20, 24, 255}); }
    if (c->type == 2) {
        sbox(0, 1.95f, -0.3f, 1.7f, 0.1f, 0.1f, (Color){60, 60, 66, 255});
        sbox(-0.8f, 1.65f, -0.3f, 0.1f, 0.6f, 0.1f, (Color){60, 60, 66, 255}); sbox(0.8f, 1.65f, -0.3f, 0.1f, 0.6f, 0.1f, (Color){60, 60, 66, 255});
        for (int l = -1; l <= 1; l += 2) sbox(l * 0.45f, 2.0f, -0.3f, 0.3f, 0.2f, 0.2f, (Color){255, 240, 150, 255});
    }
    rlPopMatrix();
}
static void drawCar(Car *c) {
    rlPushMatrix();
    rlTranslatef(c->x, 0, c->z);
    rlRotatef(c->h * RAD2DEG, 0, 1, 0);
    if (!c->alive) rlRotatef(c->roll * RAD2DEG * 0.5f, 0, 0, 1);
    drawCarModel(c);
    rlPopMatrix();
}

static void drawTank(Tank *t, Color olive) {
    Color dk = shade(olive, 0.45f);
    if (t->dead) { olive = (Color){40, 40, 38, 255}; dk = (Color){20, 20, 20, 255}; }
    rlPushMatrix();
    rlTranslatef(t->x, 0, t->z);
    rlRotatef(t->h * RAD2DEG, 0, 1, 0);
    sbox(0, 1.0f, 0, 3.4f, 1.0f, 6.4f, olive);
    sbox(0, 0.85f, 3.45f, 3.2f, 0.7f, 0.9f, shade(olive, 0.9f));      // sloped glacis stand-in
    sbox(0, 1.75f, -2.2f, 3.0f, 0.5f, 1.8f, shade(olive, 0.8f));
    for (int s = -1; s <= 1; s += 2) {
        sbox(s * 2.15f, 0.8f, 0, 1.0f, 1.5f, 7.4f, dk);
        sbox(s * 2.15f, 1.7f, 0, 1.2f, 0.15f, 7.0f, olive);           // fender
        float off = fmodf(t->odo, 0.9f);
        for (float z = -3.4f; z <= 3.5f; z += 0.9f)
            sbox(s * 2.15f, 0.82f, z + off - (z + off > 3.5f ? 7.2f : 0), 1.05f, 1.56f, 0.18f, (Color){18, 18, 18, 255});
        for (int w = 0; w < 5; w++) { float z = -3 + w * 1.5f; DrawCylinderEx((Vector3){s * 2.7f, 0.8f, z}, (Vector3){s * 2.77f, 0.8f, z}, 0.65f, 0.65f, 8, (Color){55, 58, 50, 255}); }
    }
    rlPushMatrix();
    rlTranslatef(0, 2.1f, 0.2f);
    rlRotatef((t->tur - t->h) * RAD2DEG, 0, 1, 0);
    sbox(0, 0.0f, -0.2f, 2.8f, 1.0f, 3.4f, shade(olive, 1.05f));
    sbox(0, 0.45f, -1.6f, 2.4f, 0.8f, 1.4f, shade(olive, 0.95f));
    sbox(-0.7f, 0.7f, -0.1f, 0.9f, 0.3f, 0.9f, dk);   // hatch
    sbox(0.7f, 0.65f, 0.4f, 0.12f, 0.9f, 0.12f, dk);  // antenna base
    rlPushMatrix();
    rlTranslatef(0, 0.1f, 1.3f - t->recoil * 0.5f);
    rlRotatef(-t->pitch * RAD2DEG, 1, 0, 0);
    sbox(0, 0, 0, 1.0f, 0.9f, 0.9f, dk);
    DrawCylinderEx((Vector3){0, 0, 0.3f}, (Vector3){0, 0, 5.2f}, 0.26f, 0.22f, 8, (Color){45, 48, 42, 255});
    DrawCylinderEx((Vector3){0, 0, 4.2f}, (Vector3){0, 0, 5.0f}, 0.34f, 0.34f, 8, (Color){30, 32, 28, 255});
    rlPopMatrix();
    rlPopMatrix();
    rlPopMatrix();
}

static float segDark = 1;
static void sb(float x, float y, float z, float w, float h, float d, Color c) { sbox(x, y, z, w, h, d, shade(c, segDark)); }
static void drawSeg(Seg *g) {
    Color dark = {28, 28, 32, 255}, steel = {70, 70, 76, 255};
    for (int bz = -1; bz <= 1; bz += 2) for (int sx = -1; sx <= 1; sx += 2) sb(sx * 1.0f, 0.55f, bz * 3.0f, 0.4f, 0.9f, 2.4f, dark);
    sb(0, 0.95f, 0, 2.6f, 0.4f, 9.4f, dark);
    switch (g->kind) {
    case 0: {
        Color red = {190, 45, 40, 255};
        sb(0, 2.2f, 0.8f, 3.0f, 2.0f, 7.2f, red);
        sb(0, 3.6f, -2.2f, 3.1f, 1.5f, 3.2f, (Color){70, 70, 80, 255});
        sb(0, 3.8f, -3.85f, 2.4f, 0.8f, 0.1f, (Color){40, 60, 90, 255}); sb(0, 3.8f, -0.6f, 2.4f, 0.8f, 0.1f, (Color){40, 60, 90, 255});
        sb(1.53f, 3.8f, -2.2f, 0.08f, 0.8f, 1.8f, (Color){40, 60, 90, 255}); sb(-1.53f, 3.8f, -2.2f, 0.08f, 0.8f, 1.8f, (Color){40, 60, 90, 255});
        sb(0, 2.0f, 0.8f, 3.06f, 0.35f, 7.3f, (Color){255, 170, 40, 255});
        DrawCylinder((Vector3){0, 3.2f, 2.5f}, 0.4f, 0.5f, 1.3f, 8, shade((Color){40, 40, 44, 255}, segDark));
        if (g->alive) { sb(-0.9f, 2.0f, 4.45f, 0.5f, 0.4f, 0.08f, (Color){255, 250, 200, 255}); sb(0.9f, 2.0f, 4.45f, 0.5f, 0.4f, 0.08f, (Color){255, 250, 200, 255}); }
        break;
    }
    case 1:
        sb(0, 2.9f, 0, 3.1f, 3.4f, 9.2f, (Color){125, 70, 50, 255});
        sb(1.57f, 2.9f, 0, 0.08f, 2.8f, 3.2f, (Color){80, 45, 35, 255}); sb(-1.57f, 2.9f, 0, 0.08f, 2.8f, 3.2f, (Color){80, 45, 35, 255});
        sb(0, 4.7f, 0, 2.8f, 0.2f, 8.8f, (Color){90, 55, 42, 255});
        break;
    case 2:
        DrawCylinderEx((Vector3){0, 2.9f, -4.4f}, (Vector3){0, 2.9f, 4.4f}, 1.6f, 1.6f, 12, shade((Color){38, 38, 42, 255}, segDark));
        DrawCylinderEx((Vector3){0, 2.9f, -0.5f}, (Vector3){0, 2.9f, 0.5f}, 1.65f, 1.65f, 12, shade((Color){255, 150, 30, 255}, segDark));
        sb(0, 4.6f, 0, 0.5f, 0.3f, 0.5f, steel);
        break;
    default:
        sb(0, 1.35f, 0, 3.1f, 0.3f, 9.4f, steel);
        sb(0, 2.9f, -2.2f, 3.0f, 2.8f, 4.3f, (Color){40, 110, 180, 255});
        sb(0, 2.9f, 2.4f, 3.0f, 2.8f, 4.3f, (Color){205, 120, 40, 255});
        break;
    }
}
static void drawTrains(void) {
    float rh = HALFW + 60;
    for (int ti = 0; ti < 2; ti++) {
        Train *t = &trains[ti];
        if (fabsf(t->z - tank.z) < 220) {
            sbox(0, 0.03f, t->z, 2 * rh, 0.06f, 4.0f, (Color){112, 104, 94, 255});
            for (int r = -1; r <= 1; r += 2) sbox(0, 0.14f, t->z + r * 0.75f, 2 * rh, 0.14f, 0.16f, (Color){150, 150, 158, 255});
            float x0 = floorf((tank.x - 200) / 2.2f) * 2.2f;
            for (float x = x0; x < tank.x + 200; x += 2.2f) if (fabsf(x) < rh) sbox(x, 0.09f, t->z, 0.4f, 0.1f, 3.2f, (Color){72, 52, 38, 255});
        }
        for (int k = 0; k < NTSEG; k++) {
            Seg *g = &t->seg[k];
            float x = segX(t, k);
            float sz = segZ(t, k);
            if (hypotf(x - tank.x, sz - tank.z) > 450) continue;
            rlPushMatrix();
            rlTranslatef(x, 0, sz);
            rlRotatef(t->dir > 0 ? 90 : -90, 0, 1, 0);
            if (!g->alive) { rlRotatef((k % 3 - 1) * 6.0f, 0, 0, 1); rlScalef(1, 0.7f, 1); segDark = 0.35f; } else segDark = 1;
            drawSeg(g);
            segDark = 1;
            rlPopMatrix();
        }
    }
}

static void drawWorld(Camera3D cam) {
    BeginMode3D(cam);
    DrawPlane((Vector3){tank.x, -0.3f, tank.z}, (Vector2){3000, 3000}, (Color){84, 120, 70, 255});
    float W2 = HALFW + ROAD / 2;
    // roads
    for (int i = 0; i <= N; i++) {
        float p = nodeX(i);
        sbox(0, -0.15f, p, 2 * W2, 0.3f, ROAD, (Color){62, 62, 68, 255});
        sbox(p, -0.14f, 0, ROAD, 0.3f, 2 * W2, (Color){62, 62, 68, 255});
        for (int k = 0; k < N; k++) {   // centre lines between intersections
            float a = nodeX(k) + ROAD / 2 + 1, b = nodeX(k + 1) - ROAD / 2 - 1;
            sbox((a + b) / 2, 0.02f, p, b - a, 0.04f, 0.3f, (Color){230, 190, 40, 255});
            sbox(p, 0.02f, (a + b) / 2, 0.3f, 0.04f, b - a, (Color){230, 190, 40, 255});
        }
    }
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        float cx = blockC(i), cz = blockC(j);
        int k = blockKind[i][j];
        sbox(cx, 0, cz, BLK, 0.24f, BLK, (Color){168, 168, 162, 255});
        Color g = k == 1 || k == 5 ? (Color){120, 122, 128, 255} : k == 3 || k == 4 ? (Color){95, 95, 100, 255} : (Color){76, 140, 66, 255};
        sbox(cx, 0, cz, BLK - 4, 0.28f, BLK - 4, g);
    }
    // craters
    for (int i = 0; i < 120; i++) if (craters[i].r > 0)
        DrawCylinder((Vector3){craters[i].x, 0.14f + (i % 5) * 0.004f, craters[i].z}, craters[i].r, craters[i].r, 0.02f, 14, (Color){25, 22, 20, 210});
    // objects
    for (int i = 0; i < nobj; i++) {
        Obj *o = &objs[i];
        float dx = o->x - tank.x, dz = o->z - tank.z, d2 = dx * dx + dz * dz;
        float lim = o->type == O_BLDG ? 420 : (o->type == O_HOUSE ? 320 : 200);
        if (d2 > lim * lim) continue;
        drawObj(o);
    }
    for (int i = 0; i < MAXCAR; i++) { Car *c = &cars[i]; if (c->x != 0 || c->z != 0) { if (hypotf(c->x - tank.x, c->z - tank.z) < 400) drawCar(c); } }
    drawTrains();
    for (int p = 0; p < MAXP; p++) if (pactive[p]) drawTank(&players[p], pcol[p]);
    for (int i = 0; i < MAXET; i++) if (ets[i].used && hypotf(ets[i].t.x - tank.x, ets[i].t.z - tank.z) < 450) drawTank(&ets[i].t, (Color){150, 75, 60, 255});
    // shells
    for (int i = 0; i < MAXPROJ; i++) if (proj[i].on) {
        if (proj[i].type == 0) DrawSphere(proj[i].p, 0.45f, (Color){255, 210, 120, 255});
        else {
            Vector3 d = Vector3Scale(Vector3Normalize(proj[i].v), 2.5f);
            DrawLine3D(proj[i].p, Vector3Subtract(proj[i].p, d), (Color){255, 240, 150, 255});
        }
    }
    // aim marker + shell arc preview
    if (!tank.dead) {
        DrawCylinder((Vector3){aimPt.x, 0.2f, aimPt.z}, 1.5f, 1.5f, 0.03f, 20, (Color){255, 60, 40, 90});
        DrawCylinderWires((Vector3){aimPt.x, 0.2f, aimPt.z}, 3.5f, 3.5f, 0.03f, 20, (Color){255, 70, 50, 255});
    }
    // fireballs & particles
    for (int i = 0; i < 24; i++) if (flashes[i].t > 0) {
        float k = 1 - flashes[i].t / 0.3f;
        DrawSphere(flashes[i].p, flashes[i].r * (0.4f + k), (Color){255, (unsigned char)(200 - 120 * k), 60, (unsigned char)(200 * (1 - k))});
    }
    for (int i = 0; i < MAXPART; i++) {
        Part *q = &parts[i];
        if (q->life <= 0) continue;
        Color c = q->c;
        float f = q->life / q->max;
        if (q->smoke) c.a = (unsigned char)(c.a * f);
        else if (f < 0.25f) c.a = (unsigned char)(255 * f * 4);
        DrawCube(q->p, q->size, q->size, q->size, c);
    }
    EndMode3D();
}

// ---------------------------------------------------------------- HUD
static void drawBar(int x, int y, int w, int h, float f, Color c) {
    DrawRectangle(x - 2, y - 2, w + 4, h + 4, (Color){0, 0, 0, 160});
    DrawRectangle(x, y, (int)(w * Clamp(f, 0, 1)), h, c);
}
static Camera3D gcam;
static void drawHUD(int W, int H) {
    int etDead = 0, etTotal = 0;   // counted from the tanks themselves, so kills by trains, blasts etc. count too
    for (int i = 0; i < MAXET; i++) if (ets[i].used) { etTotal++; if (ets[i].t.dead) etDead++; }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        if (!e->used || e->t.dead || distToTank(e->t.x, e->t.z) > 120) continue;
        Vector3 p = {e->t.x, 5.5f, e->t.z};
        if (Vector3DotProduct(Vector3Subtract(p, gcam.position), Vector3Subtract(gcam.target, gcam.position)) <= 0) continue;
        Vector2 sp = GetWorldToScreen(p, gcam);
        drawBar((int)sp.x - 25, (int)sp.y, 50, 6, e->t.hp / ET_HP, (Color){230, 60, 50, 255});
    }
    DrawText(TextFormat("SCORE %d", score), 16, 12, 32, WHITE);
    int y = 52;
    DrawText(TextFormat("Tanks %d/%d   Cars %d   Houses %d   Buildings %d   Trees %d   Fences %d", etDead, etTotal, carKills, kills[O_HOUSE], kills[O_BLDG], kills[O_TREE], kills[O_FENCE]), 16, y, 18, (Color){230, 230, 230, 255});
    for (int i = 0; i < 6; i++) if (msgT[i] > 0) DrawText(msgs[i], 16, y + 28 + i * 22, 20, (Color){255, 230, 120, (unsigned char)(255 * Clamp(msgT[i], 0, 1))});
    DrawText("ARMOR", 16, H - 62, 16, WHITE); drawBar(16, H - 42, 260, 16, tank.hp / TANK_HP, tank.hp > TANK_HP * 0.3f ? (Color){80, 200, 90, 255} : (Color){230, 60, 50, 255});
    DrawText("CANNON", 300, H - 62, 16, WHITE); drawBar(300, H - 42, 160, 16, 1 - tank.reload / 1.3f, tank.reload <= 0 ? (Color){255, 200, 60, 255} : (Color){150, 120, 50, 255});
    // minimap (north-up, z up, x to the left - same as the view when facing +z)
    int ms = 180, ox = W - ms - 14, oy = 14;
    float sc = ms / (2 * HALFW + ROAD);
    DrawRectangle(ox - 2, oy - 2, ms + 4, ms + 4, (Color){0, 0, 0, 170});
    DrawRectangle(ox, oy, ms, ms, (Color){55, 55, 60, 255});
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        int k = blockKind[i][j];
        Color c = k == 1 || k == 5 ? (Color){120, 120, 130, 255} : k == 3 ? (Color){190, 80, 60, 255} : k == 2 ? (Color){50, 120, 60, 255} : (Color){80, 130, 75, 255};
        float bx = (HALFW + ROAD / 2 - blockC(i) - BLK / 2) * sc, by = (HALFW + ROAD / 2 - blockC(j) - BLK / 2) * sc;
        DrawRectangle(ox + (int)(bx), oy + (int)(by), (int)(BLK * sc) + 1, (int)(BLK * sc) + 1, c);
    }
    for (int ti = 0; ti < 2; ti++) {
        int ry = oy + (int)((HALFW + ROAD / 2 - trains[ti].z) * sc);
        DrawLine(ox, ry, ox + ms, ry, (Color){20, 20, 20, 255});
        for (int k = 0; k < NTSEG; k++) if (trains[ti].seg[k].alive) {
            float tx = segX(&trains[ti], k);
            if (fabsf(tx) < HALFW + ROAD / 2) DrawRectangle(ox + (int)((HALFW + ROAD / 2 - tx) * sc) - 2, oy + (int)((HALFW + ROAD / 2 - segZ(&trains[ti], k)) * sc) - 1, 4, 3, k == 0 ? (Color){255, 140, 40, 255} : (Color){200, 160, 110, 255});
        }
    }
    for (int i = 0; i < MAXCAR; i++) if (cars[i].alive)
        DrawRectangle(ox + (int)((HALFW + ROAD / 2 - cars[i].x) * sc) - 1, oy + (int)((HALFW + ROAD / 2 - cars[i].z) * sc) - 1, 3, 3, YELLOW);
    for (int i = 0; i < MAXET; i++) if (ets[i].used && !ets[i].t.dead)
        DrawRectangle(ox + (int)((HALFW + ROAD / 2 - ets[i].t.x) * sc) - 3, oy + (int)((HALFW + ROAD / 2 - ets[i].t.z) * sc) - 3, 6, 6, (Color){255, 50, 40, 255});
    for (int p = 0; p < MAXP; p++) {
        if (!pactive[p] || players[p].dead) continue;
        Tank *pt = &players[p];
        float px = ox + (HALFW + ROAD / 2 - pt->x) * sc, py = oy + (HALFW + ROAD / 2 - pt->z) * sc;
        float fx = -sinf(pt->h), fy = -cosf(pt->h);
        Vector2 a = {px + fx * 7, py + fy * 7}, b = {px - fx * 4 - fy * 4, py - fy * 4 + fx * 4}, c = {px - fx * 4 + fy * 4, py - fy * 4 - fx * 4};
        DrawTriangle(a, c, b, p == me ? WHITE : pcol[p]);
    }
    // crosshair at the mouse
    Vector2 m = getenv("TT_NOFPS") ? GetWorldToScreen(inp[me].aim, gcam) : GetMousePosition();   // screenshot runs have no mouse
    DrawCircleLines((int)m.x, (int)m.y, 12, (Color){255, 255, 255, 220});
    DrawLine((int)m.x - 18, (int)m.y, (int)m.x - 6, (int)m.y, WHITE); DrawLine((int)m.x + 6, (int)m.y, (int)m.x + 18, (int)m.y, WHITE);
    DrawLine((int)m.x, (int)m.y - 18, (int)m.x, (int)m.y - 6, WHITE); DrawLine((int)m.x, (int)m.y + 6, (int)m.x, (int)m.y + 18, WHITE);
    if (gameT < 12 && !tank.dead && !getenv("TT_NOFPS")) {
        const char *h = netMode == M_SINGLE ? "W/S drive   A/D turn   Mouse aim   LMB cannon   RMB / Ctrl machine gun   Q/E look, C recenter, wheel zoom   R restart   P pause" : "W/S drive   A/D turn   Mouse aim   LMB cannon   RMB / Ctrl machine gun   Q/E look, C recenter, wheel zoom";
        DrawText(h, W / 2 - MeasureText(h, 18) / 2, H - 100, 18, (Color){255, 255, 255, (unsigned char)(255 * Clamp(12 - gameT, 0, 1))});
    }
    if (tank.dead) {
        const char *s = "TANK DESTROYED";
        DrawText(s, W / 2 - MeasureText(s, 60) / 2, H / 2 - 60, 60, (Color){255, 80, 60, 255});
        bool mates = false;
        for (int q = 0; q < MAXP; q++) if (q != me && pactive[q] && !players[q].dead) mates = true;
        if (mates) s = isHost || netMode == M_SINGLE ? TextFormat("Respawning in %d  -  score %d", (int)ceilf(fmaxf(respawnT[me], 0)), score) : TextFormat("Respawning soon  -  score %d", score);
        else s = isClient ? TextFormat("All tanks destroyed  -  score %d  -  waiting for the host to restart (R)", score) : TextFormat("Score %d  -  press R to restart", score);
        DrawText(s, W / 2 - MeasureText(s, 26) / 2, H / 2 + 10, 26, WHITE);
    }
    for (int p = 0; p < MAXP; p++) {   // teammates: name and armor above their tanks
        if (p == me || !pactive[p] || players[p].dead) continue;
        Vector3 wp = {players[p].x, 6.0f, players[p].z};
        if (Vector3DotProduct(Vector3Subtract(wp, gcam.position), Vector3Subtract(gcam.target, gcam.position)) <= 0) continue;
        Vector2 sp = GetWorldToScreen(wp, gcam);
        const char *nm = TextFormat("P%d", p + 1);
        DrawText(nm, (int)sp.x - MeasureText(nm, 18) / 2, (int)sp.y - 22, 18, pcol[p]);
        drawBar((int)sp.x - 25, (int)sp.y, 50, 6, players[p].hp / TANK_HP, (Color){80, 200, 90, 255});
    }
    if (netMode != M_SINGLE) {
        int n = 0; for (int p = 0; p < MAXP; p++) if (pactive[p]) n++;
        const char *s = TextFormat(isHost ? "CO-OP  hosting on UDP port %d  -  %d player%s" : "CO-OP  you are P%d  -  %d players", isHost ? NET_PORT : me + 1, n, n == 1 ? "" : "s");
        DrawText(s, W / 2 - MeasureText(s, 18) / 2, 32, 18, (Color){200, 230, 255, 255});
    }
    if (paused) { const char *s = "PAUSED"; DrawText(s, W / 2 - MeasureText(s, 50) / 2, H / 2 - 25, 50, WHITE); }
    if (!getenv("TT_NOFPS")) DrawFPS(W / 2 - 30, 8);
}

// ---------------------------------------------------------------- networking
static float snapT, inputT;
static bool gotSnap;
static Tank ptgt[MAXP], ettgt[MAXET];                 // client: where the host says things are (we ease toward it)
static struct { float x, z, h; } ctgt[MAXCAR];
static char netStatus[96];
static float mgSndT[MAXP];

static unsigned worldHash(void) {   // both sides build the same city from a fixed seed; this catches version mismatches
    unsigned h = 2166136261u;
    h = (h ^ (unsigned)nobj) * 16777619u;
    for (int i = 0; i < nobj; i++) {
        h = (h ^ objs[i].type) * 16777619u;
        h = (h ^ (unsigned)(int)(objs[i].x * 0.125f)) * 16777619u;
        h = (h ^ (unsigned)(int)(objs[i].z * 0.125f)) * 16777619u;
    }
    return h;
}
static void wTank(Wr *w, Tank *t) {
    wf(w, t->x); wf(w, t->z); wf(w, t->h); wf(w, t->tur); wf(w, t->pitch); wf(w, t->spd); wf(w, t->hp); wf(w, t->recoil); wf(w, t->reload);
}
static void rTank(Rd *r, Tank *t) {
    t->x = rdf(r); t->z = rdf(r); t->h = rdf(r); t->tur = rdf(r); t->pitch = rdf(r); t->spd = rdf(r); t->hp = rdf(r); t->recoil = rdf(r); t->reload = rdf(r);
}

// ---- host side
static void hostSendSnapshots(void) {
    unsigned char buf[1400];
    Wr w = {buf, 0, sizeof buf};
    w8(&w, P_SNAPA);
    w32(&w, (unsigned)score); w16(&w, etKills); w16(&w, carKills);
    for (int i = 0; i < O_NTYPES; i++) w16(&w, kills[i]);
    for (int p = 0; p < MAXP; p++) {
        Tank *t = &players[p];
        w8(&w, (pactive[p] ? 1 : 0) | (t->dead ? 2 : 0) | (t->mgOn ? 4 : 0));
        if (pactive[p]) wTank(&w, t);
    }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        w8(&w, (e->used ? 1 : 0) | (e->t.dead ? 2 : 0));
        if (e->used) wTank(&w, &e->t);
    }
    for (int ti = 0; ti < 2; ti++) {
        Train *t = &trains[ti];
        wf(&w, t->spd);
        for (int k = 0; k < NTSEG; k++) { w8(&w, t->seg[k].alive); wf(&w, segX(t, k)); wf(&w, segZ(t, k)); }
    }
    net_broadcast(0, false, buf, w.n);
    w.n = 0; w8(&w, P_SNAPB);
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &cars[i];
        w8(&w, (c->alive ? 1 : 0) | (c->crushed ? 2 : 0)); wf(&w, c->x); wf(&w, c->z); wf(&w, c->h);
    }
    net_broadcast(0, false, buf, w.n);
    w.n = 0; w8(&w, P_SNAPC);
    int cnt = 0, at = w.n; w8(&w, 0);
    for (int i = 0; i < MAXPROJ && cnt < 48; i++) {
        Proj *p = &proj[i];
        if (!p->on) continue;
        w8(&w, p->type | (p->owner << 1));
        wf(&w, p->p.x); wf(&w, p->p.y); wf(&w, p->p.z); wf(&w, p->v.x); wf(&w, p->v.y); wf(&w, p->v.z);
        cnt++;
    }
    buf[at] = (unsigned char)cnt;
    net_broadcast(0, false, buf, w.n);
}
static void hostOnPacket(int slot, const unsigned char *d, int len) {
    Rd r = {d, 0, len};
    int type = r8(&r), p = slot + 1;
    if (p < 1 || p >= MAXP) return;
    if (type == P_HELLO) {
        int ver = r8(&r); int n = r16(&r); unsigned hash = r32(&r);
        unsigned char buf[2048];
        Wr w = {buf, 0, sizeof buf};
        if (ver != NET_VER || n != nobj || hash != worldHash()) {
            w8(&w, P_REJECT); const char *m = ver != NET_VER ? "different game version" : "different world (version mismatch)";
            for (const char *c = m; *c; c++) w8(&w, *c);
            net_send(slot, 1, true, buf, w.n);
            return;
        }
        pactive[p] = true; spawnPlayer(p); memset(&inp[p], 0, sizeof(inp[p]));
        addMsg("Player %d joined", p + 1);
        w8(&w, P_WELCOME); w8(&w, p);
        for (int i = 0; i < nobj; i += 8) {
            int bits = 0;
            for (int b = 0; b < 8 && i + b < nobj; b++) if (!objs[i + b].alive) bits |= 1 << b;
            w8(&w, bits);
        }
        net_send(slot, 1, true, buf, w.n);
    } else if (type == P_INPUT) {
        if (!pactive[p]) return;
        Input *in = &inp[p];
        in->thr = (int8_t)r8(&r) / 100.0f; in->steer = (int8_t)r8(&r) / 100.0f;
        int b = r8(&r);
        in->cannon = b & 1; in->mg = b & 2; in->brake = b & 4;
        in->aim.x = rdf(&r); in->aim.z = rdf(&r); in->aim.y = 0.3f;
    }
}
static void hostPoll(void) {
    NetEvent ev;
    while (net_poll(&ev)) {
        if (ev.type == NET_RECEIVE) hostOnPacket(ev.peer, ev.data, ev.len);
        else if (ev.type == NET_DISCONNECT) {
            int p = ev.peer + 1;
            if (p >= 1 && p < MAXP && pactive[p]) { pactive[p] = false; addMsg("Player %d left", p + 1); }
        }
    }
}

// ---- client side
static void fireFx(int kind, Vector3 m, float yaw) {   // muzzle flash + bang for a shot somebody else fired
    for (int i = 0; i < 12; i++)
        part(m, (Vector3){sinf(yaw) * rf(4, 14) + rf(-3, 3), rf(-1, 3), cosf(yaw) * rf(4, 14) + rf(-3, 3)}, rf(.3f, .8f), rf(.4f, 1), i % 2 ? ORANGE : (Color){90, 90, 90, 200}, 0, 1, true);
    snd(sCannon, (kind ? 0.8f : 0.9f) * vol(m.x, m.z), kind ? rf(0.85f, 0.95f) : rf(0.95f, 1.05f));
    if (!kind) shake += 0.1f * vol(m.x, m.z);
}
static void clientOnEvent(Rd *r) {
    int ev = r8(r);
    netApplying = true;
    switch (ev) {
    case EV_EXPLODE: { float x = rdf(r), y = rdf(r), z = rdf(r), rad = rdf(r); bool shell = r8(r); explode(x, y, z, rad, 0, shell); break; }
    case EV_OBJKILL: { int i = r16(r); float fx = rdf(r), fz = rdf(r); if (i >= 0 && i < nobj && objs[i].alive) killObj(&objs[i], fx, fz); break; }
    case EV_EMIT: { Vector3 p = {rdf(r), rdf(r), rdf(r)}; float dur = rdf(r); int k = r8(r); addEmit(p, dur, k); break; }
    case EV_MSG: { char m[64]; int n = r8(r); if (n > 60) n = 60; for (int i = 0; i < n; i++) m[i] = (char)r8(r); m[n] = 0; addMsg("%s", m); break; }
    case EV_FIRE: { int k = r8(r); Vector3 m = {rdf(r), rdf(r), rdf(r)}; float yaw = rdf(r); fireFx(k, m, yaw); break; }
    case EV_RESTART: genWorld(); gotSnap = false; break;
    }
    netApplying = false;
}
static void clientOnSnap(int type, Rd *r) {
    if (type == P_SNAPA) {
        score = (int)r32(r); etKills = r16(r); carKills = r16(r);
        for (int i = 0; i < O_NTYPES; i++) kills[i] = r16(r);
        for (int p = 0; p < MAXP; p++) {
            int fl = r8(r);
            bool was = pactive[p];
            pactive[p] = fl & 1;
            if (!pactive[p]) continue;
            Tank n; memset(&n, 0, sizeof n);
            rTank(r, &n);
            Tank *t = &players[p];
            bool snap = !was || !gotSnap;
            ptgt[p] = n;
            t->dead = fl & 2; t->mgOn = fl & 4;
            t->hp = n.hp; t->spd = n.spd; t->recoil = n.recoil; t->reload = n.reload; t->pitch = n.pitch;
            if (snap) { t->x = n.x; t->z = n.z; t->h = n.h; t->tur = n.tur; }
        }
        for (int i = 0; i < MAXET; i++) {
            int fl = r8(r);
            ETank *e = &ets[i];
            e->used = fl & 1;
            if (!e->used) continue;
            Tank n; memset(&n, 0, sizeof n);
            rTank(r, &n);
            bool dead = fl & 2;
            if (dead && !e->t.dead) debris(e->t.x, 2, e->t.z, 30, (Color){70, 80, 60, 255}, 16);
            ettgt[i] = n;
            e->t.dead = dead; e->t.hp = n.hp; e->t.spd = n.spd; e->t.recoil = n.recoil; e->t.pitch = n.pitch;
            if (!gotSnap || dead) { e->t.x = n.x; e->t.z = n.z; e->t.h = n.h; e->t.tur = n.tur; }
        }
        for (int ti = 0; ti < 2; ti++) {
            Train *t = &trains[ti];
            t->spd = rdf(r);
            for (int k = 0; k < NTSEG; k++) {
                Seg *g = &t->seg[k];
                bool al = r8(r); float x = rdf(r), z = rdf(r);
                if (g->alive && !al) { g->hp = 0; damageSeg(t, k, 1); }   // visuals only on a client
                g->alive = al; g->moving = al && t->spd > 0.5f;
                if (al) g->u = t->dir * x + HALFW + 40; else { g->wx = x; g->wz = z; }
            }
        }
        gotSnap = true;
    } else if (type == P_SNAPB) {
        for (int i = 0; i < MAXCAR; i++) {
            int fl = r8(r); float x = rdf(r), z = rdf(r), h = rdf(r);
            Car *c = &cars[i];
            bool al = fl & 1;
            if (c->alive && !al) { c->x = x; c->z = z; killCar(c, fl & 2); }
            if (al) { ctgt[i].x = x; ctgt[i].z = z; ctgt[i].h = h; }
            else { c->x = x; c->z = z; c->h = h; }
            c->alive = al;
        }
    } else if (type == P_SNAPC) {
        int cnt = r8(r);
        memset(proj, 0, sizeof(proj));
        for (int i = 0; i < cnt && i < MAXPROJ; i++) {
            int tb = r8(r);
            Proj *p = &proj[i];
            p->on = true; p->type = tb & 1; p->owner = tb >> 1;
            p->p = (Vector3){rdf(r), rdf(r), rdf(r)}; p->v = (Vector3){rdf(r), rdf(r), rdf(r)}; p->life = 3;
        }
    }
}
// connection state for a client: 0 connecting, 1 playing, 2 failed
static int clientState;
static void clientPoll(void) {
    NetEvent ev;
    while (net_poll(&ev)) {
        if (ev.type == NET_CONNECT) {
            unsigned char buf[16]; Wr w = {buf, 0, sizeof buf};
            w8(&w, P_HELLO); w8(&w, NET_VER); w16(&w, nobj); w32(&w, worldHash());
            net_send(0, 1, true, buf, w.n);
        } else if (ev.type == NET_DISCONNECT) {
            if (clientState == 0) snprintf(netStatus, sizeof netStatus, "Could not connect to the host");
            else snprintf(netStatus, sizeof netStatus, "Lost connection to the host");
            clientState = 2;
        } else if (ev.type == NET_RECEIVE && ev.len > 0) {
            Rd r = {ev.data, 0, ev.len};
            int type = r8(&r);
            if (type == P_WELCOME) {
                me = r8(&r); if (me < 0 || me >= MAXP) me = 1;
                for (int i = 0; i < nobj; i += 8) {
                    int bits = r8(&r);
                    for (int b = 0; b < 8 && i + b < nobj; b++) if (bits & (1 << b)) { objs[i + b].alive = false; objs[i + b].fall = 1; }
                }
                cur = me; pactive[me] = true; clientState = 1; gotSnap = false;
            } else if (type == P_REJECT) {
                snprintf(netStatus, sizeof netStatus, "Host refused: ");
                int n = (int)strlen(netStatus);
                while (r.n < r.len && n < (int)sizeof(netStatus) - 1) netStatus[n++] = (char)r8(&r);
                netStatus[n] = 0; clientState = 2;
            } else if (clientState == 1) {
                if (type == P_EVENT) clientOnEvent(&r);
                else clientOnSnap(type, &r);
            }
        }
    }
}
static void clientSendInput(void) {
    unsigned char buf[24]; Wr w = {buf, 0, sizeof buf};
    Input *in = &inp[me];
    w8(&w, P_INPUT); w8(&w, (int)(in->thr * 100)); w8(&w, (int)(in->steer * 100));
    w8(&w, (in->cannon ? 1 : 0) | (in->mg ? 2 : 0) | (in->brake ? 4 : 0));
    wf(&w, in->aim.x); wf(&w, in->aim.z);
    net_send(0, 0, false, buf, w.n);
}
// client per-frame: ease toward the latest snapshot, advance shells, sounds and fx that the host doesn't send
static void clientUpdate(float dt) {
    float k = fminf(1, 18 * dt);
    for (int p = 0; p < MAXP; p++) {
        if (!pactive[p]) continue;
        Tank *t = &players[p], *g = &ptgt[p];
        t->x += (g->x - t->x) * k; t->z += (g->z - t->z) * k;
        t->h += angDiff(t->h, g->h) * k; t->tur += angDiff(t->tur, g->tur) * k;
        t->odo += t->spd * dt;
        if (t->mgOn && !t->dead) { mgSndT[p] -= dt; if (mgSndT[p] <= 0) { mgSndT[p] = 0.075f; snd(sMG, 0.25f * vol(t->x, t->z), rf(0.9f, 1.3f)); } }
        if (fabsf(t->spd) > 3 && !t->dead && GetRandomValue(0, 3) == 0)
            part((Vector3){t->x - sinf(t->h) * 3.4f * (t->spd > 0 ? 1 : -1) + rf(-1.5f, 1.5f), 0.3f, t->z - cosf(t->h) * 3.4f * (t->spd > 0 ? 1 : -1)}, (Vector3){rf(-.5f, .5f), rf(.5f, 1.5f), rf(-.5f, .5f)}, rf(.6f, 1.2f), rf(.6f, 1.2f), (Color){170, 160, 140, 110}, 0, 1.5f, true);
    }
    for (int i = 0; i < MAXET; i++) {
        ETank *e = &ets[i];
        if (!e->used || e->t.dead) continue;
        e->t.x += (ettgt[i].x - e->t.x) * k; e->t.z += (ettgt[i].z - e->t.z) * k;
        e->t.h += angDiff(e->t.h, ettgt[i].h) * k; e->t.tur += angDiff(e->t.tur, ettgt[i].tur) * k;
        e->t.odo += fabsf(e->t.spd) * dt;
    }
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &cars[i];
        if (!c->alive) { if (c->x != 0 || c->z != 0) deadCarFx(c, dt); continue; }
        c->x += (ctgt[i].x - c->x) * k; c->z += (ctgt[i].z - c->z) * k;
        c->h += angDiff(c->h, ctgt[i].h) * k;
    }
    for (int i = 0; i < MAXPROJ; i++) {
        Proj *p = &proj[i];
        if (!p->on) continue;
        if (p->type == 0) p->v.y -= GRAV * dt;
        p->p = Vector3Add(p->p, Vector3Scale(p->v, dt));
        if (p->type == 0 && GetRandomValue(0, 1) == 0) part(p->p, (Vector3){0, 0.5f, 0}, 0.5f, 0.5f, (Color){200, 200, 200, 150}, 0, 0.8f, true);
    }
}

// ---- menu / startup
static bool startHost(int port) {
    if (!net_host(port, MAXP - 1)) return false;
    net_lan_serve(port, MAXP);
    netMode = M_HOST; pactive[0] = true; me = 0; cur = 0;
    return true;
}
static bool startJoin(const char *addr) {
    char host[96]; int port = NET_PORT;
    snprintf(host, sizeof host, "%s", addr);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); if (port <= 0) port = NET_PORT; }
    if (host[0] == 0) snprintf(host, sizeof host, "127.0.0.1");
    if (!net_connect(host, port)) return false;
    netMode = M_CLIENT; clientState = 0; snprintf(netStatus, sizeof netStatus, "Connecting to %.60s:%d ...", host, port);
    return true;
}

// ---------------------------------------------------------------- main
enum { PH_MENU, PH_IP, PH_CONNECT, PH_PLAY };
static NetLanGame lan[16]; static int nlan, lanSel;
static void drawMenu(int phase, const char *ip, const char *msg) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    ClearBackground((Color){24, 30, 22, 255});
    const char *t = "TANK TRACKS";
    DrawText(t, W / 2 - MeasureText(t, 90) / 2, H / 5, 90, (Color){190, 210, 120, 255});
    int y = H / 2 - 20;
    if (phase == PH_MENU) {
        const char *l[3] = {"1   Single player", "2   Host a co-op game (up to 4 players)", "3   Join a co-op game"};
        for (int i = 0; i < 3; i++) DrawText(l[i], W / 2 - 280, y + i * 44, 30, WHITE);
        DrawText("Esc quits", W / 2 - 280, y + 160, 20, GRAY);
    } else if (phase == PH_IP) {
        DrawText("Host address (IP or name, optional :port - default port 7777)", W / 2 - 330, y - 10, 22, WHITE);
        DrawRectangle(W / 2 - 330, y + 30, 660, 44, (Color){0, 0, 0, 160});
        DrawText(TextFormat("%s%s", ip, ((int)(GetTime() * 2) & 1) ? "_" : ""), W / 2 - 320, y + 38, 30, (Color){255, 230, 120, 255});
        DrawText(nlan ? "Enter to connect, Up/Down picks a LAN game (leave the box empty), Esc goes back" : "Enter to connect, Esc to go back (empty = this computer)", W / 2 - 330, y + 90, 20, GRAY);
        DrawText(nlan ? "Games found on your network:" : "Searching the network for games ...", W / 2 - 330, y + 130, 22, (Color){190, 210, 120, 255});
        for (int i = 0; i < nlan; i++) {
            bool sel = i == lanSel && !ip[0];
            DrawText(TextFormat("%s %s:%d   %d/%d players", sel ? ">" : " ", lan[i].ip, lan[i].port, lan[i].players, lan[i].maxPlayers), W / 2 - 330, y + 165 + i * 30, 26, sel ? (Color){255, 230, 120, 255} : WHITE);
        }
    } else if (phase == PH_CONNECT) {
        DrawText(msg, W / 2 - MeasureText(msg, 28) / 2, y + 10, 28, WHITE);
        DrawText("Esc to cancel", W / 2 - 60, y + 60, 20, GRAY);
    }
    if (msg && phase != PH_CONNECT && msg[0]) DrawText(msg, W / 2 - MeasureText(msg, 24) / 2, H - 80, 24, (Color){255, 120, 100, 255});
}
static bool anyOtherAlive(int p) {
    for (int q = 0; q < MAXP; q++) if (q != p && pactive[q] && !players[q].dead) return true;
    return false;
}

int main(int argc, char **argv) {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "TANK TRACKS");
    SetExitKey(KEY_NULL);
    InitAudioDevice();
    audioOK = IsAudioDeviceReady();
    if (audioOK) {
        sCannon = synth(160, 35, 0.55f, 0.55f, 3.5f, 1);
        sMG = synth(1100, 400, 0.05f, 0.6f, 10, 1);
        sExp = synth(120, 30, 0.9f, 0.85f, 4, 0);
        sCrunch = synth(300, 70, 0.22f, 0.85f, 8, 0);
        sEng = engineLoop();
        sPick = synth(500, 1200, 0.18f, 0, 5, 0);
        sHorn = synth(220, 220, 1.1f, 0.04f, 1.0f, 1);
    }
    rlDisableBackfaceCulling();
    rlSetClipPlanes(1.0, 3500.0);
    makeAds();
    pactive[0] = true;
    genWorld();   // every machine builds the same city from a fixed seed
    const char *shotPath = getenv("TT_SHOT");     // test hooks: TT_SHOT=file.png TT_DEMO=1 ...
    bool demo = getenv("TT_DEMO") != NULL;
    int phase = PH_MENU;
    char ip[96] = "";
    char menuMsg[128] = "";
    // command line: --host [port]   --join host[:port]
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host")) { if (startHost(i + 1 < argc && atoi(argv[i + 1]) > 0 ? atoi(argv[i + 1]) : NET_PORT)) phase = PH_PLAY; else snprintf(menuMsg, sizeof menuMsg, "Could not start the host"); }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) { if (startJoin(argv[i + 1])) phase = PH_CONNECT; else snprintf(menuMsg, sizeof menuMsg, "Could not connect"); }
    }
    if ((shotPath || demo) && netMode == M_SINGLE && !getenv("TT_MENU")) phase = PH_PLAY;
    if (phase == PH_PLAY) HideCursor();
    if (getenv("TT_TYPE")) {   // test hook: park the tank 40 units south of the first object of this type
        int ty = atoi(getenv("TT_TYPE"));
        for (int i = 0; i < nobj; i++) if (objs[i].type == ty) { if (objs[i].face & 1) { tank.x = objs[i].x + 40; tank.z = objs[i].z; tank.h = -PI / 2; } else { tank.x = objs[i].x; tank.z = objs[i].z - 40; tank.h = 0; } camYaw = tank.h; fprintf(stderr, "found at %.0f %.0f\n", objs[i].x, objs[i].z); break; }
    }
    if (getenv("TT_TRAIN")) { for (int k = 0; k < NTSEG; k++) trains[0].seg[k].u = HALFW + 40 - 20 - k * TLEN; tank.x = -35; tank.z = trains[0].z - 40; tank.h = 0; camYaw = 0; }
    if (getenv("TT_BUS")) {
        cars[0].x = tank.x; cars[0].z = tank.z + 9; cars[0].h = PI / 2 + 0.3f; cars[0].d = 1;
        if (atoi(getenv("TT_BUS")) == 1) { bool so = scoreOn; scoreOn = false; killCar(&cars[0], false); scoreOn = so; cars[0].fireT = 0; cars[0].boomT = 0; }
    }
    if (getenv("TT_NEAR")) { ets[0].i = 4; ets[0].j = 2; ets[0].d = 1; ets[0].t.x = nodeX(4); ets[0].t.z = nodeX(2) + 10; ets[0].t.h = ets[0].t.tur = PI; ets[0].d = 3; }
    Camera3D cam = {0};
    cam.up = (Vector3){0, 1, 0}; cam.fovy = 60; cam.projection = CAMERA_PERSPECTIVE;
    int frame = 0;
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        float dt = fminf(GetFrameTime(), 0.05f);
        int W = GetScreenWidth(), H = GetScreenHeight();
        if (phase != PH_PLAY) {   // ---- title / join screens
            if (phase == PH_MENU) {
                if (IsKeyPressed(KEY_ESCAPE)) break;
                if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_KP_1)) { phase = PH_PLAY; HideCursor(); }
                if (IsKeyPressed(KEY_TWO) || IsKeyPressed(KEY_KP_2)) {
                    if (startHost(NET_PORT)) { phase = PH_PLAY; HideCursor(); } else snprintf(menuMsg, sizeof menuMsg, "Could not start the host (is port %d in use?)", NET_PORT);
                }
                if (IsKeyPressed(KEY_THREE) || IsKeyPressed(KEY_KP_3)) { phase = PH_IP; menuMsg[0] = 0; nlan = 0; lanSel = 0; net_lan_scan_start(); }
            } else if (phase == PH_IP) {
                nlan = net_lan_scan_update(lan, 8);
                if (lanSel >= nlan) lanSel = nlan ? nlan - 1 : 0;
                if (IsKeyPressed(KEY_DOWN) && lanSel < nlan - 1) lanSel++;
                if (IsKeyPressed(KEY_UP) && lanSel > 0) lanSel--;
                int ch;
                while ((ch = GetCharPressed()) > 0) { int n = (int)strlen(ip); if (n < 90 && ch > 32 && ch < 127) { ip[n] = (char)ch; ip[n + 1] = 0; } }
                if (IsKeyPressed(KEY_BACKSPACE) && ip[0]) ip[strlen(ip) - 1] = 0;
                if (IsKeyPressed(KEY_ESCAPE)) { net_lan_scan_stop(); phase = PH_MENU; menuMsg[0] = 0; }
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                    char target[96]; snprintf(target, sizeof target, "%s", ip);
                    if (!ip[0] && nlan) snprintf(target, sizeof target, "%s:%d", lan[lanSel].ip, lan[lanSel].port);
                    net_lan_scan_stop();
                    if (startJoin(target)) phase = PH_CONNECT; else snprintf(menuMsg, sizeof menuMsg, "Could not resolve that address");
                }
            } else if (phase == PH_CONNECT) {
                if (IsKeyPressed(KEY_ESCAPE)) { net_stop(); netMode = M_SINGLE; phase = PH_MENU; menuMsg[0] = 0; }
                clientPoll();
                if (clientState == 1) { phase = PH_PLAY; HideCursor(); }
                else if (clientState == 2) { net_stop(); netMode = M_SINGLE; phase = PH_MENU; snprintf(menuMsg, sizeof menuMsg, "%.95s", netStatus); }
            }
            BeginDrawing();
            drawMenu(phase, ip, phase == PH_CONNECT ? netStatus : menuMsg);
            EndDrawing();
            if (getenv("TT_MENU") && shotPath && ++frame == 20) { TakeScreenshot(shotPath); break; }
            continue;
        }
        cur = me;
        bool netGame = netMode != M_SINGLE;
        if (IsKeyPressed(KEY_ESCAPE)) break;
        if (IsKeyPressed(KEY_P) && !netGame) paused = !paused;
        if (IsKeyPressed(KEY_R) && !isClient) { genWorld(); if (isHost) evRestart(); }
        if (isHost) { hostPoll(); int np = 0; for (int p = 0; p < MAXP; p++) if (pactive[p]) np++; net_lan_serve_update(np); }
        if (isClient) { clientPoll(); if (clientState == 2) { net_stop(); netMode = M_SINGLE; phase = PH_MENU; ShowCursor(); snprintf(menuMsg, sizeof menuMsg, "%.95s", netStatus); genWorld(); memset(pactive, 0, sizeof pactive); pactive[0] = true; me = 0; cur = 0; continue; } }
        {   // Q/E look around while held; the camera swings back behind the tank when released (C snaps it back)
            float qe = (IsKeyDown(KEY_E) ? 1 : 0) - (IsKeyDown(KEY_Q) ? 1 : 0);
            if (qe != 0) camOff += qe * 1.8f * dt;
            else camOff -= camOff * fminf(1, 2.5f * dt);
            if (IsKeyPressed(KEY_C)) camOff = 0;
        }
        camDist = Clamp(camDist - GetMouseWheelMove() * 3, 12, 60);
        // camera follows the hull from behind
        camYaw += angDiff(camYaw, tank.h + camOff) * fminf(1, 3 * dt);
        float ch = camDist * 0.55f;
        Vector3 tp = {tank.x, 1.5f, tank.z};
        cam.position = (Vector3){tp.x - sinf(camYaw) * camDist, ch, tp.z - cosf(camYaw) * camDist};
        cam.target = (Vector3){tp.x + sinf(camYaw) * 6, 1.5f, tp.z + cosf(camYaw) * 6};
        if (shake > 0) { cam.position.x += rf(-1, 1) * shake; cam.position.y += rf(-1, 1) * shake; shake = fmaxf(0, shake - 2.5f * dt); }
        // controls -> this player's input
        Input *in = &inp[me];
        in->thr = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1 : 0) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1 : 0);
        in->steer = (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1 : 0) - (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1 : 0);
        in->brake = IsKeyDown(KEY_SPACE);
        in->cannon = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        in->mg = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsKeyDown(KEY_LEFT_CONTROL);
        // mouse -> ground point
        Ray ray = GetScreenToWorldRay(GetMousePosition(), cam);
        if (ray.direction.y < -0.02f) {
            float t = -(ray.position.y - 0.3f) / ray.direction.y;
            aimPt = Vector3Add(ray.position, Vector3Scale(ray.direction, fminf(t, 600)));
        } else aimPt = Vector3Add(ray.position, Vector3Scale(ray.direction, 400));
        aimPt.y = 0.3f;
        float ad = hypotf(aimPt.x - tank.x, aimPt.z - tank.z);
        if (ad > 360) { aimPt.x = tank.x + (aimPt.x - tank.x) * 360 / ad; aimPt.z = tank.z + (aimPt.z - tank.z) * 360 / ad; }
        if (demo) {   // scripted run for screenshots: drive forward and shell whatever is ahead
            aimPt = (Vector3){tank.x + sinf(tank.h + ((getenv("TT_TRAIN") || getenv("TT_TYPE")) ? 0.0f : 0.35f)) * 45, 0.3f, tank.z + cosf(tank.h + ((getenv("TT_TRAIN") || getenv("TT_TYPE")) ? 0.0f : 0.35f)) * 45};
        }
        if (getenv("TT_AUTO")) {   // test hook: drive forward and fire the cannon every other second
            in->thr = 1; in->steer = atoi(getenv("TT_AUTO")) > 1 ? 0.3f : 0; in->cannon = (frame / 60) % 2;
            aimPt = (Vector3){tank.x + sinf(tank.tur) * 45, 0.3f, tank.z + cosf(tank.tur) * 45};
        }
        if (isClient) {
            inputT += dt;
            if (inputT >= 1.0f / 30) { inputT = 0; clientSendInput(); }
            clientUpdate(dt);
            trainFx(dt);
            updateFx(dt);
        } else if (!paused) {
            gameT += dt;
            if (getenv("TT_KILLLOCO") && frame == 30) damageSeg(&trains[0], 0, 1e4f);
            if (demo) { if (frame % 90 == 5 && tank.reload <= 0) fireShell(); tank.spd = 8; }
            for (int p = 0; p < MAXP; p++) if (pactive[p]) { cur = p; updateTank(dt); }
            cur = me;
            updateCars(dt);
            updateETanks(dt);
            updateTrains(dt);
            trainFx(dt);
            updateProj(dt);
            updateFx(dt);
            for (int p = 0; p < MAXP; p++) {   // a dead tank comes back after a few seconds as long as a teammate is still alive
                if (!pactive[p] || !players[p].dead) continue;
                respawnT[p] -= dt;
                if (respawnT[p] <= 0 && anyOtherAlive(p)) { spawnPlayer(p); addMsg("Player %d is back in action", p + 1); }
            }
            if (isHost) {
                snapT += dt;
                if (snapT >= 1.0f / 30) { snapT = 0; hostSendSnapshots(); }
            }
        }
        if (audioOK) {
            if (!IsSoundPlaying(sEng) && !tank.dead) PlaySound(sEng);
            SetSoundVolume(sEng, tank.dead ? 0 : 0.18f + 0.1f * fabsf(tank.spd) / 16);
            SetSoundPitch(sEng, 0.8f + 0.6f * fabsf(tank.spd) / 16);
        }
        BeginDrawing();
        ClearBackground((Color){140, 190, 235, 255});
        gcam = cam;
        drawWorld(cam);
        drawHUD(W, H);
        EndDrawing();
        net_flush();
        frame++;
        if (shotPath && frame == (getenv("TT_FRAMES") ? atoi(getenv("TT_FRAMES")) : 240)) { TakeScreenshot(shotPath); break; }
    }
    net_stop();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
