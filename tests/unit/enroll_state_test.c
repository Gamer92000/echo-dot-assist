/* Synthetic regression checks: no firmware, model, microphone or recordings needed. */
#include "../../src/hassmic/enroll.c"
#include <assert.h>
#include <sys/file.h>
#ifndef TEST_TMP
#define TEST_TMP "/tmp"
#endif

static void put(const char *name, const char *text)
{
    char p[500]; snprintf(p, sizeof p, "%s/%s", state_dir(), name);
    assert(!statefile_write(p, 0600, text, strlen(text)));
}
static void expect(const char *name, const char *text)
{
    char p[500], got[500] = ""; snprintf(p, sizeof p, "%s/%s", state_dir(), name);
    FILE *f = fopen(p, "r"); assert(f); size_t n = fread(got, 1, sizeof got - 1, f); got[n] = 0; assert(!fclose(f));
    assert(!strcmp(got, text));
}
static atomic_int stop_feed;
static void *feed_thread(void *unused)
{
    (void)unused; int16_t pcm[64] = {0};
    while (!atomic_load(&stop_feed)) enroll_feed(pcm, 64);
    return NULL;
}
int main(int argc, char **argv)
{
    /* Also exercise the same startup/recovery functions against an isolated on-device installer workspace. */
    if (argc == 2 && (!strcmp(argv[1], "--reconcile") || !strcmp(argv[1], "--discard"))) {
        char json[4096], error[300] = ""; enroll_init();
        int rc = !strcmp(argv[1], "--discard") ? enroll_dismiss(error, sizeof error) : 0;
        enroll_json(json, sizeof json); puts(json);
        if (rc) fprintf(stderr, "%s\n", error);
        return rc ? 1 : 0;
    }
    char dir[] = TEST_TMP "/enroll-state-XXXXXX", p[500], err[300] = ""; assert(mkdtemp(dir));
    setenv("HASSMIC_STATE", dir, 1); setenv("HASSMIC_MODELS", dir, 1);
    snprintf(p, sizeof p, "%s/installer.lock", dir); setenv("HASSMIC_ARTIFACT_LOCK", p, 1);
    int fence = open(p, O_CREAT | O_RDWR, 0644); assert(fence >= 0);
    snprintf(p, sizeof p, "%s/artifacts", dir); assert(!mkdir(p, 0700));
    snprintf(p, sizeof p, "%s/nova-en-US", dir); assert(!mkdir(p, 0700));
    const char *pending = "nova-en-US\nNova\n1\n0123456789abcdef0123456789abcdef\n";
    put("wake_word", "alexa\n"); put("enroll-pending", pending);
    put("artifacts/result", "OK wake:nova-en-US-other\n");
    assert(!install_result()); expect("wake_word", "alexa\n");
    enroll_init(); assert(S.state == ST_UNRESOLVED && !S.installed); expect("wake_word", "alexa\n");
    put("artifacts/result", "OK wake:nova-en-US\n");
    put("nova-en-US/checksum.txt", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(install_result() && S.state == ST_UNRESOLVED && !S.installed); expect("wake_word", "alexa\n");
    assert(!enroll_dismiss(err, sizeof err) && S.state == ST_IDLE); expect("wake_word", "alexa\n");
    put("enroll-pending", pending);
    put("nova-en-US/checksum.txt", "0123456789abcdef0123456789abcdef");
    enroll_init(); assert(S.state == ST_DONE && S.installed); expect("wake_word", "nova-en-US\n");
    put("wake_word", "alexa\n"); put("enroll-pending", pending);
    put("artifacts/result", "FAILED wake:nova-en-US: simulated refusal\n");
    memset(&S, 0, sizeof S); assert(install_result() && S.state == ST_FAILED); expect("wake_word", "alexa\n");
    /* Permanent silence: a live root lock prevents clearing; once idle, retract only this queued ID. */
    put("enroll-pending", pending); put("artifacts/result", "");
    put("artifacts/request", "wake:nova-en-US\nsound\n");
    put("artifacts/wake.nova-en-US.ready", "ready");
    enroll_init(); assert(S.state == ST_UNRESOLVED);
    assert(!flock(fence, LOCK_EX | LOCK_NB));
    assert(enroll_dismiss(err, sizeof err) == -1 && S.state == ST_UNRESOLVED);
    expect("artifacts/request", "wake:nova-en-US\nsound\n");
    assert(!flock(fence, LOCK_UN));
    assert(!enroll_dismiss(err, sizeof err) && S.state == ST_IDLE);
    expect("artifacts/request", "sound\n");
    snprintf(p, sizeof p, "%s/enroll-pending", dir); assert(access(p, F_OK));
    snprintf(p, sizeof p, "%s/artifacts/wake.nova-en-US.ready", dir); assert(access(p, F_OK));
    /* Confirmed installation with a selection write failure still has an installed model and can recover. */
    snprintf(p, sizeof p, "%s/wake_word", dir); assert(!unlink(p)); assert(!mkdir(p, 0700));
    put("enroll-pending", pending); put("artifacts/result", "OK wake:nova-en-US\n");
    assert(install_result() && S.state == ST_UNRESOLVED && S.installed);
    assert(!enroll_dismiss(err, sizeof err) && S.state == ST_IDLE); assert(!rmdir(p));
    expect("nova-en-US/checksum.txt", "0123456789abcdef0123456789abcdef");
    close(fence);
    for (int state = ST_PACKAGING; state <= ST_DONE; state++) {
        S.state = state; atomic_store(&cancel_flag, 0); enroll_cancel(); assert(!cancelled() && S.state == state);
    }
    S.state = ST_TRAINING; enroll_cancel(); assert(cancelled());
    pthread_t th; atomic_store(&rec_on, 1); assert(!pthread_create(&th, NULL, feed_thread, NULL));
    for (int i = 0; i < 10000; i++) {
        int16_t *b = malloc(128); assert(b);
        pthread_mutex_lock(&mic_lk); rec_buf = b; rec_want = 64; rec_n = 0; pthread_mutex_unlock(&mic_lk);
        rec_release();
    }
    atomic_store(&stop_feed, 1); pthread_join(th, NULL); remove_tree(dir);
    puts("enroll_state: buffer teardown, exact confirmation, refusal, version and restart checks passed");
}
