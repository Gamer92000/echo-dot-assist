/* A small JSON reader for documents too nested for the string search sendspin.c makes do with (Music Assistant's
 * API, mass.c): the text parsed once into tokens that point into it, values looked up by key and walked in order,
 * strings unescaped only when read.  One allocation per document (the token array); untrusted input: every
 * length checked, nesting at most JSON_DEPTH deep. */
#ifndef JSON_H
#define JSON_H
#include <stddef.h>

#define JSON_DEPTH 64
enum { J_OBJ = 1, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL };

/* a value: its text [start, end) in the document (a string's without the quotes), and the token after it and all it
 * holds (its next sibling's) */
struct jtok { int type, start, end, next; };
struct json { const char *s; struct jtok *t; int n, cap; };

int  json_parse(struct json *j, const char *s, size_t len);    /* 0: parsed (j->s keeps pointing at s) */
void json_free(struct json *j);

int  json_get(const struct json *j, int obj, const char *key); /* the value of key in obj: its token, -1 none */
int  json_first(const struct json *j, int tok);                /* first element of an array, first key of an object; -1 */
int  json_next(const struct json *j, int in, int tok);         /* the next sibling in container in: -1 at its end */
int  json_len(const struct json *j, int tok);                  /* elements of an array, keys of an object */
/* every element of an array (k: its token) */
#define JSON_EACH(j, arr, k) for (int k = json_first(j, arr); k >= 0; k = json_next(j, arr, k))
int  json_type(const struct json *j, int tok);                 /* 0 for -1 */
/* a string unescaped into UTF-8 (cut at a character boundary), a number or true/false as written: 1; null, missing,
 * an object or an array: 0 and "" */
int  json_str(const struct json *j, int tok, char *out, size_t n);
long long json_int(const struct json *j, int tok, long long def);
double json_num(const struct json *j, int tok, double def);
int  json_bool(const struct json *j, int tok, int def);        /* true/false; a number: not 0 */
int  json_streq(const struct json *j, int tok, const char *s); /* a string equal to s */
/* s as a JSON string, quotes included, into out: its length; -1 if it did not fit, and out is "" then ("" where
 * even that does not fit): always terminated */
int  json_esc(char *out, size_t n, const char *s);
#endif
