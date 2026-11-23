/* The firmware's libcurl (7.50.1 on the Echos: OpenSSL and the system CA store, /system/etc/security/cacerts),
 * dlopen'ed once for everything in hassmic that speaks HTTPS: online updates and the plugin index (update.c),
 * media over HTTPS (https.c), Amazon's downloads (davs.c).  Each sets its own options and opens its sockets
 * through net_socket; what they share is the loading. */
#ifndef FWCURL_H
#define FWCURL_H

struct curl_slist;
struct fwcurl {
    void *(*init)(void);
    int (*setopt)(void *, int, ...);
    int (*perform)(void *);
    void (*cleanup)(void *);
    int (*getinfo)(void *, int, ...);
    const char *(*strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
};

/* The library's functions, loaded at the first call; NULL where there is none (said once in the log).  Any thread,
 * no lock held: the first caller loads it and the others wait for that */
const struct fwcurl *fwcurl(void);
#endif
