/* What zigbeed keeps over restarts, in its folder's zb.state: the network (channel, PAN IDs, keys, our addresses),
 * the frame counters (the NWK one saved 4096 ahead of what was sent, so a restart never sends a counter twice even
 * after a power cut: zb_save at every 4096), the host's endpoints, and every node with its address, capability, link
 * key and whether it is our child.  Text, "key value" lines; written to a temporary file, synced, renamed, the
 * folder synced. */
#include "zb.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int dz_want_connected;

static void hexout(FILE *f, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]); }
static int hexin(const char *s, uint8_t *b, size_t n)
{
    if (strlen(s) < 2 * n) return -1;
    for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; b[i] = (uint8_t)v; }
    return 0;
}

void store_save(void)
{
    char path[512], tmp[520];
    snprintf(path, sizeof path, "%s/zb.state", zb.dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG("store: %s: cannot write", tmp); return; }
    zb.nwk_fc_saved = zb.nwk_fc + 4096;
    fprintf(f, "zigbeed 1\nrole %d\nieee %016llx\nnwk %04x\npan %04x\nepan %016llx\nchannel %d\nmask %08x\n",
            zb.role, (unsigned long long)zb.ieee, zb.nwk, zb.pan, (unsigned long long)zb.epan, zb.channel, zb.channel_mask);
    fprintf(f, "nwkkey "); hexout(f, zb.nwk_key, 16); fprintf(f, "\nkeyseq %u\ntclk ", zb.key_seq); hexout(f, zb.tclk, 16);
    fprintf(f, "\ntc %016llx\nnwkfc %u\napsfc %u\nupdate %u\npredefined %d\nconnected %d\nparent %04x\nparentieee %016llx\ndepth %u\njoined %d\nprefer %016llx\n",
            (unsigned long long)zb.tc_ieee, zb.nwk_fc_saved, zb.aps_fc + 1024, zb.update_id, zb.predefined_pan,
            zb.role == R_COORD ? dz_want_connected : zb.joined, zb.parent, (unsigned long long)zb.parent_ieee, zb.depth, zb.joined, (unsigned long long)zb.prefer_epan);
    for (int i = 0; i < ZB_EP_SLOTS; i++) if (zb.ep_len[i]) { fprintf(f, "ep %d ", i); hexout(f, zb.ep[i], zb.ep_len[i]); fputc('\n', f); }
    for (int i = 0; i < zb.nnodes; i++) {
        struct node *n = &zb.nodes[i];
        if (!n->used || !n->ieee || (n->join_at && !n->authed)) continue;     /* a joiner that never proved itself: not kept */
        fprintf(f, "node %016llx %04x %02x %02x ", (unsigned long long)n->ieee, n->nwk, n->cap,
                n->child | n->neighbor << 1 | n->has_key << 2 | n->key_preconf << 3 | n->key_verified << 4);
        if (n->has_key) hexout(f, n->key, 16); else fputc('-', f);
        /* its frame counter as last heard: frames under it stay replays over a restart */
        fprintf(f, " %u %04x %u %u\n", n->timeout_s, n->parent, n->nwk_fc, n->fc_known);
    }
    gp_save(f);
    if (zb.have_prev_key && zb.prev_key_until > now_ms()) {
        fprintf(f, "prevkey "); hexout(f, zb.prev_key, 16); fprintf(f, " %u %lld\n", zb.prev_key_seq, (long long)(zb.prev_key_until - now_ms()));
    }
    if (fflush(f) || fsync(fileno(f))) { fclose(f); unlink(tmp); return; }
    fclose(f);
    if (rename(tmp, path)) { unlink(tmp); return; }
    int d = open(zb.dir, O_RDONLY | O_DIRECTORY);
    if (d >= 0) { fsync(d); close(d); }
}

int store_load(void)
{
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/zb.state", zb.dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int role = -1, conn = 0;
    while (fgets(line, sizeof line, f)) {
        char k[16], v[200]; unsigned long long x; unsigned u;
        if (sscanf(line, "%15s %199s", k, v) != 2) continue;
        if (!strcmp(k, "role")) role = atoi(v);
        else if (!strcmp(k, "ieee") && sscanf(v, "%llx", &x) == 1) zb.ieee = x;
        else if (!strcmp(k, "nwk") && sscanf(v, "%x", &u) == 1) zb.nwk = (uint16_t)u;
        else if (!strcmp(k, "pan") && sscanf(v, "%x", &u) == 1) zb.pan = (uint16_t)u;
        else if (!strcmp(k, "epan") && sscanf(v, "%llx", &x) == 1) zb.epan = x;
        else if (!strcmp(k, "channel")) zb.channel = atoi(v);
        else if (!strcmp(k, "mask") && sscanf(v, "%x", &u) == 1) zb.channel_mask = u;
        else if (!strcmp(k, "nwkkey")) hexin(v, zb.nwk_key, 16);
        else if (!strcmp(k, "keyseq")) zb.key_seq = (uint8_t)atoi(v);
        else if (!strcmp(k, "tclk")) hexin(v, zb.tclk, 16);
        else if (!strcmp(k, "tc") && sscanf(v, "%llx", &x) == 1) zb.tc_ieee = x;
        else if (!strcmp(k, "nwkfc")) zb.nwk_fc = zb.nwk_fc_saved = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "apsfc")) zb.aps_fc = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "update")) zb.update_id = (uint8_t)atoi(v);
        else if (!strcmp(k, "predefined")) zb.predefined_pan = atoi(v);
        else if (!strcmp(k, "connected")) conn = atoi(v);
        else if (!strcmp(k, "parent") && sscanf(v, "%x", &u) == 1) zb.parent = (uint16_t)u;
        else if (!strcmp(k, "parentieee") && sscanf(v, "%llx", &x) == 1) zb.parent_ieee = x;
        else if (!strcmp(k, "depth")) zb.depth = (uint8_t)atoi(v);
        else if (!strcmp(k, "joined")) zb.joined = atoi(v);
        else if (!strcmp(k, "prefer") && sscanf(v, "%llx", &x) == 1) zb.prefer_epan = x;
        else if (!strcmp(k, "ep")) {
            int i; char hx[200];
            if (sscanf(line, "ep %d %199s", &i, hx) == 2 && i >= 0 && i < ZB_EP_SLOTS) {
                size_t l = strlen(hx) / 2;
                if (l && l <= sizeof zb.ep[0] && !hexin(hx, zb.ep[i], l)) zb.ep_len[i] = (uint8_t)l;
            }
        } else if (!strcmp(k, "prevkey")) {
            char hx[40]; unsigned sq; long long left;
            if (sscanf(line, "prevkey %39s %u %lld", hx, &sq, &left) == 3 && !hexin(hx, zb.prev_key, 16) && left > 0) {
                zb.prev_key_seq = (uint8_t)sq; zb.have_prev_key = 1; zb.prev_key_until = now_ms() + left;
            }
        } else if (!strcmp(k, "gp")) {
            unsigned id, sec, kt, hk, mode, ms, fc, grp, fck = 0; char key[40]; uint8_t kb[16];
            if (sscanf(line, "gp %x %u %u %u %u %u %u %x %39s %u", &id, &sec, &kt, &hk, &mode, &ms, &fc, &grp, key, &fck) >= 9 && !hexin(key, kb, 16))
                gp_load(id, sec, kt, hk, mode, ms, fc, grp, kb, fck);
        } else if (!strcmp(k, "node")) {
            unsigned long long ie; unsigned nwk, cap, fl, to = 0, par = 0xFFFF, fc = 0, fck = 0; char key[40];
            if (sscanf(line, "node %llx %x %x %x %39s %u %x %u %u", &ie, &nwk, &cap, &fl, key, &to, &par, &fc, &fck) < 5) continue;
            struct node *n = node_add(ie, (uint16_t)nwk);
            if (!n) continue;
            n->cap = (uint8_t)cap; n->child = fl & 1; n->neighbor = 0; n->has_key = fl >> 2 & 1; n->key_preconf = fl >> 3 & 1;
            n->key_verified = fl >> 4 & 1; n->timeout_s = to; n->parent = (uint16_t)par; n->seen = now_ms();
            n->nwk_fc = fc; n->fc_known = fck ? 1 : 0; n->authed = 1;
            if (n->has_key && hexin(key, n->key, 16)) n->has_key = 0;
        }
    }
    fclose(f);
    if (role >= 0 && role != (int)zb.role) {
        /* another role than this start's: the network was the other role's; only the node's own EUI-64 stays */
        LOG("store: the state is a %s's, not this role's: starting anew", role == R_COORD ? "coordinator" : "router");
        uint64_t ie = zb.ieee;
        memset(zb.nodes, 0, sizeof zb.nodes); zb.nnodes = 0;
        zb.pan = 0; zb.epan = 0; zb.nwk = 0xFFFF; zb.joined = 0; memset(zb.nwk_key, 0, 16);
        zb.ieee = ie;
        return 0;
    }
    if (zb.role == R_COORD) dz_want_connected = conn;
    return conn;
}
