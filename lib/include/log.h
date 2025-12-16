#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>

void log_init(const char *log_file);
void log_write(const char *component, const char *op, const char *user, const char *details, int result);
void log_close(void);

#endif

