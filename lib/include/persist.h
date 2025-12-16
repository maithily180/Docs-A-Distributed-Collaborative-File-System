#ifndef PERSIST_H
#define PERSIST_H

// Simple JSON-like persistence for file metadata
int persist_save_metadata(const char *path, void *data, int size);
int persist_load_metadata(const char *path, void *data, int max_size);

#endif

