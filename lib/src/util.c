#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(path, mode) mkdir(path, mode)
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include "../include/util.h"

static int mkdir_if_missing(const char *path) {
    int rc = MKDIR(path, 0755);
    if (rc == 0) return 0;
#ifdef _WIN32
    if (errno == EEXIST) return 0;
#else
    if (errno == EEXIST) return 0;
#endif
    return rc;
}

int mkpath(const char *path) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p; *p = '\0';
            if (mkdir_if_missing(tmp) != 0) {}
            *p = save;
        }
    }
    return mkdir_if_missing(tmp);
}

int read_file_all(const char *path, char **out_buf, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    buf[sz] = '\0';
    fclose(f);
    *out_buf = buf; if (out_len) *out_len = (int)sz; return 0;
}

int write_file_all(const char *path, const char *buf, int len) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp)); tmp[sizeof(tmp)-1] = '\0';
    for (int i = (int)strlen(tmp)-1; i >= 0; --i) {
        if (tmp[i] == '/' || tmp[i] == '\\') { tmp[i] = '\0'; break; }
    }
    if (strlen(tmp) > 0) mkpath(tmp);
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    if (fwrite(buf, 1, len, f) != (size_t)len) { fclose(f); return -1; }
    fclose(f); return 0;
}

static int load_config_buffer(char **out_buf) {
    const char *candidates[] = { "config.yaml", "config.json", NULL };
    for (int i = 0; candidates[i]; i++) {
        if (read_file_all(candidates[i], out_buf, NULL) == 0) {
            return 0;
        }
    }
    return -1;
}

static int extract_value_from_buffer(const char *buf, const char *key, char *out, size_t out_len) {
    if (!buf || !key || !out || out_len == 0) return 0;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(buf, pattern);
    if (!pos) {
        pos = strstr(buf, key);
    }
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) {
        colon = strchr(pos, '=');
    }
    if (!colon) return 0;
    colon++;
    while (*colon && isspace((unsigned char)*colon)) colon++;
    size_t idx = 0;
    if (*colon == '"' || *colon == '\'') {
        char quote = *colon++;
        while (*colon && *colon != quote && idx < out_len - 1) {
            out[idx++] = *colon++;
        }
    } else {
        while (*colon && *colon != '\n' && *colon != '\r' && *colon != '#' && *colon != ',' && idx < out_len - 1) {
            if (isspace((unsigned char)*colon)) break;
            out[idx++] = *colon++;
        }
        while (idx > 0 && isspace((unsigned char)out[idx-1])) idx--;
    }
    out[idx] = '\0';
    return idx > 0;
}

int config_get_string(const char *key, char *out_buf, size_t out_len) {
    if (!key || !out_buf || out_len == 0) return 0;
    out_buf[0] = '\0';
    char *config_data = NULL;
    if (load_config_buffer(&config_data) != 0) {
        return 0;
    }
    int found = extract_value_from_buffer(config_data, key, out_buf, out_len);
    free(config_data);
    return found;
}

int config_get_uint16(const char *key, uint16_t *out_value) {
    if (!key || !out_value) return 0;
    char buf[64];
    if (!config_get_string(key, buf, sizeof(buf))) return 0;
    *out_value = (uint16_t)atoi(buf);
    return 1;
}


