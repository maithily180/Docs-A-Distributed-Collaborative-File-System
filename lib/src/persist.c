#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "../../lib/include/persist.h"
#include "../../lib/include/util.h"

// Simple JSON-like persistence for file metadata
int persist_save_metadata(const char *path, void *data, int size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int written = fwrite(data, 1, size, f);
    fclose(f);
    return (written == size) ? 0 : -1;
}

int persist_load_metadata(const char *path, void *data, int max_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int read = fread(data, 1, max_size, f);
    fclose(f);
    return read;
}

