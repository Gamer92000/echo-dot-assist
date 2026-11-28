/* hassmic (gprcp): the glibc names Qorvo's library (built for a Raspberry Pi's glibc) uses that musl lacks: its assert
 * handler prints a backtrace with them.  None here: the assert's own line says where */
#include <stddef.h>

int backtrace(void **buffer, int size)
{
    (void)buffer;
    (void)size;
    return 0;
}

char **backtrace_symbols(void *const *buffer, int size)
{
    (void)buffer;
    (void)size;
    return NULL;
}
