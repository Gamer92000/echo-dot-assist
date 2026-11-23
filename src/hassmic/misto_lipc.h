#ifndef MISTO_LIPC_H
#define MISTO_LIPC_H
/* Integer LIPC property through EchoRoot's native tools. Returns 0 on success.
 * value == NULL reads; otherwise sets. Output and execution time are bounded. */
int misto_lipc_int(const char *service, const char *property, const int *value, int *out);
int misto_tool_int(const char *tool, const char *service, const char *property, const int *value, int *out);
#endif
