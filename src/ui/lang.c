/* The date on the screen in the language spoken at home (hassmic's setting speech_language, "lg"): bionic has no locale
 * but C's, so strftime's %A %B were English whatever the language.  One table per language, the day first and
 * capitalised (it starts the line), the month as the language writes it after a day's number (Finnish and Polish:
 * their genitive/partitive, "11. marraskuuta", "11 listopada").  A language not here, or none told: English, the
 * drawer's "Wednesday, 1 November" as before (strftime's %e padded it to " 1"), which the clock now writes too (it
 * had "Wednesday 01 November").  Japanese is left out: the firmware's Roboto has no kanji.  tests/unit/ui_test.c checks each one */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

struct lang {
    const char *code, *fmt;             /* %A the day, %e its number, %B the month */
    const char *days[7], *months[12];  /* Sunday first, as struct tm */
};

static const struct lang langs[] = {
    { "en", "%A, %e %B",
      { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
      { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" } },
    { "de", "%A, %e. %B",
      { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
      { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember" } },
    { "fr", "%A %e %B",
      { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" },
      { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre", "décembre" } },
    { "es", "%A, %e de %B",
      { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" },
      { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" } },
    { "it", "%A %e %B",
      { "Domenica", "Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato" },
      { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre" } },
    { "pt", "%A, %e de %B",
      { "Domingo", "Segunda-feira", "Terça-feira", "Quarta-feira", "Quinta-feira", "Sexta-feira", "Sábado" },
      { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro" } },
    { "nl", "%A %e %B",
      { "Zondag", "Maandag", "Dinsdag", "Woensdag", "Donderdag", "Vrijdag", "Zaterdag" },
      { "januari", "februari", "maart", "april", "mei", "juni", "juli", "augustus", "september", "oktober", "november", "december" } },
    { "sv", "%A %e %B",
      { "Söndag", "Måndag", "Tisdag", "Onsdag", "Torsdag", "Fredag", "Lördag" },
      { "januari", "februari", "mars", "april", "maj", "juni", "juli", "augusti", "september", "oktober", "november", "december" } },
    { "da", "%A %e. %B",
      { "Søndag", "Mandag", "Tirsdag", "Onsdag", "Torsdag", "Fredag", "Lørdag" },
      { "januar", "februar", "marts", "april", "maj", "juni", "juli", "august", "september", "oktober", "november", "december" } },
    { "nb", "%A %e. %B",
      { "Søndag", "Mandag", "Tirsdag", "Onsdag", "Torsdag", "Fredag", "Lørdag" },
      { "januar", "februar", "mars", "april", "mai", "juni", "juli", "august", "september", "oktober", "november", "desember" } },
    { "fi", "%A %e. %B",
      { "Sunnuntai", "Maanantai", "Tiistai", "Keskiviikko", "Torstai", "Perjantai", "Lauantai" },
      { "tammikuuta", "helmikuuta", "maaliskuuta", "huhtikuuta", "toukokuuta", "kesäkuuta", "heinäkuuta", "elokuuta",
        "syyskuuta", "lokakuuta", "marraskuuta", "joulukuuta" } },
    { "pl", "%A, %e %B",
      { "Niedziela", "Poniedziałek", "Wtorek", "Środa", "Czwartek", "Piątek", "Sobota" },
      { "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca", "sierpnia", "września", "października",
        "listopada", "grudnia" } },
    { "ro", "%A, %e %B",
      { "Duminică", "Luni", "Marți", "Miercuri", "Joi", "Vineri", "Sâmbătă" },
      { "ianuarie", "februarie", "martie", "aprilie", "mai", "iunie", "iulie", "august", "septembrie", "octombrie",
        "noiembrie", "decembrie" } },
};

void ui_date(char *out, size_t cap, const struct tm *tm, const char *code)
{
    const struct lang *l = &langs[0];
    for (size_t i = 0; code && i < sizeof langs / sizeof *langs; i++) if (!strcmp(code, langs[i].code)) l = &langs[i];
    size_t n = 0;
    if (!cap) return;
    for (const char *f = l->fmt; *f && n < cap - 1; f++) {
        const char *part = NULL; char num[8];
        if (*f == '%' && f[1]) {
            f++;
            if (*f == 'A') part = l->days[(unsigned)tm->tm_wday % 7];
            else if (*f == 'B') part = l->months[(unsigned)tm->tm_mon % 12];
            else if (*f == 'e') { snprintf(num, sizeof num, "%d", tm->tm_mday); part = num; }
        }
        if (!part) { out[n++] = *f; continue; }
        int k = snprintf(out + n, cap - n, "%s", part);
        n = k < 0 ? n : n + (size_t)k >= cap ? cap - 1 : n + (size_t)k;
    }
    out[n] = 0;
}
