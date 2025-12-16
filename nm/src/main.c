#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "../../lib/include/net.h"
#include "../../lib/include/util.h"
#include "../../lib/include/hashmap.h"
#include "../../lib/include/lru_cache.h"
#include "../../lib/include/persist.h"
#include "../../lib/include/log.h"
#include "../../lib/include/error_codes.h"

static char nm_bind_host[64] = "0.0.0.0";
static uint16_t nm_client_port = 8000;
static uint16_t nm_ss_port = 8001;
static int nm_verbose = 0;
static int nm_exec_allow_all = 0;

static void print_nm_usage(const char *prog) {
    printf("Usage: %s [--host IP] [--port CLIENT_PORT] [--ss-port SS_REG_PORT] [--verbose] [--exec-allow]\n", prog);
    printf("Defaults: host=0.0.0.0, port=8000, ss-port=8001\n");
}

static void load_nm_config_defaults(void) {
    char buf[64];
    if (config_get_string("nm.host", buf, sizeof(buf))) {
        strncpy(nm_bind_host, buf, sizeof(nm_bind_host)-1);
        nm_bind_host[sizeof(nm_bind_host)-1] = '\0';
    }
    uint16_t tmp;
    if (config_get_uint16("nm.port", &tmp) && tmp != 0) {
        nm_client_port = tmp;
    }
    if (config_get_uint16("nm.ss_port", &tmp) && tmp != 0) {
        nm_ss_port = tmp;
    }
}

// Minimal NM: accepts client commands and SS registrations.

typedef struct {
    char filename[256];  // can include path like "folder/file.txt"
    char owner[64];
    char ss_ip[64];
    uint16_t ss_client_port;
    // simple ACLs: arrays of usernames for R and RW
    char readers[64][64]; int readers_count;
    char writers[64][64]; int writers_count;
    int is_folder;  // 1 if this is a folder

    // ADD THESE NEW FIELDS:
    int word_count;           // Number of words in file
    int char_count;           // Number of characters in file
    time_t last_access_time;  // Last time file was accessed
    time_t created_time;      // When file was created
    time_t modified_time;     // Last modification time
} FileEntry;

#define MAX_FILES 1024
static FileEntry files[MAX_FILES];
static int files_count = 0;
static HashMap *file_map = NULL;  // O(1) file lookup
static LRUCache *file_cache = NULL;  // LRU cache for recent searches

typedef struct {
    char ss_id[64];
    char ip[64];
    uint16_t admin_port;
    uint16_t client_port;
    int is_primary;  // 1 if primary, 0 if replica
    char replica_of[64];  // SS ID this is a replica of (empty if primary)
    time_t last_heartbeat;  // For failure detection
    int is_active;  // 1 if active, 0 if failed
} SSInfo;

#define MAX_SS 32
static SSInfo sss[MAX_SS];
static int ss_count = 0;

// track logged-in users (simple set)
static char users[256][64];
static int users_count = 0;

// Access request structure
typedef struct {
    char filename[256];
    char requesting_user[64];
    char access_type[8];  // "-R" or "-W"
    time_t request_time;
} AccessRequest;

#define MAX_ACCESS_REQUESTS 1024
static AccessRequest access_requests[MAX_ACCESS_REQUESTS];
static int access_requests_count = 0;

// Mutex to protect shared data structures
static pthread_mutex_t nm_mutex = PTHREAD_MUTEX_INITIALIZER;

static void trim(char *s){int n=(int)strlen(s);while(n>0 && (s[n-1]=='\r'||s[n-1]=='\n'||isspace((unsigned char)s[n-1]))) s[--n]='\0';}

// Case-insensitive string comparison
static int strcasecmp_safe(const char *a, const char *b) {
    if (!a || !b) return (a != b);
    while (*a && *b) {
        int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (diff != 0) return diff;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static const char *exec_safe_cmds[] = { "echo", "ls", "pwd", "dir", "type", NULL };

static int exec_command_allowed(const char *script) {
    if (nm_exec_allow_all) return 1;
    if (!script) return 0;
    const char *p = script;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (*p == '\n') { p++; continue; }
        if (*p == '\0') break;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        char cmd[64];
        int idx = 0;
        while (*p && !isspace((unsigned char)*p) && idx < (int)sizeof(cmd) - 1) {
            cmd[idx++] = *p++;
        }
        cmd[idx] = '\0';
        if (cmd[0] == '\0') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        int allowed = 0;
        for (int i = 0; exec_safe_cmds[i]; i++) {
            if (strcasecmp_safe(exec_safe_cmds[i], cmd) == 0) {
                allowed = 1;
                break;
            }
        }
        if (!allowed) return 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 1;
}

// Validate filename: alphanumeric, dots, dashes, underscores, slashes only
// Must have extension (.txt, .md, etc.)
static int is_valid_filename(const char *fname) {
    if (!fname || fname[0] == '\0') return 0;
    
    // Check for spaces
    if (strchr(fname, ' ') != NULL) return 0;
    
    // Check for invalid characters
    for (const char *p = fname; *p; p++) {
        char ch = *p;
        if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != '_' && ch != '/') {
            return 0;  // Invalid character
        }
    }
    
    // Must have extension (contains a dot before the end)
    const char *dot = strrchr(fname, '.');
    if (!dot || dot == fname || *(dot + 1) == '\0') {
        return 0;  // No extension or dot at start/end
    }
    
    return 1;  // Valid filename
}

// Convert time_t to IST string (UTC+5:30)
static void time_t_to_ist_string(time_t t, char *buf, int buflen) {
    // Convert to IST: UTC + 5:30 = UTC + 19800 seconds
    time_t ist_time = t + (5 * 3600 + 30 * 60);
    struct tm *tm_info = gmtime(&ist_time);
    strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", tm_info);
}

// O(1) file lookup using hashmap with LRU cache
static int find_file_index(const char *filename) {
    if (!file_map || !filename) return -1;
    
    // Check LRU cache first
    int cached_idx = lru_cache_get(file_cache, filename);
    if (cached_idx >= 0) {
        // Verify it's still valid
        if (cached_idx < files_count && strcmp(files[cached_idx].filename, filename) == 0) {
            return cached_idx;
        }
    }
    
    // Lookup in hashmap
    int idx = hashmap_get(file_map, filename);
    if (idx >= 0 && idx < files_count && strcmp(files[idx].filename, filename) == 0) {
        // Update cache
        lru_cache_put(file_cache, filename, idx);
        return idx;
    }
    
    return -1;
}

// Add file to hashmap
static void add_file_to_map(const char *filename, int idx) {
    if (file_map && filename && idx >= 0) {
        hashmap_put(file_map, filename, idx);
        lru_cache_put(file_cache, filename, idx);
    }
}

// Remove file from hashmap
static void remove_file_from_map(const char *filename) {
    if (file_map && filename) {
        hashmap_remove(file_map, filename);
    }
}

// Save metadata to disk
static void save_metadata(void) {
    char path[512] = "nm/metadata.dat";
    mkpath("nm");
    FILE *f = fopen(path, "wb");
    if (f) {
        // Write files_count
        fwrite(&files_count, sizeof(int), 1, f);
        // Write all file entries
        for (int i = 0; i < files_count; i++) {
            fwrite(&files[i], sizeof(FileEntry), 1, f);
        }
        // Write users
        fwrite(&users_count, sizeof(int), 1, f);
        for (int i = 0; i < users_count; i++) {
            fwrite(users[i], 64, 1, f);
        }
        // Write access requests
        fwrite(&access_requests_count, sizeof(int), 1, f);
        for (int i = 0; i < access_requests_count; i++) {
            fwrite(&access_requests[i], sizeof(AccessRequest), 1, f);
        }
        fclose(f);
    }
}

// Load metadata from disk
static void load_metadata(void) {
    char path[512] = "nm/metadata.dat";
    FILE *f = fopen(path, "rb");
    if (f) {
        // Read files_count
        fread(&files_count, sizeof(int), 1, f);
        if (files_count > MAX_FILES) files_count = MAX_FILES;
        // Read all file entries
        for (int i = 0; i < files_count; i++) {
            fread(&files[i], sizeof(FileEntry), 1, f);
            // Rebuild hashmap
            add_file_to_map(files[i].filename, i);
        }
        // Read users
        fread(&users_count, sizeof(int), 1, f);
        if (users_count > 256) users_count = 256;
        for (int i = 0; i < users_count; i++) {
            fread(users[i], 64, 1, f);
        }
        // Try to read access requests (may not exist in old metadata files)
        if (fread(&access_requests_count, sizeof(int), 1, f) == 1) {
            if (access_requests_count > MAX_ACCESS_REQUESTS) access_requests_count = MAX_ACCESS_REQUESTS;
            for (int i = 0; i < access_requests_count; i++) {
                fread(&access_requests[i], sizeof(AccessRequest), 1, f);
            }
        } else {
            access_requests_count = 0;
        }
        fclose(f);
    }
}

// Recursive function to display folder tree structure
static void display_folder_tree(int cfd, const char *base_path, const char *prefix, int is_last, FileEntry *files, int files_count) {
    int prefix_len = (int)strlen(base_path);
    
    // Collect all items under this folder (direct children only)
    typedef struct {
        char name[256];
        char full_path[512];
        int is_folder;
        int index;
    } FolderItem;
    
    FolderItem items[512];
    int item_count = 0;
    
    pthread_mutex_lock(&nm_mutex);
    for (int i=0; i<files_count; i++) {
        // Skip the folder itself
        if (strcmp(files[i].filename, base_path) == 0) {
            continue;
        }
        // Check if this item is a direct child
        int filename_len = (int)strlen(files[i].filename);
        if (filename_len > prefix_len + 1 &&
            strncmp(files[i].filename, base_path, prefix_len) == 0 && 
            files[i].filename[prefix_len] == '/') {
            const char *remainder = files[i].filename + prefix_len + 1;
            // Only direct children (no more '/' in remainder)
            if (strchr(remainder, '/') == NULL) {
                if (item_count < 512) {
                    strncpy(items[item_count].name, remainder, sizeof(items[item_count].name)-1);
                    items[item_count].name[sizeof(items[item_count].name)-1] = '\0';
                    strncpy(items[item_count].full_path, files[i].filename, sizeof(items[item_count].full_path)-1);
                    items[item_count].full_path[sizeof(items[item_count].full_path)-1] = '\0';
                    items[item_count].is_folder = files[i].is_folder;
                    items[item_count].index = i;
                    item_count++;
                }
            }
        }
    }
    pthread_mutex_unlock(&nm_mutex);
    
    // Sort items: folders first, then files, both alphabetically
    for (int i = 0; i < item_count - 1; i++) {
        for (int j = i + 1; j < item_count; j++) {
            int swap = 0;
            // Folders come before files
            if (items[i].is_folder && !items[j].is_folder) {
                swap = 0;  // i is folder, j is file - keep order
            } else if (!items[i].is_folder && items[j].is_folder) {
                swap = 1;  // i is file, j is folder - swap
            } else {
                // Both same type, sort alphabetically
                if (strcmp(items[i].name, items[j].name) > 0) {
                    swap = 1;
                }
            }
            if (swap) {
                FolderItem temp = items[i];
                items[i] = items[j];
                items[j] = temp;
            }
        }
    }
    
    // Display items
    for (int i = 0; i < item_count; i++) {
        int is_last_item = (i == item_count - 1);
        char buf[1024];
        
        // Build the tree prefix
        char tree_prefix[512];
        snprintf(tree_prefix, sizeof(tree_prefix), "%s", prefix);
        
        if (is_last_item) {
            snprintf(buf, sizeof(buf), "%s└── ", tree_prefix);
        } else {
            snprintf(buf, sizeof(buf), "%s├── ", tree_prefix);
        }
        
        if (items[i].is_folder) {
            char dir_buf[256];
            snprintf(dir_buf, sizeof(dir_buf), "[DIR] %s", items[i].name);
            strncat(buf, dir_buf, sizeof(buf) - strlen(buf) - 1);
        } else {
            strncat(buf, items[i].name, sizeof(buf) - strlen(buf) - 1);
        }
        
        net_send_line(cfd, buf);
        
        // If it's a folder, recursively display its contents
        if (items[i].is_folder) {
            char new_prefix[512];
            if (is_last_item) {
                snprintf(new_prefix, sizeof(new_prefix), "%s    ", tree_prefix);
            } else {
                snprintf(new_prefix, sizeof(new_prefix), "%s│   ", tree_prefix);
            }
            display_folder_tree(cfd, items[i].full_path, new_prefix, is_last_item, files, files_count);
        }
    }
}

// Thread function to handle client connection
static void* handle_client(void *arg) {
    // arg now contains both socket and client info
    typedef struct {
        int cfd;
        char client_ip[64];
        uint16_t client_port;
    } ClientContext;
    
    ClientContext *ctx = (ClientContext*)arg;
    int cfd = ctx->cfd;
    const char *client_ip = ctx->client_ip;
    uint16_t client_port = ctx->client_port;
    free(ctx);  // Free the allocated memory
    
    char user[64] = "";
    char line[1024];
    net_send_line(cfd, "WELCOME Docs++ NM. Please LOGIN <username>");
    while (1) {
        int n = net_recv_line(cfd, line, sizeof(line));
        if (n <= 0) break;
        trim(line);
        if (strncmp(line, "LOGIN ", 6) == 0) {
            char username_buf[64] = "";
            unsigned advertised_port = 0;
            int matched = sscanf(line+6, "%63s %u", username_buf, &advertised_port);
            if (matched < 1) {
                net_send_line(cfd, "ERR username required");
                continue;
            }
            strncpy(user, username_buf, sizeof(user)-1);
            user[sizeof(user)-1] = '\0';
            if (matched >= 2 && advertised_port > 0 && advertised_port < 65535) {
                client_port = (uint16_t)advertised_port;
            }
            char ok[256]; snprintf(ok, sizeof(ok), "OK LOGGED IN %s", user);
            net_send_line(cfd, ok);
            pthread_mutex_lock(&nm_mutex);
            int seen = 0; for (int i=0;i<users_count;i++) if (strcmp(users[i], user)==0) { seen=1; break; }
            if (!seen && users_count < 256) { strncpy(users[users_count++], user, 63); }
            pthread_mutex_unlock(&nm_mutex);
            // Log the login
            char log_details[128];
            snprintf(log_details, sizeof(log_details), "IP=%s Port=%u", client_ip, client_port);
            log_write("NM", "LOGIN", user, log_details, 0);
        } else if (strncmp(line, "VIEW REQUEST", 12)==0 || strncmp(line, "VIEWREQUEST", 11)==0) {
            // Handle "VIEW REQUEST" or "VIEWREQUEST" as alias for LISTREQUESTS
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256] = "";
            // Optional: VIEW REQUEST <filename> or just VIEW REQUEST for all
            int cmd_len = (strncmp(line, "VIEW REQUEST", 12)==0) ? 12 : 11;
            if (strlen(line) > cmd_len) {
                sscanf(line+cmd_len, "%255s", fname);
            }
            pthread_mutex_lock(&nm_mutex);
            int count = 0;
            for (int i=0; i<access_requests_count; i++) {
                // If filename specified, only show requests for that file
                if (fname[0] != '\0' && strcmp(access_requests[i].filename, fname) != 0) {
                    continue;
                }
                // Check if user is the owner of the file
                int idx = find_file_index(access_requests[i].filename);
                if (idx >= 0 && strcasecmp_safe(files[idx].owner, user)==0) {
                    if (count == 0) {
                        net_send_line(cfd, "PENDING ACCESS REQUESTS:");
                    }
                    char out[512];
                    char time_str[64];
                    time_t_to_ist_string(access_requests[i].request_time, time_str, sizeof(time_str));
                    snprintf(out, sizeof(out), "--> File: %s | User: %s | Type: %s | Requested: %s", 
                            access_requests[i].filename, access_requests[i].requesting_user, 
                            access_requests[i].access_type, time_str);
                    net_send_line(cfd, out);
                    count++;
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (count == 0) {
                if (fname[0] != '\0') {
                    net_send_line(cfd, "No pending requests for this file.");
                } else {
                    net_send_line(cfd, "No pending access requests.");
                }
            }
            net_send_line(cfd, "END");
        } else if (strcmp(line, "VIEW") == 0 || (strncmp(line, "VIEW ", 5)==0 && line[5]=='-')) {
    // parse flags (case-insensitive)
    // Only match "VIEW" or "VIEW -" (with flags), not "VIEW REQUEST" etc.
    char line_upper[1024];
    strncpy(line_upper, line, sizeof(line_upper)-1);
    line_upper[sizeof(line_upper)-1] = '\0';
    for (char *p = line_upper; *p; p++) *p = (char)toupper((unsigned char)*p);
    
    int show_all = 0, show_long = 0;
    char *flag_pos = strstr(line_upper, "-");
    if (flag_pos) {
        for (char *p = flag_pos + 1; *p && *p != ' '; p++) {
            if (*p == 'A') { show_all = 1; }
            if (*p == 'L') { show_long = 1; }
        }
    }
    
    // Send appropriate header
    if (show_long) {
        net_send_line(cfd, "-------------------------------------------------------------------");
        net_send_line(cfd, "|  Filename      | Words | Chars | Last Access Time  | Owner   |");
        net_send_line(cfd, "|----------------|-------|-------|-------------------|---------|");
    } else {
        net_send_line(cfd, "FILES:");
    }
    
    pthread_mutex_lock(&nm_mutex);
    for (int i=0;i<files_count;i++) {
        if (files[i].is_folder) continue;
        
        int can_view = show_all;
        if (!show_all) {
            if (user[0] != '\0') {
                if (strcasecmp_safe(files[i].owner, user)==0) can_view = 1;
                for (int r=0;r<files[i].readers_count;r++) {
                    if (strcasecmp_safe(files[i].readers[r], user)==0) { can_view = 1; break; }
                }
                for (int w=0;w<files[i].writers_count;w++) {
                    if (strcasecmp_safe(files[i].writers[w], user)==0) { can_view = 1; break; }
                }
            } else {
                can_view = 0;
            }
        }
        if (!can_view) continue;
        
        if (!show_long) {
            char buf[512]; 
            snprintf(buf, sizeof(buf), "--> %s", files[i].filename); 
            net_send_line(cfd, buf);
        } else {
            // Get stats from SS - find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            int found_ss = 0;
            for (int j = 0; j < ss_count; j++) {
                if (sss[j].is_active && strcmp(sss[j].ip, files[i].ss_ip) == 0 && 
                    sss[j].client_port == files[i].ss_client_port) {
                    strncpy(ss_ip, sss[j].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[j].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int j = 0; j < ss_count; j++) {
                    if (sss[j].is_active && !sss[j].is_primary && strcmp(sss[j].replica_of, "") != 0) {
                        for (int k = 0; k < ss_count; k++) {
                            if (strcmp(sss[k].ss_id, sss[j].replica_of) == 0 &&
                                strcmp(sss[k].ip, files[i].ss_ip) == 0 &&
                                sss[k].client_port == files[i].ss_client_port) {
                                strncpy(ss_ip, sss[j].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[j].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            long size = 0;
            int words = 0, chars = 0;
            
            if (found_ss) {
                int sfd = net_connect(ss_ip, admin_port);
                if (sfd >= 0) {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "INFO %s", files[i].filename);
                    net_send_line(sfd, cmd);
                    
                    char resp[512];
                    if (net_recv_line(sfd, resp, sizeof(resp)) > 0) {
                        sscanf(resp, "SIZE %ld WORDS %d CHARS %d", &size, &words, &chars);
                    }
                    net_close(sfd);
                }
            }
            
            // Format time in IST (UTC + 5:30)
            char time_str[64];
            time_t ist_time = files[i].last_access_time + (5 * 3600 + 30 * 60);
            struct tm *tm_info = gmtime(&ist_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);

            
            char row[512];
            snprintf(row, sizeof(row), "| %-14s | %5d | %5d | %-17s | %-7s |",
                     files[i].filename, words, chars, time_str, files[i].owner);
            net_send_line(cfd, row);
        }
    }
    pthread_mutex_unlock(&nm_mutex);
    
    if (show_long) {
        net_send_line(cfd, "-------------------------------------------------------------------");
    }
    net_send_line(cfd, "END");
}else if (strncmp(line, "CREATE ", 7) == 0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char *fname = line+7; 
            // Trim leading/trailing whitespace
            while (*fname == ' ' || *fname == '\t') fname++;
            char *end = fname + strlen(fname) - 1;
            while (end > fname && (*end == ' ' || *end == '\t')) *end-- = '\0';
            if (*fname=='\0'){ net_send_line(cfd, "ERR filename required"); continue; }
            // Validate filename
            if (!is_valid_filename(fname)) { net_send_line(cfd, "ERR invalid filename (must be alphanumeric with extension, no spaces)"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int exists = (find_file_index(fname) >= 0);
            if (exists) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, errcode_to_string(ERR_FILE_EXISTS)); goto cont; }
            if (ss_count == 0) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR no storage server available"); goto cont; }
            // choose first active primary SS (make a copy)
            SSInfo ss_copy = {0};
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_primary && sss[i].is_active) {
                    ss_copy = sss[i];
                    found_ss = 1;
                    break;
                }
            }
            // Fallback to any active SS
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active) {
                        ss_copy = sss[i];
                        found_ss = 1;
                        break;
                    }
                }
            }
            if (!found_ss) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR no active storage server"); goto cont; }
            pthread_mutex_unlock(&nm_mutex);
            // ask SS admin to create file
            int sfd = net_connect(ss_copy.ip, ss_copy.admin_port);
            if (sfd < 0) { net_send_line(cfd, "ERR cannot reach storage server"); goto cont; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "CREATE %s", fname);
            log_write("NM", "SS_CREATE", ss_copy.ss_id, fname, 0);
            net_send_line(sfd, cmd);
            char resp[512]; if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); goto cont; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2) != 0) { net_send_line(cfd, resp); goto cont; }
            
            // Async replication to replicas (don't wait for response)
            pthread_mutex_lock(&nm_mutex);
            for (int i = 0; i < ss_count; i++) {
                if (!sss[i].is_primary && strcmp(sss[i].replica_of, ss_copy.ss_id) == 0) {
                    // Send async replication request (non-blocking)
                    int rep_fd = net_connect(sss[i].ip, sss[i].admin_port);
                    if (rep_fd >= 0) {
                        char rep_log[256];
                        snprintf(rep_log, sizeof(rep_log), "REPLICATE_FILE %s target_ss=%s", fname, sss[i].ss_id);
                        log_write("NM", "REPLICATE_FILE", ss_copy.ss_id, rep_log, 0);
                        net_send_line(rep_fd, cmd);
                        net_close(rep_fd);  // Don't wait for response
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            // record file
            pthread_mutex_lock(&nm_mutex);
            if (files_count < MAX_FILES) {
                strncpy(files[files_count].filename, fname, sizeof(files[files_count].filename)-1);
                strncpy(files[files_count].owner, user, sizeof(files[files_count].owner)-1);
                strncpy(files[files_count].ss_ip, ss_copy.ip, sizeof(files[files_count].ss_ip)-1);
                files[files_count].ss_client_port = ss_copy.client_port;
                files[files_count].readers_count = 0;
                files[files_count].writers_count = 0;
                files[files_count].is_folder = 0;
                 // Initialize new metadata fields
                files[files_count].word_count = 0;
                files[files_count].char_count = 0;
                files[files_count].created_time = time(NULL);
                files[files_count].modified_time = time(NULL);
                files[files_count].last_access_time = time(NULL);
                int new_idx = files_count++;
                add_file_to_map(fname, new_idx);
            }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist to disk
            char create_log[512];
            snprintf(create_log, sizeof(create_log), "file=%s IP=%s Port=%u", fname, client_ip, client_port);
            log_write("NM", "CREATE", user, create_log, 0);
            net_send_line(cfd, "OK File Created Successfully!");
        } else if (strncmp(line, "READ ", 5) == 0) {
            char *fname = line+5; if (*fname=='\0'){ net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); char read_err_log[512]; snprintf(read_err_log, sizeof(read_err_log), "file=%s IP=%s Port=%u error=NOT_FOUND", fname, client_ip, client_port); log_write("NM", "READ", user, read_err_log, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            // access check: owner or in readers/writers (case-insensitive)
            int has_access = 0;
            if (user[0] != '\0' && strcasecmp_safe(files[idx].owner, user)!=0) {
                for (int r=0;r<files[idx].readers_count;r++) if (strcasecmp_safe(files[idx].readers[r], user)==0) { has_access=1; break; }
                if (!has_access) for (int w=0;w<files[idx].writers_count;w++) if (strcasecmp_safe(files[idx].writers[w], user)==0) { has_access=1; break; }
            } else if (user[0] != '\0') {
                has_access = 1;  // owner
            }
            char ss_ip[64]; uint16_t ss_port = 0;
            if (idx >= 0) {
                strncpy(ss_ip, files[idx].ss_ip, sizeof(ss_ip)-1);
                ss_port = files[idx].ss_client_port;
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!has_access) { char access_log[512]; snprintf(access_log, sizeof(access_log), "file=%s IP=%s Port=%u error=NO_ACCESS", fname, client_ip, client_port); log_write("NM", "READ", user, access_log, ERR_NO_ACCESS); net_send_line(cfd, errcode_to_string(ERR_NO_ACCESS)); continue; }
            char read_ok_log[512]; snprintf(read_ok_log, sizeof(read_ok_log), "file=%s IP=%s Port=%u SS=%s:%u", fname, client_ip, client_port, ss_ip, ss_port); log_write("NM", "READ", user, read_ok_log, 0);
            char file_loc_log[256]; snprintf(file_loc_log, sizeof(file_loc_log), "GET_FILE_LOCATION file=%s SS=%s:%u", fname, ss_ip, ss_port); log_write("NM", "GET_FILE_LOCATION", user, file_loc_log, 0);
            // Update last access time
            pthread_mutex_lock(&nm_mutex);
            if (idx >= 0) {
                files[idx].last_access_time = time(NULL);
                save_metadata();
            }
            pthread_mutex_unlock(&nm_mutex);

            // Tell client how to reach SS (simple inline for now)
            char buf[256]; snprintf(buf, sizeof(buf), "SS %s %u", ss_ip, ss_port);
            net_send_line(cfd, buf);
        } else if (strncmp(line, "WRITE ", 6) == 0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256]; int sidx=-1;
            if (sscanf(line+6, "%255s %d", fname, &sidx) < 2) { net_send_line(cfd, "ERR bad args"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); char write_err_log[512]; snprintf(write_err_log, sizeof(write_err_log), "file=%s IP=%s Port=%u error=NOT_FOUND", fname, client_ip, client_port); log_write("NM", "WRITE", user, write_err_log, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            // write access: owner or writers list (case-insensitive)
            int has_write = 0;
            if (strcasecmp_safe(files[idx].owner, user)==0) {
                has_write = 1;  // owner
            } else {
                for (int w=0;w<files[idx].writers_count;w++) if (strcasecmp_safe(files[idx].writers[w], user)==0) { has_write=1; break; }
            }
            char ss_ip[64]; uint16_t ss_port = 0;
            if (idx >= 0) {
                strncpy(ss_ip, files[idx].ss_ip, sizeof(ss_ip)-1);
                ss_port = files[idx].ss_client_port;
            }
            if (!has_write) {
                pthread_mutex_unlock(&nm_mutex);
                char write_noaccess_log[512]; snprintf(write_noaccess_log, sizeof(write_noaccess_log), "file=%s IP=%s Port=%u error=NO_WRITE_ACCESS", fname, client_ip, client_port); log_write("NM", "WRITE", user, write_noaccess_log, ERR_NO_WRITE_ACCESS);
                net_send_line(cfd, errcode_to_string(ERR_NO_WRITE_ACCESS));
                continue;
            }
            // UPDATE modified_time when WRITE is initiated
            if (idx >= 0) {
                files[idx].modified_time = time(NULL);
            }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist the updated timestamp
            char write_ok_log[512]; snprintf(write_ok_log, sizeof(write_ok_log), "file=%s IP=%s Port=%u SS=%s:%u", fname, client_ip, client_port, ss_ip, ss_port); log_write("NM", "WRITE", user, write_ok_log, 0);
            char file_loc_log2[256]; snprintf(file_loc_log2, sizeof(file_loc_log2), "GET_FILE_LOCATION file=%s SS=%s:%u", fname, ss_ip, ss_port); log_write("NM", "GET_FILE_LOCATION", user, file_loc_log2, 0);
            char buf[256]; snprintf(buf, sizeof(buf), "SS %s %u", ss_ip, ss_port);
            net_send_line(cfd, buf);
        } else if (strncmp(line, "STREAM ", 7) == 0) {
            char *fname = line+7; if (*fname=='\0'){ net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            // same access rule as READ (case-insensitive)
            pthread_mutex_lock(&nm_mutex);
            int idx_stream = find_file_index(fname);
            if (idx_stream<0){ pthread_mutex_unlock(&nm_mutex); char stream_err_log[512]; snprintf(stream_err_log, sizeof(stream_err_log), "file=%s IP=%s Port=%u error=NOT_FOUND", fname, client_ip, client_port); log_write("NM", "STREAM", user, stream_err_log, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            int has_access_stream = 0;
            if (user[0] != '\0' && strcasecmp_safe(files[idx_stream].owner, user)!=0) {
                for (int r=0;r<files[idx_stream].readers_count;r++) if (strcasecmp_safe(files[idx_stream].readers[r], user)==0) { has_access_stream=1; break; }
                if (!has_access_stream) for (int w=0;w<files[idx_stream].writers_count;w++) if (strcasecmp_safe(files[idx_stream].writers[w], user)==0) { has_access_stream=1; break; }
            } else if (user[0] != '\0') {
                has_access_stream = 1;  // owner
            }
            char ss_ip_stream[64]; uint16_t ss_port_stream = 0;
            if (idx_stream >= 0) {
                strncpy(ss_ip_stream, files[idx_stream].ss_ip, sizeof(ss_ip_stream)-1);
                ss_port_stream = files[idx_stream].ss_client_port;
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!has_access_stream) { char stream_noaccess_log[512]; snprintf(stream_noaccess_log, sizeof(stream_noaccess_log), "file=%s IP=%s Port=%u error=NO_ACCESS", fname, client_ip, client_port); log_write("NM", "STREAM", user, stream_noaccess_log, ERR_NO_ACCESS); net_send_line(cfd, errcode_to_string(ERR_NO_ACCESS)); continue; }
            char stream_ok_log[512]; snprintf(stream_ok_log, sizeof(stream_ok_log), "file=%s IP=%s Port=%u SS=%s:%u", fname, client_ip, client_port, ss_ip_stream, ss_port_stream); log_write("NM", "STREAM", user, stream_ok_log, 0);
            char buf_stream[256]; snprintf(buf_stream, sizeof(buf_stream), "SS %s %u", ss_ip_stream, ss_port_stream);
            net_send_line(cfd, buf_stream);
        } else if (strncmp(line, "EXEC ", 5) == 0) {
            char *fname = line+5; if (*fname=='\0'){ net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ char exec_err_log[512]; snprintf(exec_err_log, sizeof(exec_err_log), "file=%s IP=%s Port=%u error=NOT_FOUND", fname, client_ip, client_port); log_write("NM", "EXEC", user, exec_err_log, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            char exec_ok_log[512]; snprintf(exec_ok_log, sizeof(exec_ok_log), "file=%s IP=%s Port=%u", fname, client_ip, client_port); log_write("NM", "EXEC", user, exec_ok_log, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0, client_port_ss = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    client_port_ss = sss[i].client_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                client_port_ss = sss[i].client_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            // fetch file content from SS admin
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "FETCH %s", fname);
            net_send_line(sfd, cmd);
            char r[1024]; if (net_recv_line(sfd, r, sizeof(r))<=0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            int used_admin = 1;
            if (strcmp(r, "BEGIN")!=0) {
                // Fallback: use client READ if admin lacks FETCH
                net_close(sfd); used_admin = 0;
                int c2 = net_connect(ss_ip, client_port_ss);
                if (c2<0){ net_send_line(cfd, r); continue; }
                char w[256]; net_recv_line(c2, w, sizeof(w)); // welcome (may or may not be sent)
                char rcmd[512]; snprintf(rcmd, sizeof(rcmd), "READ %s", fname);
                net_send_line(c2, rcmd);
                if (net_recv_line(c2, r, sizeof(r))<=0) { net_close(c2); net_send_line(cfd, "ERR SS no response"); continue; }
                if (strncmp(r, "OK", 2)!=0) { net_close(c2); net_send_line(cfd, r); continue; }
                // receive content - treat whole file as bash script
                char content[1<<16]; content[0]='\0';
                // Read multiple lines until END or connection close
                char linebuf[1024]; int first_line = 1;
                while (net_recv_line(c2, linebuf, sizeof(linebuf)) > 0) {
                    if (strcmp(linebuf, "END") == 0) break;
                    // Some SS client implementations prefix lines with "L ". Strip it if present.
                    char *p = linebuf;
                    if (p[0] == 'L' && p[1] == ' ') p += 2;
                    if (!first_line) {
                        strncat(content, "\n", sizeof(content) - strlen(content) - 1);
                    }
                    first_line = 0;
                    strncat(content, p, sizeof(content) - strlen(content) - 1);
                }
                net_close(c2);
                if (content[0] == '\0') { net_send_line(cfd, "ERR empty"); continue; }
                if (!exec_command_allowed(content)) {
                    net_send_line(cfd, "ERR EXEC blocked; allowed commands: echo/ls/pwd (start NM with --exec-allow to override)");
                    continue;
                }
                // Write to temp script file and execute
                net_send_line(cfd, "OK");
                char tmp_script[512]; snprintf(tmp_script, sizeof(tmp_script), "nm_exec_tmp.sh");
                FILE *tf = fopen(tmp_script, "w");
                if (tf) {
                    fputs(content, tf);
                    fclose(tf);
#ifdef _WIN32
                    FILE *pp = _popen("cmd /c nm_exec_tmp.sh", "r");
#else
                    FILE *pp = popen("/bin/sh nm_exec_tmp.sh 2>&1", "r");
#endif
                    if (pp) {
                        char out[900];
                        while (fgets(out, sizeof(out), pp)) {
                            out[strcspn(out, "\r\n")] = 0;
                            net_send_line(cfd, out);
                        }
#ifdef _WIN32
                        _pclose(pp);
#else
                        pclose(pp);
#endif
                    }
                    remove(tmp_script);
                }
                net_send_line(cfd, "END");
                continue;
            }
            // assemble lines
            char script[1<<16]; script[0]='\0'; int first = 1;
            while (1) {
                if (net_recv_line(sfd, r, sizeof(r))<=0) { break; }
                if (strcmp(r, "END")==0) break;
                if (r[0]=='L' && r[1]==' ') {
                    if (!first) strncat(script, "\n", sizeof(script)-strlen(script)-1); first=0;
                    strncat(script, r+2, sizeof(script)-strlen(script)-1);
                }
            }
            net_close(sfd);
            // Treat whole script as bash script - write to temp file and execute
            if (!exec_command_allowed(script)) {
                net_send_line(cfd, "ERR EXEC blocked; allowed commands: echo/ls/pwd (start NM with --exec-allow to override)");
                continue;
            }
            net_send_line(cfd, "OK");
            char tmp_script[512]; snprintf(tmp_script, sizeof(tmp_script), "nm_exec_tmp.sh");
            FILE *tf = fopen(tmp_script, "w");
            if (tf) {
                fputs(script, tf);
                fclose(tf);
#ifdef _WIN32
                FILE *pp = _popen("cmd /c nm_exec_tmp.sh", "r");
#else
                FILE *pp = popen("/bin/sh nm_exec_tmp.sh 2>&1", "r");
#endif
                if (pp) {
                    char out[900];
                    while (fgets(out, sizeof(out), pp)) {
                        out[strcspn(out, "\r\n")] = 0;
                        net_send_line(cfd, out);
                    }
#ifdef _WIN32
                    _pclose(pp);
#else
                    pclose(pp);
#endif
                }
                remove(tmp_script);
            }
            net_send_line(cfd, "END");
        } else if (strncmp(line, "INFO ", 5) == 0) {
            char *fname = line+5; if (*fname=='\0'){ net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx >= 0) {
                // UPDATE last_access_time when INFO is called
                files[idx].last_access_time = time(NULL);
            }
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ char info_err_log[512]; snprintf(info_err_log, sizeof(info_err_log), "file=%s IP=%s Port=%u error=NOT_FOUND", fname, client_ip, client_port); log_write("NM", "INFO", user, info_err_log, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            char info_ok_log[512]; snprintf(info_ok_log, sizeof(info_ok_log), "file=%s IP=%s Port=%u", fname, client_ip, client_port); log_write("NM", "INFO", user, info_ok_log, 0);
            save_metadata();  // Persist the updated timestamp
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            // ask SS for info
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "INFO %s", fname);
            net_send_line(sfd, cmd);
            char resp[512]; if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(sfd);
            // resp: SIZE <bytes> WORDS <words> CHARS <chars>
            long size=0; int words=0, chars=0;
            sscanf(resp, "SIZE %ld WORDS %d CHARS %d", &size, &words, &chars);
            
            // Format timestamps in IST
            char created_str[64], modified_str[64], access_str[64];
            time_t_to_ist_string(files[idx].created_time, created_str, sizeof(created_str));
            time_t_to_ist_string(files[idx].modified_time, modified_str, sizeof(modified_str));
            time_t_to_ist_string(files[idx].last_access_time, access_str, sizeof(access_str));
            
            char out[512];
            snprintf(out, sizeof(out), "--> File: %s", files[idx].filename); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Owner: %s", files[idx].owner); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Created: %s", created_str); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Last Modified: %s", modified_str); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Size: %ld bytes", size); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Words: %d", words); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Chars: %d", chars); net_send_line(cfd, out);
            snprintf(out, sizeof(out), "--> Last Accessed: %s by %s", access_str, user); net_send_line(cfd, out);
            // Access list
            snprintf(out, sizeof(out), "--> Access: %s (RW)", files[idx].owner); net_send_line(cfd, out);
            for (int r=0;r<files[idx].readers_count;r++) {
                snprintf(out, sizeof(out), "--> Access: %s (R)", files[idx].readers[r]); net_send_line(cfd, out);
            }
            for (int w=0;w<files[idx].writers_count;w++) {
                snprintf(out, sizeof(out), "--> Access: %s (RW)", files[idx].writers[w]); net_send_line(cfd, out);
            }
            net_send_line(cfd, "END");
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char *fname = line+7;
            // Trim leading/trailing whitespace
            while (*fname == ' ' || *fname == '\t') fname++;
            char *end = fname + strlen(fname) - 1;
            while (end > fname && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';
            if (*fname == '\0') { net_send_line(cfd, "ERR filename required"); continue; }
            
            // Make a copy of filename since we'll be modifying the array
            char fname_copy[256];
            strncpy(fname_copy, fname, sizeof(fname_copy)-1);
            fname_copy[sizeof(fname_copy)-1] = '\0';
            
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname_copy);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR not found"); continue; }
            if (strcasecmp_safe(files[idx].owner, user)!=0) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR only owner can delete"); continue; }
            // Get the file's storage server info
            char file_ss_ip[64];
            uint16_t file_ss_client_port = 0;
            strncpy(file_ss_ip, files[idx].ss_ip, sizeof(file_ss_ip)-1);
            file_ss_ip[sizeof(file_ss_ip)-1] = '\0';
            file_ss_client_port = files[idx].ss_client_port;
            // Find the SSInfo entry that matches this file's storage server (primary or replica)
            SSInfo ss_copy = {0};
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && 
                    strcmp(sss[i].ip, file_ss_ip) == 0 && 
                    sss[i].client_port == file_ss_client_port) {
                    ss_copy = sss[i];
                    found_ss = 1;
                    break;
                }
            }
            // If primary not found, try to find a replica
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, file_ss_ip) == 0 &&
                                sss[j].client_port == file_ss_client_port) {
                                ss_copy = sss[i];
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            
            if (!found_ss) { net_send_line(cfd, "ERR storage server for file not found or inactive"); continue; }
            
            // Check if file is locked (WRITE in progress) - use separate connection
            int check_fd = net_connect(ss_copy.ip, ss_copy.admin_port);
            if (check_fd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char check_cmd[512]; snprintf(check_cmd, sizeof(check_cmd), "CHECKLOCK %s", fname_copy);
            net_send_line(check_fd, check_cmd);
            char lock_resp[256]; if (net_recv_line(check_fd, lock_resp, sizeof(lock_resp)) <= 0) { net_close(check_fd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(check_fd);  // Close CHECKLOCK connection (SS closes after one command)
            if (strncmp(lock_resp, "ERR", 3)==0) { net_send_line(cfd, "ERR file is locked for writing"); continue; }
            // File is not locked, proceed with deletion - use new connection
            int sfd = net_connect(ss_copy.ip, ss_copy.admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "DELETE %s", fname_copy);
            log_write("NM", "SS_DELETE", ss_copy.ss_id, fname_copy, 0);
            net_send_line(sfd, cmd);
            char resp[512]; if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2)!=0) { net_send_line(cfd, resp); continue; }
            
            // remove from table
            pthread_mutex_lock(&nm_mutex);
            // Verify idx is still valid and matches the file we want to delete
            if (idx >= 0 && idx < files_count && strcmp(files[idx].filename, fname_copy) == 0) {
                remove_file_from_map(fname_copy);
                // Shift remaining files down
                for (int j=idx+1; j<files_count; j++) {
                    files[j-1] = files[j];
                    // Update hashmap index for shifted file
                    add_file_to_map(files[j-1].filename, j-1);
                }
                files_count--;
            }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist to disk
            char delete_log[512]; snprintf(delete_log, sizeof(delete_log), "file=%s IP=%s Port=%u", fname_copy, client_ip, client_port); log_write("NM", "DELETE", user, delete_log, 0);
            char ok[256]; snprintf(ok, sizeof(ok), "OK File '%s' deleted successfully!", fname_copy); net_send_line(cfd, ok);
        } else if (strncmp(line, "UNDO ", 5) == 0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char *fname = line+5;
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); log_write("NM", "UNDO", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            // Check write permission (owner or writers)
            int has_write = 0;
            if (strcasecmp_safe(files[idx].owner, user)==0) has_write = 1;
            if (!has_write) {
                for (int w=0;w<files[idx].writers_count;w++) {
                    if (strcasecmp_safe(files[idx].writers[w], user)==0) { has_write=1; break; }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!has_write) { char undo_noaccess_log[512]; snprintf(undo_noaccess_log, sizeof(undo_noaccess_log), "file=%s IP=%s Port=%u error=NO_WRITE_ACCESS", fname, client_ip, client_port); log_write("NM", "UNDO", user, undo_noaccess_log, ERR_NO_WRITE_ACCESS); net_send_line(cfd, errcode_to_string(ERR_NO_WRITE_ACCESS)); continue; }
            char undo_ok_log[512]; snprintf(undo_ok_log, sizeof(undo_ok_log), "file=%s IP=%s Port=%u", fname, client_ip, client_port); log_write("NM", "UNDO", user, undo_ok_log, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "UNDO %s", fname);
            net_send_line(sfd, cmd);
            char resp[256]; if (net_recv_line(sfd, resp, sizeof(resp))<=0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2)==0) net_send_line(cfd, "OK Undo Successful!"); else net_send_line(cfd, resp);
        } else if (strncmp(line, "ADDACCESS ", 10)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char mode[8]; char fname[256]; char u2[64];
            // Format: ADDACCESS -R <filename> <username>  OR -W
            if (sscanf(line+10, "%7s %255s %63s", mode, fname, u2) != 3) { net_send_line(cfd, "ERR bad args"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); log_write("NM", "ADDACCESS", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            if (strcasecmp_safe(files[idx].owner, user)!=0) { pthread_mutex_unlock(&nm_mutex); log_write("NM", "ADDACCESS", user, fname, ERR_ONLY_OWNER); net_send_line(cfd, errcode_to_string(ERR_ONLY_OWNER)); continue; }
            if (strcmp(mode, "-R")==0) {
                // Check if user already has read access (case-insensitive)
                int already_has = 0;
                for (int r=0; r<files[idx].readers_count; r++) {
                    if (strcasecmp_safe(files[idx].readers[r], u2)==0) { already_has=1; break; }
                }
                if (!already_has && files[idx].readers_count < 64) {
                    strncpy(files[idx].readers[files[idx].readers_count++], u2, 63);
                }
            } else if (strcmp(mode, "-W")==0) {
                // Check if user already has write access (case-insensitive)
                int already_has = 0;
                for (int w=0; w<files[idx].writers_count; w++) {
                    if (strcasecmp_safe(files[idx].writers[w], u2)==0) { already_has=1; break; }
                }
                if (!already_has && files[idx].writers_count < 64) {
                    strncpy(files[idx].writers[files[idx].writers_count++], u2, 63);
                }
            } else { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR mode"); continue; }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist to disk
            char addaccess_log[512]; snprintf(addaccess_log, sizeof(addaccess_log), "file=%s mode=%s target=%s IP=%s Port=%u", fname, mode, u2, client_ip, client_port); log_write("NM", "ADDACCESS", user, addaccess_log, 0);
            net_send_line(cfd, "OK Access granted successfully!");
        } else if (strncmp(line, "REMACCESS ", 10)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256]; char u2[64];
            if (sscanf(line+10, "%255s %63s", fname, u2) != 2) { net_send_line(cfd, "ERR bad args"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); log_write("NM", "REMACCESS", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            if (strcasecmp_safe(files[idx].owner, user)!=0) { pthread_mutex_unlock(&nm_mutex); log_write("NM", "REMACCESS", user, fname, ERR_ONLY_OWNER); net_send_line(cfd, errcode_to_string(ERR_ONLY_OWNER)); continue; }
            // Remove from writers (case-insensitive)
            int w=0; for (int i=0;i<files[idx].writers_count;i++) if (strcasecmp_safe(files[idx].writers[i], u2)!=0) strncpy(files[idx].writers[w++], files[idx].writers[i], 63); files[idx].writers_count=w;
            // Remove from readers (case-insensitive)
            int r=0; for (int i=0;i<files[idx].readers_count;i++) if (strcasecmp_safe(files[idx].readers[i], u2)!=0) strncpy(files[idx].readers[r++], files[idx].readers[i], 63); files[idx].readers_count=r;
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist to disk
            char remaccess_log[512]; snprintf(remaccess_log, sizeof(remaccess_log), "file=%s target=%s IP=%s Port=%u", fname, u2, client_ip, client_port); log_write("NM", "REMACCESS", user, remaccess_log, 0);
            net_send_line(cfd, "OK Access removed successfully!");
        } else if (strncmp(line, "CHECKPOINT ", 11)==0) {
            char fname[256], tag[64];
            if (sscanf(line+11, "%255s %63s", fname, tag) != 2) { net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ log_write("NM", "CHECKPOINT", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            log_write("NM", "CHECKPOINT", user, fname, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "CHECKPOINT %s %s", fname, tag);
            net_send_line(sfd, cmd);
            char resp[256]; if (net_recv_line(sfd, resp, sizeof(resp))<=0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2)==0) net_send_line(cfd, "OK Checkpoint created successfully!"); else net_send_line(cfd, resp);
        } else if (strncmp(line, "VIEWCHECKPOINT ", 15)==0) {
            char fname[256], tag[64];
            if (sscanf(line+15, "%255s %63s", fname, tag) != 2) { net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ log_write("NM", "VIEWCHECKPOINT", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            log_write("NM", "VIEWCHECKPOINT", user, fname, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "VIEWCHECKPOINT %s %s", fname, tag);
            net_send_line(sfd, cmd);
            char resp[4096]; 
            if (net_recv_line(sfd, resp, sizeof(resp))<=0) { 
                net_close(sfd); 
                net_send_line(cfd, "ERR SS no response"); 
                continue; 
            }
            if (strncmp(resp, "OK", 2)==0) {
                // Read the content from SS (SS sends content as one line after "OK", but it may contain \n characters)
                char content[8192];
                if (net_recv_line(sfd, content, sizeof(content)) > 0) {
                    // Split content by newlines and send each line to client
                    char *p = content;
                    int has_content = 0;
                    while (*p) {
                        char line[4096];
                        int i = 0;
                        // Read until newline or end of string
                        while (*p && *p != '\n' && *p != '\r' && i < sizeof(line) - 1) {
                            line[i++] = *p++;
                        }
                        line[i] = '\0';
                        if (i > 0) {
                            net_send_line(cfd, line);
                            has_content = 1;
                        }
                        // Skip newline characters
                        while (*p == '\n' || *p == '\r') p++;
                    }
                    if (!has_content) {
                        // Empty checkpoint
                        net_send_line(cfd, "");
                    }
                } else {
                    // Empty checkpoint
                    net_send_line(cfd, "");
                }
                net_send_line(cfd, "END");
            } else {
                // Error from SS
                net_send_line(cfd, resp);
            }
            net_close(sfd);
        } else if (strncmp(line, "REVERT ", 7)==0) {
            char fname[256], tag[64];
            if (sscanf(line+7, "%255s %63s", fname, tag) != 2) { net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ log_write("NM", "REVERT", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            log_write("NM", "REVERT", user, fname, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "REVERT %s %s", fname, tag);
            net_send_line(sfd, cmd);
            char resp[256]; if (net_recv_line(sfd, resp, sizeof(resp))<=0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); continue; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2)==0) net_send_line(cfd, "OK File reverted successfully!"); else net_send_line(cfd, resp);
        } else if (strncmp(line, "LISTCHECKPOINTS ", 16)==0) {
            char fname[256];
            if (sscanf(line+16, "%255s", fname) != 1) { net_send_line(cfd, errcode_to_string(ERR_INVALID_ARGS)); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            pthread_mutex_unlock(&nm_mutex);
            if (idx<0){ log_write("NM", "LISTCHECKPOINTS", user, fname, ERR_FILE_NOT_FOUND); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            log_write("NM", "LISTCHECKPOINTS", user, fname, 0);
            // Find active SS for this file (primary or replica)
            char ss_ip[64]; uint16_t admin_port = 0;
            pthread_mutex_lock(&nm_mutex);
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && strcmp(sss[i].ip, files[idx].ss_ip) == 0 && 
                    sss[i].client_port == files[idx].ss_client_port) {
                    strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                    admin_port = sss[i].admin_port;
                    found_ss = 1;
                    break;
                }
            }
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, files[idx].ss_ip) == 0 &&
                                sss[j].client_port == files[idx].ss_client_port) {
                                strncpy(ss_ip, sss[i].ip, 63); ss_ip[63] = '\0';
                                admin_port = sss[i].admin_port;
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server unavailable"); continue; }
            int sfd = net_connect(ss_ip, admin_port);
            if (sfd<0){ net_send_line(cfd, "ERR SS not reachable"); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "LISTCHECKPOINTS %s", fname);
            net_send_line(sfd, cmd);
            char resp[512];
            while (1) {
                if (net_recv_line(sfd, resp, sizeof(resp))<=0) break;
                net_send_line(cfd, resp);
                if (strcmp(resp, "END")==0) break;
            }
            net_close(sfd);
        } else if (strncmp(line, "CREATEFOLDER ", 13)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char *fname = line+13;
            // Trim leading/trailing whitespace
            while (*fname == ' ' || *fname == '\t') fname++;
            char *end = fname + strlen(fname) - 1;
            while (end > fname && (*end == ' ' || *end == '\t')) *end-- = '\0';
            if (*fname=='\0'){ net_send_line(cfd, "ERR folder name required"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int exists = (find_file_index(fname) >= 0);
            if (exists) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR folder exists"); goto cont; }
            if (ss_count == 0) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR no storage server available"); goto cont; }
            // choose first active primary SS (make a copy)
            SSInfo ss_copy = {0};
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_primary && sss[i].is_active) {
                    ss_copy = sss[i];
                    found_ss = 1;
                    break;
                }
            }
            // Fallback to any active SS
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active) {
                        ss_copy = sss[i];
                        found_ss = 1;
                        break;
                    }
                }
            }
            if (!found_ss) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR no active storage server"); goto cont; }
            pthread_mutex_unlock(&nm_mutex);
            // ask SS admin to create folder
            int sfd = net_connect(ss_copy.ip, ss_copy.admin_port);
            if (sfd < 0) { net_send_line(cfd, "ERR cannot reach storage server"); goto cont; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "CREATEFOLDER %s", fname);
            net_send_line(sfd, cmd);
            char resp[512]; if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) { net_close(sfd); net_send_line(cfd, "ERR SS no response"); goto cont; }
            net_close(sfd);
            if (strncmp(resp, "OK", 2) != 0) { net_send_line(cfd, resp); goto cont; }
            
            // Async replication to replicas (don't wait for response)
            pthread_mutex_lock(&nm_mutex);
            for (int i = 0; i < ss_count; i++) {
                if (!sss[i].is_primary && strcmp(sss[i].replica_of, ss_copy.ss_id) == 0) {
                    // Send async replication request (non-blocking)
                    int rep_fd = net_connect(sss[i].ip, sss[i].admin_port);
                    if (rep_fd >= 0) {
                        net_send_line(rep_fd, cmd);
                        net_close(rep_fd);  // Don't wait for response
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            // record folder
            pthread_mutex_lock(&nm_mutex);
            if (files_count < MAX_FILES) {
                strncpy(files[files_count].filename, fname, sizeof(files[files_count].filename)-1);
                strncpy(files[files_count].owner, user, sizeof(files[files_count].owner)-1);
                strncpy(files[files_count].ss_ip, ss_copy.ip, sizeof(files[files_count].ss_ip)-1);
                files[files_count].ss_client_port = ss_copy.client_port;
                files[files_count].readers_count = 0;
                files[files_count].writers_count = 0;
                files[files_count].is_folder = 1;
                // Initialize new metadata fields
                files[files_count].word_count = 0;
                files[files_count].char_count = 0;
                files[files_count].created_time = time(NULL);
                files[files_count].modified_time = time(NULL);
                files[files_count].last_access_time = time(NULL);
                int new_idx = files_count++;
                add_file_to_map(fname, new_idx);
            }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();  // Persist to disk
            char create_log[512];
            snprintf(create_log, sizeof(create_log), "folder=%s IP=%s Port=%u", fname, client_ip, client_port);
            log_write("NM", "CREATEFOLDER", user, create_log, 0);
            net_send_line(cfd, "OK Folder created successfully!");
        } else if (strncmp(line, "MOVE ", 5)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256], foldername[256];
            if (sscanf(line+5, "%255s %255s", fname, foldername) != 2) { net_send_line(cfd, "ERR bad args"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int fidx = find_file_index(fname);
            int foldidx = find_file_index(foldername);
            if (foldidx >= 0 && !files[foldidx].is_folder) foldidx = -1;  // Ensure it's a folder
            if (fidx<0){ pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR file not found"); continue; }
            if (foldidx<0 || !files[foldidx].is_folder){ pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR folder not found"); continue; }
            if (strcasecmp_safe(files[fidx].owner, user)!=0) { pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, "ERR only owner can move"); continue; }
            int is_folder_item = files[fidx].is_folder;  // Store flag before unlocking
            // Get the file's storage server info
            char file_ss_ip[64];
            uint16_t file_ss_client_port = 0;
            strncpy(file_ss_ip, files[fidx].ss_ip, sizeof(file_ss_ip)-1);
            file_ss_ip[sizeof(file_ss_ip)-1] = '\0';
            file_ss_client_port = files[fidx].ss_client_port;
            // Find the SSInfo entry that matches this file's storage server (primary or replica)
            SSInfo ss_copy = {0};
            int found_ss = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && 
                    strcmp(sss[i].ip, file_ss_ip) == 0 && 
                    sss[i].client_port == file_ss_client_port) {
                    ss_copy = sss[i];
                    found_ss = 1;
                    break;
                }
            }
            // If primary not found, try to find a replica
            if (!found_ss) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && !sss[i].is_primary && strcmp(sss[i].replica_of, "") != 0) {
                        for (int j = 0; j < ss_count; j++) {
                            if (strcmp(sss[j].ss_id, sss[i].replica_of) == 0 &&
                                strcmp(sss[j].ip, file_ss_ip) == 0 &&
                                sss[j].client_port == file_ss_client_port) {
                                ss_copy = sss[i];
                                found_ss = 1;
                                break;
                            }
                        }
                        if (found_ss) break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (!found_ss) { net_send_line(cfd, "ERR storage server for file not found or inactive"); continue; }
            // Build new path - extract just the filename (basename) from fname
            const char *basename = fname;
            const char *last_slash = strrchr(fname, '/');
            if (last_slash) basename = last_slash + 1;
            char newpath[512]; snprintf(newpath, sizeof(newpath), "%s/%s", foldername, basename);
            // Check if target exists
            pthread_mutex_lock(&nm_mutex);
            int target_exists = (find_file_index(newpath) >= 0);
            pthread_mutex_unlock(&nm_mutex);
            if (target_exists) { net_send_line(cfd, "ERR target exists"); continue; }
            // Move file on SS
            int sfd = net_connect(ss_copy.ip, ss_copy.admin_port);
            if (sfd>=0) {
                char cmd[512]; snprintf(cmd, sizeof(cmd), "MOVE %s %s", fname, newpath);
                net_send_line(sfd, cmd);
                char resp[256]; 
                if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) {
                    net_close(sfd);
                    net_send_line(cfd, "ERR SS no response");
                    continue;
                }
                net_close(sfd);
                if (strncmp(resp, "OK", 2)==0) {
                    pthread_mutex_lock(&nm_mutex);
                    remove_file_from_map(fname);
                    strncpy(files[fidx].filename, newpath, sizeof(files[fidx].filename)-1);
                    add_file_to_map(newpath, fidx);
                    pthread_mutex_unlock(&nm_mutex);
                    save_metadata();  // Persist to disk
                    char move_log[512]; snprintf(move_log, sizeof(move_log), "file=%s to=%s IP=%s Port=%u", fname, newpath, client_ip, client_port);
                    log_write("NM", "MOVE", user, move_log, 0);
                    if (is_folder_item) {
                        net_send_line(cfd, "OK Folder moved successfully!");
                    } else {
                        net_send_line(cfd, "OK File moved successfully!");
                    }
                } else {
                    net_send_line(cfd, resp);
                }
            } else {
                net_send_line(cfd, "ERR SS not reachable");
            }
        } else if (strncmp(line, "VIEWFOLDER ", 11)==0) {
            char *foldername = line+11;
            // Trim leading/trailing whitespace
            while (*foldername == ' ' || *foldername == '\t') foldername++;
            char *end = foldername + strlen(foldername) - 1;
            while (end > foldername && (*end == ' ' || *end == '\t')) *end-- = '\0';
            
            pthread_mutex_lock(&nm_mutex);
            int foldidx = find_file_index(foldername);
            if (foldidx<0 || !files[foldidx].is_folder) foldidx = -1;
            pthread_mutex_unlock(&nm_mutex);
            if (foldidx<0){ net_send_line(cfd, "ERR folder not found"); continue; }
            
            net_send_line(cfd, "Contents of folder:");
            display_folder_tree(cfd, foldername, "", 1, files, files_count);
            net_send_line(cfd, "END");
        } else if (strcmp(line, "LIST")==0) {
            log_write("NM", "LIST", user, "", 0);
            net_send_line(cfd, "USERS:");
            for (int i=0;i<users_count;i++) { char out[128]; snprintf(out, sizeof(out), "--> %s", users[i]); net_send_line(cfd, out); }
            net_send_line(cfd, "END");
        } else if (strncmp(line, "REQUESTACCESS ", 14)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256];
            if (sscanf(line+14, "%255s", fname) != 1) { net_send_line(cfd, "ERR bad args"); continue; }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            // Check if user already has access or is owner
            int has_access = 0;
            if (strcasecmp_safe(files[idx].owner, user)==0) {
                has_access = 1;  // owner
            } else {
                for (int r=0;r<files[idx].readers_count;r++) {
                    if (strcasecmp_safe(files[idx].readers[r], user)==0) { has_access=1; break; }
                }
                if (!has_access) {
                    for (int w=0;w<files[idx].writers_count;w++) {
                        if (strcasecmp_safe(files[idx].writers[w], user)==0) { has_access=1; break; }
                    }
                }
            }
            if (has_access) {
                pthread_mutex_unlock(&nm_mutex);
                net_send_line(cfd, "ERR you already have access to this file");
                continue;
            }
            // Check if request already exists
            int request_exists = 0;
            for (int i=0; i<access_requests_count; i++) {
                if (strcmp(access_requests[i].filename, fname)==0 && 
                    strcasecmp_safe(access_requests[i].requesting_user, user)==0) {
                    request_exists = 1;
                    break;
                }
            }
            if (request_exists) {
                pthread_mutex_unlock(&nm_mutex);
                net_send_line(cfd, "ERR access request already pending");
                continue;
            }
            // Add request (default to read access)
            if (access_requests_count < MAX_ACCESS_REQUESTS) {
                strncpy(access_requests[access_requests_count].filename, fname, sizeof(access_requests[access_requests_count].filename)-1);
                strncpy(access_requests[access_requests_count].requesting_user, user, sizeof(access_requests[access_requests_count].requesting_user)-1);
                strncpy(access_requests[access_requests_count].access_type, "-R", sizeof(access_requests[access_requests_count].access_type)-1);
                access_requests[access_requests_count].request_time = time(NULL);
                access_requests_count++;
            }
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();
            char req_log[512]; snprintf(req_log, sizeof(req_log), "file=%s IP=%s Port=%u", fname, client_ip, client_port);
            log_write("NM", "REQUESTACCESS", user, req_log, 0);
            net_send_line(cfd, "OK Access request submitted successfully!");
        } else if (strncmp(line, "APPROVE_REQUEST ", 16)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256], req_user[64], access_mode[8] = "-R";
            // Format: APPROVE_REQUEST <filename> <requesting_user> [-W|-R]
            if (sscanf(line+16, "%255s %63s %7s", fname, req_user, access_mode) < 2) { 
                net_send_line(cfd, "ERR bad args"); continue; 
            }
            // If access_mode not provided, default to -R
            if (strcmp(access_mode, "-R") != 0 && strcmp(access_mode, "-W") != 0) {
                strncpy(access_mode, "-R", sizeof(access_mode)-1);
            }
            pthread_mutex_lock(&nm_mutex);
            int idx = find_file_index(fname);
            if (idx<0){ pthread_mutex_unlock(&nm_mutex); net_send_line(cfd, errcode_to_string(ERR_FILE_NOT_FOUND)); continue; }
            if (strcasecmp_safe(files[idx].owner, user)!=0) { 
                pthread_mutex_unlock(&nm_mutex); 
                net_send_line(cfd, errcode_to_string(ERR_ONLY_OWNER)); 
                continue; 
            }
            // Find and remove the request
            int found_request = -1;
            for (int i=0; i<access_requests_count; i++) {
                if (strcmp(access_requests[i].filename, fname)==0 && 
                    strcasecmp_safe(access_requests[i].requesting_user, req_user)==0) {
                    found_request = i;
                    break;
                }
            }
            if (found_request < 0) {
                pthread_mutex_unlock(&nm_mutex);
                net_send_line(cfd, "ERR no pending request found");
                continue;
            }
            // Grant access based on mode
            if (strcmp(access_mode, "-R")==0) {
                // Check if user already has read access
                int already_has = 0;
                for (int r=0; r<files[idx].readers_count; r++) {
                    if (strcasecmp_safe(files[idx].readers[r], req_user)==0) { already_has=1; break; }
                }
                if (!already_has && files[idx].readers_count < 64) {
                    strncpy(files[idx].readers[files[idx].readers_count++], req_user, 63);
                }
            } else if (strcmp(access_mode, "-W")==0) {
                // Check if user already has write access
                int already_has = 0;
                for (int w=0; w<files[idx].writers_count; w++) {
                    if (strcasecmp_safe(files[idx].writers[w], req_user)==0) { already_has=1; break; }
                }
                if (!already_has && files[idx].writers_count < 64) {
                    strncpy(files[idx].writers[files[idx].writers_count++], req_user, 63);
                }
            }
            // Remove the request
            for (int i=found_request; i<access_requests_count-1; i++) {
                access_requests[i] = access_requests[i+1];
            }
            access_requests_count--;
            pthread_mutex_unlock(&nm_mutex);
            save_metadata();
            char approve_log[512]; snprintf(approve_log, sizeof(approve_log), "file=%s user=%s mode=%s IP=%s Port=%u", fname, req_user, access_mode, client_ip, client_port);
            log_write("NM", "APPROVE_REQUEST", user, approve_log, 0);
            net_send_line(cfd, "OK Access request approved successfully!");
        } else if (strncmp(line, "LISTREQUESTS", 12)==0 || strncmp(line, "VIEWREQUESTS", 12)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char fname[256] = "";
            // Optional: LISTREQUESTS <filename> or just LISTREQUESTS for all
            // Also support VIEWREQUESTS as alias
            int cmd_len = (strncmp(line, "LISTREQUESTS", 12)==0) ? 12 : 12;
            if (strlen(line) > cmd_len) {
                sscanf(line+cmd_len, "%255s", fname);
            }
            pthread_mutex_lock(&nm_mutex);
            int count = 0;
            for (int i=0; i<access_requests_count; i++) {
                // If filename specified, only show requests for that file
                if (fname[0] != '\0' && strcmp(access_requests[i].filename, fname) != 0) {
                    continue;
                }
                // Check if user is the owner of the file
                int idx = find_file_index(access_requests[i].filename);
                if (idx >= 0 && strcasecmp_safe(files[idx].owner, user)==0) {
                    if (count == 0) {
                        net_send_line(cfd, "PENDING ACCESS REQUESTS:");
                    }
                    char out[512];
                    char time_str[64];
                    time_t_to_ist_string(access_requests[i].request_time, time_str, sizeof(time_str));
                    snprintf(out, sizeof(out), "--> File: %s | User: %s | Type: %s | Requested: %s", 
                            access_requests[i].filename, access_requests[i].requesting_user, 
                            access_requests[i].access_type, time_str);
                    net_send_line(cfd, out);
                    count++;
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            if (count == 0) {
                if (fname[0] != '\0') {
                    net_send_line(cfd, "No pending requests for this file.");
                } else {
                    net_send_line(cfd, "No pending access requests.");
                }
            }
            net_send_line(cfd, "END");
        } else if (strncmp(line, "SEARCH ", 7)==0) {
            if (user[0] == '\0') { net_send_line(cfd, "ERR please LOGIN first"); continue; }
            char keyword[256];
            if (sscanf(line+7, "%255s", keyword) != 1) { 
                net_send_line(cfd, "ERR bad args"); 
                continue; 
            }
            
            log_write("NM", "SEARCH", user, keyword, 0);
            
            // Collect all matching files from all active storage servers
            char all_results[2048][512];
            int total_matches = 0;
            
            pthread_mutex_lock(&nm_mutex);
            // Query all active storage servers
            for (int i = 0; i < ss_count && total_matches < 2048; i++) {
                if (!sss[i].is_active) continue;
                
                int sfd = net_connect(sss[i].ip, sss[i].admin_port);
                if (sfd >= 0) {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "SEARCH %s", keyword);
                    net_send_line(sfd, cmd);
                    
                    char resp[512];
                    if (net_recv_line(sfd, resp, sizeof(resp)) > 0 && strncmp(resp, "OK", 2) == 0) {
                        // Read file results until END
                        while (total_matches < 2048) {
                            if (net_recv_line(sfd, resp, sizeof(resp)) <= 0) break;
                            if (strcmp(resp, "END") == 0) break;
                            
                            // Check if file exists in our metadata and user has access
                            int file_idx = find_file_index(resp);
                            if (file_idx >= 0) {
                                // Check access permissions
                                int has_access = 0;
                                if (strcasecmp_safe(files[file_idx].owner, user) == 0) {
                                    has_access = 1;  // owner
                                } else {
                                    // Check readers
                                    for (int r = 0; r < files[file_idx].readers_count; r++) {
                                        if (strcasecmp_safe(files[file_idx].readers[r], user) == 0) {
                                            has_access = 1;
                                            break;
                                        }
                                    }
                                    // Check writers
                                    if (!has_access) {
                                        for (int w = 0; w < files[file_idx].writers_count; w++) {
                                            if (strcasecmp_safe(files[file_idx].writers[w], user) == 0) {
                                                has_access = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                                
                                if (has_access) {
                                    // Check for duplicates
                                    int is_duplicate = 0;
                                    for (int j = 0; j < total_matches; j++) {
                                        if (strcmp(all_results[j], resp) == 0) {
                                            is_duplicate = 1;
                                            break;
                                        }
                                    }
                                    if (!is_duplicate) {
                                        strncpy(all_results[total_matches], resp, sizeof(all_results[total_matches])-1);
                                        all_results[total_matches][sizeof(all_results[total_matches])-1] = '\0';
                                        total_matches++;
                                    }
                                }
                            }
                        }
                    }
                    net_close(sfd);
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            
            // Send results to client
            if (total_matches > 0) {
                net_send_line(cfd, "SEARCH RESULTS:");
                for (int i = 0; i < total_matches; i++) {
                    char out[512];
                    snprintf(out, sizeof(out), "--> %s", all_results[i]);
                    net_send_line(cfd, out);
                }
            } else {
                net_send_line(cfd, "No files found containing the keyword.");
            }
            net_send_line(cfd, "END");
        } else if (strcmp(line, "QUIT")==0) {
            net_send_line(cfd, "BYE"); break;
        } else {
            net_send_line(cfd, "ERR unknown command");
        }
        cont: ;
    }
    net_close(cfd);
    return NULL;  // Thread function must return void*
}

// Failure detection thread function
static void* check_ss_failures(void *arg) {
    (void)arg;
    while (1) {
        sleep(10);  // Check every 10 seconds
        time_t now = time(NULL);
        pthread_mutex_lock(&nm_mutex);
        for (int i = 0; i < ss_count; i++) {
            if (sss[i].is_active && (now - sss[i].last_heartbeat) > 30) {
                // SS hasn't sent heartbeat in 30 seconds - mark as failed
                sss[i].is_active = 0;
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), "SS %s marked as failed (no heartbeat for %ld seconds)", 
                         sss[i].ss_id, (long)(now - sss[i].last_heartbeat));
                log_write("NM", "SS_FAILURE", sss[i].ss_id, log_msg, 0);
                printf("[WARNING] SS %s marked as failed\n", sss[i].ss_id);
            }
        }
        pthread_mutex_unlock(&nm_mutex);
    }
    return NULL;
}

static void handle_ss_register(int cfd, const char *peer_ip) {
    // Expected: REGISTER <ss_id> <client_port>
    char ip[64];
    if (peer_ip && peer_ip[0]) {
        strncpy(ip, peer_ip, sizeof(ip)-1);
    } else {
        strncpy(ip, "127.0.0.1", sizeof(ip)-1);
    }
    ip[sizeof(ip)-1] = '\0';
    char line[512];
    if (net_recv_line(cfd, line, sizeof(line)) <= 0) { net_close(cfd); return; }
    if (strncmp(line, "REGISTER ", 9) != 0) { net_send_line(cfd, "ERR bad register"); net_close(cfd); return; }
    char ssid[64]; unsigned cp=0, ap=0;
    // Format: REGISTER <ss_id> <client_port> <admin_port> <ip>
    char tmp_ip[64]="127.0.0.1";
    int matched = sscanf(line+9, "%63s %u %u %63s", ssid, &cp, &ap, tmp_ip);
    if (matched < 3) { net_send_line(cfd, "ERR bad args"); net_close(cfd); return; }
    if (matched == 4) {
        strncpy(ip, tmp_ip, sizeof(ip)-1);
        ip[sizeof(ip)-1] = '\0';
    }
    if (peer_ip && peer_ip[0]) {
        strncpy(ip, peer_ip, sizeof(ip)-1);
        ip[sizeof(ip)-1] = '\0';
    }
    pthread_mutex_lock(&nm_mutex);
    // Check if this SS is reconnecting (already exists but was marked as failed)
    int found = 0;
    int reconnect_idx = -1;
    int was_inactive = 0;  // Track if SS was previously inactive
    for (int i = 0; i < ss_count; i++) {
        if (strcmp(sss[i].ss_id, ssid) == 0) {
            // SS exists - check if it was inactive (recovering from failure)
            was_inactive = !sss[i].is_active;
            // Update info and mark as active
            strncpy(sss[i].ip, ip, sizeof(sss[i].ip)-1);
            sss[i].ip[sizeof(sss[i].ip)-1] = '\0';
            sss[i].client_port = (uint16_t)cp;
            sss[i].admin_port = (uint16_t)ap;
            sss[i].last_heartbeat = time(NULL);
            sss[i].is_active = 1;
            found = 1;
            reconnect_idx = i;
            // Log heartbeat
            char hb_log[128];
            snprintf(hb_log, sizeof(hb_log), "SS %s heartbeat", ssid);
            log_write("NM", "HEARTBEAT", ssid, hb_log, 0);
            break;
        }
    }
    pthread_mutex_unlock(&nm_mutex);
    
    if (found) {
        // SS is reconnecting - synchronize files from replicas ONLY if it was previously inactive
        net_send_line(cfd, "OK REGISTERED");
        printf("Storage Server %s reconnected from %s (clients %u admin %u)\n", ssid, ip, cp, ap);
        net_close(cfd);
        
        // Only perform recovery if SS was previously inactive (recovering from failure)
        if (was_inactive) {
            // Perform synchronization in background (don't block registration)
            // Find all files that should be on this SS and sync them from replicas
            pthread_mutex_lock(&nm_mutex);
            if (reconnect_idx < 0 || reconnect_idx >= ss_count) {
                pthread_mutex_unlock(&nm_mutex);
                return;
            }
            SSInfo recovered_ss_copy = sss[reconnect_idx];  // Make a copy
        char recovered_ss_ip[64];
        uint16_t recovered_admin_port = recovered_ss_copy.admin_port;
        uint16_t recovered_client_port = recovered_ss_copy.client_port;
        strncpy(recovered_ss_ip, recovered_ss_copy.ip, sizeof(recovered_ss_ip)-1);
        recovered_ss_ip[sizeof(recovered_ss_ip)-1] = '\0';
        char recovered_ss_id[64];
        strncpy(recovered_ss_id, recovered_ss_copy.ss_id, sizeof(recovered_ss_id)-1);
        recovered_ss_id[sizeof(recovered_ss_id)-1] = '\0';
        
        // Collect files that should be on this SS
        int files_to_sync[1024];
        int sync_count = 0;
        for (int i = 0; i < files_count && sync_count < 1024; i++) {
            // Check if this file should be on the recovered SS
            if (strcmp(files[i].ss_ip, recovered_ss_ip) == 0 && 
                files[i].ss_client_port == recovered_client_port) {
                files_to_sync[sync_count++] = i;
            }
        }
        pthread_mutex_unlock(&nm_mutex);
        
        // Sync each file from a replica
        for (int sync_idx = 0; sync_idx < sync_count; sync_idx++) {
            int file_idx = files_to_sync[sync_idx];
            char fname[256];
            pthread_mutex_lock(&nm_mutex);
            if (file_idx >= 0 && file_idx < files_count) {
                strncpy(fname, files[file_idx].filename, sizeof(fname)-1);
                fname[sizeof(fname)-1] = '\0';
            } else {
                pthread_mutex_unlock(&nm_mutex);
                continue;
            }
            pthread_mutex_unlock(&nm_mutex);
            
            // Find an active replica that has this file
            pthread_mutex_lock(&nm_mutex);
            SSInfo replica_ss_copy = {0};
            int found_replica = 0;
            for (int i = 0; i < ss_count; i++) {
                if (sss[i].is_active && !sss[i].is_primary && 
                    strcmp(sss[i].replica_of, recovered_ss_id) == 0) {
                    replica_ss_copy = sss[i];
                    found_replica = 1;
                    break;
                }
            }
            // If no direct replica, find any active SS that might have the file
            if (!found_replica) {
                for (int i = 0; i < ss_count; i++) {
                    if (sss[i].is_active && i != reconnect_idx) {
                        replica_ss_copy = sss[i];
                        found_replica = 1;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&nm_mutex);
            
            if (found_replica) {
                // Fetch file from replica
                int rep_fd = net_connect(replica_ss_copy.ip, replica_ss_copy.admin_port);
                if (rep_fd >= 0) {
                    char fetch_cmd[512];
                    snprintf(fetch_cmd, sizeof(fetch_cmd), "FETCH %s", fname);
                    net_send_line(rep_fd, fetch_cmd);
                    
                    char resp[1024];
                    if (net_recv_line(rep_fd, resp, sizeof(resp)) > 0 && 
                        strcmp(resp, "BEGIN") == 0) {
                        // Read file content
                        char file_content[65536] = "";
                        int first_line = 1;
                        while (1) {
                            if (net_recv_line(rep_fd, resp, sizeof(resp)) <= 0) break;
                            if (strcmp(resp, "END") == 0) break;
                            // Skip "L " prefix if present
                            char *content_line = resp;
                            if (resp[0] == 'L' && resp[1] == ' ') {
                                content_line = resp + 2;
                            }
                            if (!first_line) {
                                strncat(file_content, "\n", sizeof(file_content) - strlen(file_content) - 1);
                            }
                            first_line = 0;
                            strncat(file_content, content_line, sizeof(file_content) - strlen(file_content) - 1);
                        }
                        
                        // Sync to recovered SS
                        int sync_fd = net_connect(recovered_ss_ip, recovered_admin_port);
                        if (sync_fd >= 0) {
                            char sync_cmd[512];
                            snprintf(sync_cmd, sizeof(sync_cmd), "SYNC %s", fname);
                            net_send_line(sync_fd, sync_cmd);
                            
                            char sync_resp[256];
                            if (net_recv_line(sync_fd, sync_resp, sizeof(sync_resp)) > 0 && 
                                strncmp(sync_resp, "OK", 2) == 0) {
                                // Send content line by line
                                char *p = file_content;
                                while (*p) {
                                    char line[4096];
                                    int i = 0;
                                    while (*p && *p != '\n' && i < sizeof(line) - 1) {
                                        line[i++] = *p++;
                                    }
                                    line[i] = '\0';
                                    if (i > 0) {
                                        net_send_line(sync_fd, line);
                                    }
                                    if (*p == '\n') p++;
                                }
                                net_send_line(sync_fd, "END");
                                net_recv_line(sync_fd, sync_resp, sizeof(sync_resp));
                            }
                            net_close(sync_fd);
                        }
                    }
                    net_close(rep_fd);
                }
            }
            }
            
            char sync_log[512];
            snprintf(sync_log, sizeof(sync_log), "SS %s recovered, synchronized %d files", ssid, sync_count);
            log_write("NM", "SS_RECOVERY", ssid, sync_log, 0);
        }
        return;  // Return after handling reconnection
    }
    
    pthread_mutex_lock(&nm_mutex);
    if (!found && ss_count < MAX_SS) {
        // New SS registration
        strncpy(sss[ss_count].ss_id, ssid, sizeof(sss[ss_count].ss_id)-1);
        strncpy(sss[ss_count].ip, ip, sizeof(sss[ss_count].ip)-1);
        sss[ss_count].client_port = (uint16_t)cp;
        sss[ss_count].admin_port = (uint16_t)ap;
        sss[ss_count].is_primary = 1;  // First SS is primary
        sss[ss_count].replica_of[0] = '\0';
        sss[ss_count].last_heartbeat = time(NULL);
        sss[ss_count].is_active = 1;
        ss_count++;
        
        // Assign replica if we have multiple SS
        if (ss_count >= 2 && ss_count % 2 == 0) {
            // Make this one a replica of the previous one
            sss[ss_count-1].is_primary = 0;
            strncpy(sss[ss_count-1].replica_of, sss[ss_count-2].ss_id, sizeof(sss[ss_count-1].replica_of)-1);
        }
    }
    pthread_mutex_unlock(&nm_mutex);
    char ss_reg_log[256];
    snprintf(ss_reg_log, sizeof(ss_reg_log), "REGISTER_SS %s %u %u", ip, cp, ap);
    log_write("NM", "REGISTER_SS", ssid, ss_reg_log, 0);
    net_send_line(cfd, "OK REGISTERED");
    printf("Storage Server %s registered from %s (clients %u admin %u)\n", ssid, ip, cp, ap);
    net_close(cfd);
}

int main(int argc, char **argv) {
    // Initialize hashmap and cache for efficient lookups
    file_map = hashmap_create();
    file_cache = lru_cache_create();
    if (!file_map || !file_cache) {
        fprintf(stderr, "Failed to initialize hashmap/cache\n");
        return 1;
    }
    
    // Initialize logging
    log_init("logs/nm.log");
    
    // Load persisted metadata
    load_metadata();
    
    load_nm_config_defaults();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(nm_bind_host, argv[++i], sizeof(nm_bind_host)-1);
            nm_bind_host[sizeof(nm_bind_host)-1] = '\0';
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            nm_client_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ss-port") == 0 && i + 1 < argc) {
            nm_ss_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            nm_verbose = 1;
        } else if (strcmp(argv[i], "--exec-allow") == 0) {
            nm_exec_allow_all = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_nm_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_nm_usage(argv[0]);
            return 1;
        }
    }
    if (nm_ss_port == 0) {
        nm_ss_port = (uint16_t)(nm_client_port + 1);
    }
    net_set_verbose(nm_verbose);
    int cfd = net_listen_addr(nm_bind_host, nm_client_port);
    int sfd = net_listen_addr(nm_bind_host, nm_ss_port);
    if (cfd < 0 || sfd < 0) { fprintf(stderr, "Failed to listen on ports\n"); return 1; }
    printf("Name Server listening on %s:%u (clients) and %s:%u (storage registration)\n",
           nm_bind_host, nm_client_port, nm_bind_host, nm_ss_port);
    if (!nm_exec_allow_all) {
        printf("EXEC restricted to safe commands (enable --exec-allow to run arbitrary scripts)\n");
    } else {
        printf("EXEC unrestricted (use with caution)\n");
    }
    log_write("NM", "STARTUP", "SYSTEM", "Naming Server started", 0);
    
    // Start failure detection thread
    pthread_t failure_thread;
    pthread_create(&failure_thread, NULL, (void*(*)(void*))check_ss_failures, NULL);
    pthread_detach(failure_thread);
    
    while (1) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = (cfd>sfd?cfd:sfd);
#ifdef _WIN32
        SOCKET scfd = cfd, ssfd = sfd;
#endif
        FD_SET(cfd, &rfds); FD_SET(sfd, &rfds);
        struct timeval tv; tv.tv_sec=1; tv.tv_usec=0;
        int rc = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (rc > 0) {
            if (FD_ISSET(cfd, &rfds)) {
                char ip[64] = "";  // Initialize to empty string
                uint16_t p; int cl = net_accept(cfd, ip, sizeof(ip), &p);
                if (cl>=0) {
                    // Handle each client in a separate thread for true concurrency
                    // All threads share the same files array, users array, and SS list
                    pthread_t thread;
                    typedef struct {
                        int cfd;
                        char client_ip[64];
                        uint16_t client_port;
                    } ClientContext;
                    ClientContext *ctx = (ClientContext*)malloc(sizeof(ClientContext));
                    ctx->cfd = cl;
                    // Ensure IP is valid, use "127.0.0.1" if empty (shouldn't happen with our fix)
                    if (ip[0] == '\0') {
                        strncpy(ctx->client_ip, "127.0.0.1", sizeof(ctx->client_ip)-1);
                    } else {
                        strncpy(ctx->client_ip, ip, sizeof(ctx->client_ip)-1);
                    }
                    ctx->client_ip[sizeof(ctx->client_ip)-1] = '\0';  // Ensure null termination
                    ctx->client_port = p;
                    // Log client registration
                    char reg_client_log[256];
                    snprintf(reg_client_log, sizeof(reg_client_log), "REGISTER_CLIENT IP=%s Port=%u", ctx->client_ip, ctx->client_port);
                    log_write("NM", "REGISTER_CLIENT", "SYSTEM", reg_client_log, 0);
                    if (pthread_create(&thread, NULL, handle_client, ctx) != 0) {
                        // Thread creation failed, handle in main thread (fallback)
                        handle_client(ctx);
                    } else {
                        pthread_detach(thread);  // Don't wait for thread to finish
                    }
                }
            }
            if (FD_ISSET(sfd, &rfds)) {
                char ip[64]; uint16_t p; int sl = net_accept(sfd, ip, sizeof(ip), &p);
                if (sl>=0) { handle_ss_register(sl, ip); }
            }
        }
    }
}


