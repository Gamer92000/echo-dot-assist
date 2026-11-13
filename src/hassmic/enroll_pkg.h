/* The wake word set of a word trained on the Echo (enroll.c): its name, and the folder made from Amazon's own "Alexa" set
 * with the retrained model in it.  Nothing here touches the daemon, so tests/unit/enroll_pkg_test.c runs it on the PC.
 * The set is Amazon's with two changes: the model's classifier row for the word (pryon_train) and the names: the word
 * takes the place of "ALEXA" in the keyword spotter, the operating rules, the watermark's key and the symbol table, and
 * the "STOP" spotter and its rules go (the model keeps three outputs, so symbol 2 stays in the table, but nothing
 * listens for it).  See docs/GUIDE.md, "A wake word of your own". */
#ifndef ENROLL_PKG_H
#define ENROLL_PKG_H
#include <stddef.h>

#define ENROLL_MODEL "runtime_stateless_standard.streaming.fixed.int16.onnx"       /* the model's file in a set */
#define ENROLL_MARK  "enrolled.txt"                                                /* in a set made here: whose it is */

/* PHRASE as typed -> the keyword Pryon reports ("HEY_COMPUTER") and the set's folder name ("hey_computer-en-US", whose
 * prefix main.c matches against the keyword).  Letters and digits only; 3 to 32 of them; not Amazon's words or STOP.
 * 0, or -1 with the reason in ERR. */
int enroll_name(const char *phrase, const char *lang, char *key, size_t kcap, char *id, size_t icap, char *err, size_t errsz);

/* TEXT (len bytes, cap in all) without every object of any array whose "name" is NAME, the rest as it was (commas and
 * all).  The number removed, or -1 (not JSON, or no room). */
int enroll_strip_named(char *text, size_t *len, size_t cap, const char *name);

/* DEST (not yet there) = the set in BASE with the model replaced by CANDIDATE (a file) and the word KEY in Alexa's
 * place; a checksum.txt that names this exact content; the mark file with NOTE.  0, or -1 with the reason in ERR
 * (DEST is removed again). */
int enroll_make_set(const char *base, const char *candidate, const char *dest, const char *key, const char *note, char *err, size_t errsz);
#endif
