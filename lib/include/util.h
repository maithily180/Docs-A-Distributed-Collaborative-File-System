#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>

int mkpath(const char *path);
int read_file_all(const char *path, char **out_buf, int *out_len);
int write_file_all(const char *path, const char *buf, int len);

int config_get_string(const char *key, char *out_buf, size_t out_len);
int config_get_uint16(const char *key, uint16_t *out_value);

#endif

