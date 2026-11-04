/* apps.c: which of packages.xml's packages are the owner's apps, and state/apps as root's lockdown.sh reads it */
#include "apps.h"
#include "../third_party/monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *file(const char *p)
{
    static char b[4096]; FILE *f = fopen(p, "r"); size_t n = 0;
    if (f) { n = fread(b, 1, sizeof b - 1, f); fclose(f); }
    b[n] = 0; return b;
}

int main(void)
{
    char dir[] = "/tmp/apps_testXXXXXX", x[300], st[300];
    if (!mkdtemp(dir)) return 1;
    snprintf(x, sizeof x, "%s/packages.xml", dir); snprintf(st, sizeof st, "%s/apps", dir);
    setenv("HASSMIC_PACKAGES_XML", x, 1); setenv("HASSMIC_STATE", dir, 1);

    CHECK(apps_json(NULL) == NULL);                                 /* no Android */
    FILE *f = fopen(x, "w");
    fputs("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<packages>\n"
          /* the owner's */
          "<package name=\"org.schabi.newpipe\" codePath=\"/data/app/org.schabi.newpipe-1\" publicFlags=\"671628868\" privateFlags=\"0\" ut=\"1a\" version=\"1015\" userId=\"10004\">\n"
          "<package name=\"org.hassmic.screen\" codePath=\"/data/app/org.hassmic.screen-1\" publicFlags=\"671628868\" userId=\"10002\">\n"
          /* Amazon's: in /system */
          "<package name=\"com.amazon.webview.chromium\" codePath=\"/system/app/AmazonWebView\" publicFlags=\"-1207386555\" userId=\"10001\">\n"
          /* an update of a system app: in /data/app, FLAG_SYSTEM set */
          "<package name=\"com.amazon.updated\" codePath=\"/data/app/com.amazon.updated-1\" publicFlags=\"-1207386555\" userId=\"10010\">\n"
          /* a shared uid (system's) */
          "<package name=\"com.shared\" codePath=\"/data/app/com.shared-1\" publicFlags=\"0\" sharedUserId=\"1000\">\n"
          "</packages>\n", f);
    fclose(f);

    size_t n; char *j = apps_json(&n);
    CHECK(j && n == strlen(j));
    CHECK(j && !strcmp(j, "{\"apps\":[{\"package\":\"org.schabi.newpipe\",\"version\":1015,\"net\":false,\"lan\":false},{\"package\":\"org.hassmic.screen\",\"version\":0,\"net\":false,\"lan\":false}],"
                          "\"install\":{\"state\":\"idle\",\"package\":\"\",\"error\":\"\",\"have\":0}}"));
    free(j);

    CHECK(apps_set_net("org.schabi.newpipe", 1) == 0);
    CHECK(!strcmp(file(st), "net org.schabi.newpipe\n"));
    CHECK(apps_set_net("org.schabi.newpipe", 1) == 0);              /* once */
    CHECK(!strcmp(file(st), "net org.schabi.newpipe\n"));
    CHECK(apps_set_net("com.amazon.webview.chromium", 1) == -1);    /* never Amazon's */
    CHECK(apps_set_net("com.amazon.updated", 1) == -1);
    CHECK(apps_set_net("com.shared", 1) == -1);
    CHECK(apps_set_net("not.installed", 1) == -1);
    CHECK(apps_set_net("a;rm -rf", 1) == -1);
    CHECK(apps_set_net("a..b", 0) == -1);
    CHECK(apps_set_net("org.hassmic.screen", 1) == 0);
    j = apps_json(NULL);
    CHECK(j && strstr(j, "{\"package\":\"org.schabi.newpipe\",\"version\":1015,\"net\":true,\"lan\":false}") && strstr(j, "{\"package\":\"org.hassmic.screen\",\"version\":0,\"net\":true,\"lan\":false}"));
    free(j);
    CHECK(apps_set_net("org.schabi.newpipe", 0) == 0);
    CHECK(!strcmp(file(st), "net org.hassmic.screen\n"));
    CHECK(apps_set_net("not.installed", 0) == 0);                   /* taking off one gone is fine */
    /* reachable from the home network (issue #109): a list of its own beside the internet's, the same checks */
    CHECK(apps_set_lan("org.schabi.newpipe", 1) == 0);
    CHECK(!strcmp(file(st), "net org.hassmic.screen\nlan org.schabi.newpipe\n"));
    CHECK(apps_set_lan("com.amazon.webview.chromium", 1) == -1);    /* never Amazon's */
    CHECK(apps_set_lan("a;b", 1) == -1);
    j = apps_json(NULL);
    CHECK(j && strstr(j, "{\"package\":\"org.schabi.newpipe\",\"version\":1015,\"net\":false,\"lan\":true}")
            && strstr(j, "{\"package\":\"org.hassmic.screen\",\"version\":0,\"net\":true,\"lan\":false}"));
    free(j);
    CHECK(apps_set_net("org.hassmic.screen", 0) == 0 && apps_set_lan("org.schabi.newpipe", 0) == 0);
    CHECK(!strcmp(file(st), ""));

    /* an APK in pieces, checked whole, handed to root; root's answer read back */
    char res[300], req[300], apk[300];
    snprintf(res, sizeof res, "%s/result", dir); snprintf(req, sizeof req, "%s/apps-request", dir); snprintf(apk, sizeof apk, "%s/apps-staged.apk", dir);
    setenv("HASSMIC_APPS_RESULT", res, 1);
    static unsigned char data[600000]; for (size_t i = 0; i < sizeof data; i++) data[i] = (unsigned char)(i * 7 + 3);
    uint8_t hash[32]; char hex[65];
    crypto_blake2b(hash, 32, data, sizeof data);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    CHECK(apps_chunk(0, data, APPS_CHUNK) == 0);
    CHECK(apps_chunk(5, data, 10) == -1);                           /* not at the end */
    CHECK(apps_chunk(APPS_CHUNK, data + APPS_CHUNK, APPS_CHUNK) == 0);
    j = apps_json(NULL); CHECK(j && strstr(j, "\"state\":\"receiving\"") && strstr(j, "\"have\":524288")); free(j);
    CHECK(apps_install(sizeof data, hex) == -1);                    /* not all there */
    CHECK(apps_chunk(2 * APPS_CHUNK, data + 2 * APPS_CHUNK, sizeof data - 2 * APPS_CHUNK) == 0);
    char h0 = hex[0]; hex[0] = h0 == '0' ? '1' : '0';
    CHECK(apps_install(sizeof data, hex) == -1);                    /* another file */
    hex[0] = h0;
    CHECK(apps_install(sizeof data, hex) == 0);
    unsigned long id = strtoul(file(req), NULL, 10);
    CHECK(id && strstr(file(req), " install 600000\n"));
    CHECK(apps_install(sizeof data, hex) == -2);                    /* one at a time */
    CHECK(apps_uninstall("org.schabi.newpipe") == -2);
    j = apps_json(NULL); CHECK(j && strstr(j, "\"state\":\"installing\"")); free(j);
    unlink(req);                                                   /* root took it ... */
    j = apps_json(NULL); CHECK(j && strstr(j, "\"state\":\"installing\"")); free(j);   /* ... no answer yet */
    f = fopen(res, "w"); fprintf(f, "%lu ok org.schabi.newpipe\n", id); fclose(f);
    j = apps_json(NULL); CHECK(j && strstr(j, "\"install\":{\"state\":\"ok\",\"package\":\"org.schabi.newpipe\",\"error\":\"\"")); free(j);
    CHECK(apps_uninstall("com.amazon.webview.chromium") == -1);
    CHECK(apps_uninstall("org.schabi.newpipe") == 0);
    id = strtoul(file(req), NULL, 10);
    CHECK(strstr(file(req), " uninstall org.schabi.newpipe\n") != NULL);
    unlink(req);
    f = fopen(res, "w"); fprintf(f, "%lu failed INSTALL_\"bad\n", id); fclose(f);
    j = apps_json(NULL); CHECK(j && strstr(j, "\"state\":\"failed\"") && strstr(j, "\"error\":\"INSTALL_ bad\"")); free(j);

    unlink(x); unlink(st); unlink(res); unlink(apk); rmdir(dir);
    if (fails) return 1;
    printf("apps: ok\n");
    return 0;
}
