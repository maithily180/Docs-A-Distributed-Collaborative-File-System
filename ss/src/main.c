#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif
#include "../../lib/include/net.h"
#include "../../lib/include/util.h"
#include "../../lib/include/log.h"

typedef struct {
    char nm_ip[64];
    uint16_t nm_reg_port;
    uint16_t client_port;
    uint16_t admin_port;
    char ss_id[64];
    char advertise_ip[64];
} HeartbeatArgs;

static char data_root[256] = "ss/data";
static char undo_root[256] = "ss/undo";
static char checkpoint_root[256] = "ss/checkpoints";

// Per-file, per-sentence locking for true concurrent access
typedef struct {
    char filename[256];
    int locked_sentences[2048];  // -1 = unlocked, thread_id = locked
    pthread_mutex_t file_mutex;
    int ref_count;
} FileLock;

#define MAX_LOCKED_FILES 256
static FileLock file_locks[MAX_LOCKED_FILES];
static int file_locks_count = 0;
static pthread_mutex_t locks_table_mutex = PTHREAD_MUTEX_INITIALIZER;

// Get or create file lock entry
static FileLock* get_file_lock(const char *filename) {
    pthread_mutex_lock(&locks_table_mutex);
    FileLock *fl = NULL;
    for (int i=0; i<file_locks_count; i++) {
        if (strcmp(file_locks[i].filename, filename) == 0) {
            fl = &file_locks[i];
            fl->ref_count++;
            pthread_mutex_unlock(&locks_table_mutex);
            return fl;
        }
    }
    if (file_locks_count < MAX_LOCKED_FILES) {
        fl = &file_locks[file_locks_count++];
        strncpy(fl->filename, filename, sizeof(fl->filename)-1);
        pthread_mutex_init(&fl->file_mutex, NULL);
        for (int i=0; i<2048; i++) fl->locked_sentences[i] = -1;
        fl->ref_count = 1;
    }
    pthread_mutex_unlock(&locks_table_mutex);
    return fl;
}

static void release_file_lock(FileLock *fl) {
    if (!fl) return;
    pthread_mutex_lock(&locks_table_mutex);
    fl->ref_count--;
    if (fl->ref_count <= 0) {
        pthread_mutex_destroy(&fl->file_mutex);
        // Remove from table (simplified - just mark as unused)
        fl->filename[0] = '\0';
    }
    pthread_mutex_unlock(&locks_table_mutex);
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

// Heartbeat thread function
static void* send_heartbeat(void *arg) {
    HeartbeatArgs *args = (HeartbeatArgs*)arg;
    while (1) {
        sleep(20);  // Send heartbeat every 20 seconds
        int rfd = net_connect(args->nm_ip, args->nm_reg_port);
        if (rfd >= 0) {
            char hostip[64];
            if (args->advertise_ip[0] != '\0') {
                strncpy(hostip, args->advertise_ip, sizeof(hostip)-1);
                hostip[sizeof(hostip)-1] = '\0';
            } else {
                strncpy(hostip, "0.0.0.0", sizeof(hostip)-1);
                hostip[sizeof(hostip)-1] = '\0';
            }
            char reg[256]; snprintf(reg, sizeof(reg), "REGISTER %s %u %u %s", 
                                    args->ss_id, args->client_port, args->admin_port, hostip);
            net_send_line(rfd, reg);
            char resp[256]; net_recv_line(rfd, resp, sizeof(resp));
            net_close(rfd);
        }
    }
    free(arg);
    return NULL;
}

static void* handle_client_conn(void *arg) {
    int cfd = *(int*)arg;
    free(arg);  // Free the allocated memory
    char line[1024];
    if (net_send_line(cfd, "WELCOME SS CLIENT") != 0) { net_close(cfd); return NULL; }
    while (1) {
        if (net_recv_line(cfd, line, sizeof(line)) <= 0) break;
        // Handle READ command
        if (strncmp(line, "READ ", 5) == 0) {
        char *fname = line + 5;
    
            // Build file path
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", data_root, fname);
    
            // Open file
            FILE *f = fopen(path, "r");
            if (!f) {
                log_write("SS", "READ", "client", fname, -1);
                net_send_line(cfd, "ERR file not found");
                continue;
            }
    
            // Send OK
            net_send_line(cfd, "OK");
    
            // Send file content line by line
            char line[4096];
            while (fgets(line, sizeof(line), f)) {
                // Remove trailing newline
                line[strcspn(line, "\r\n")] = 0;
                net_send_line(cfd, line);
            }
    
            fclose(f);
    
            // ✅ CRITICAL: Send END marker
            net_send_line(cfd, "END");
            
            // Log successful READ
            log_write("SS", "READ", "client", fname, 0);
        }
        else if (strncmp(line, "WRITE_BEGIN ", 12)==0) {
            char fname[256]; int sidx=-1;
            if (sscanf(line+12, "%255s %d", fname, &sidx) < 2) { net_send_line(cfd, "ERR bad args"); continue; }
            if (sidx < 0) { net_send_line(cfd, "ERR invalid sentence index"); continue; }

    char vpath[512];
    snprintf(vpath, sizeof(vpath), "%s/%s", data_root, fname);
    char *vbuf = NULL;
    int vlen = 0;
    
    int file_exists = (read_file_all(vpath, &vbuf, &vlen) == 0);
    
    if (file_exists && vbuf != NULL && vlen > 0) {
        // File exists with content - count sentences
        int sentence_count = 0;
        int in_sentence = 0;
        int last_has_delimiter = 0;
        
        for (int i = 0; i < vlen; i++) {
            char ch = vbuf[i];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                in_sentence = 1;
                last_has_delimiter = 0;
            }
            if (ch == '.' || ch == '!' || ch == '?') {
                sentence_count++;
                in_sentence = 0;
                last_has_delimiter = 1;
            }
        }
        
        if (in_sentence) {
            sentence_count++;
            last_has_delimiter = 0;
        }
        
        free(vbuf);
        
        // Determine max allowed sentence index
        // If last sentence is complete (has delimiter), can append new sentence
        // If last sentence is incomplete, cannot append
        int max_allowed = last_has_delimiter ? sentence_count : sentence_count - 1;
        
        if (sidx > max_allowed) {
            char err[256];
            snprintf(err, sizeof(err), "ERR: Sentence index out of range (max: %d)", max_allowed);
            net_send_line(cfd, err);
            continue;
        }
    } else {
        // File doesn't exist or is empty - only sentence 0 valid
        if (sidx > 0) {
            net_send_line(cfd, "ERR: Sentence index out of range (file is empty)");
            continue;
        }
    }


            
            FileLock *fl = get_file_lock(fname);
            if (!fl) { net_send_line(cfd, "ERR too many locked files"); continue; }
            
            pthread_mutex_lock(&fl->file_mutex);
            // Check if sentence is already locked
            if (sidx < 2048 && fl->locked_sentences[sidx] != -1) {
                pthread_mutex_unlock(&fl->file_mutex);
                release_file_lock(fl);
                net_send_line(cfd, "ERR sentence locked");
                continue;
            }
            // Lock the sentence (use connection fd as identifier)
            if (sidx < 2048) fl->locked_sentences[sidx] = cfd;
            pthread_mutex_unlock(&fl->file_mutex);
            
            // Create swap file for this write session (copy original to swap file)
            // This ensures STREAM always reads original file while WRITE modifies swap
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            char swappath[512]; snprintf(swappath, sizeof(swappath), "%s/%s.swap.%d", data_root, fname, cfd);
            char *buf=NULL; int len=0;
            if (read_file_all(path, &buf, &len) == 0) {
                // File exists - copy to swap file and create undo snapshot
                write_file_all(swappath, buf, len);
                char upath[512]; snprintf(upath, sizeof(upath), "%s/%s.bak", undo_root, fname);
                write_file_all(upath, buf, len);
                free(buf);
            } else {
                // File doesn't exist - create empty swap file
                write_file_all(swappath, "", 0);
            }
            // Send lock info: filename and sentence index (we'll track this per connection)
            char lock_info[512]; snprintf(lock_info, sizeof(lock_info), "OK lock %s %d", fname, sidx);
            net_send_line(cfd, lock_info);
            
            // Log WRITE_BEGIN
            char write_begin_log[512]; snprintf(write_begin_log, sizeof(write_begin_log), "file=%s sentence=%d", fname, sidx);
            log_write("SS", "WRITE_BEGIN", "client", write_begin_log, 0);
        } else if (strncmp(line, "WRITE_UPDATE ", 13)==0) {
            // Parse lock info from line: WRITE_UPDATE <filename> <sentence_index> <word_index> <content>
            char fname[256]; int sidx=-1, widx=-1; char content[768];
            const char *p = line+13; while (*p==' ') p++;
            // Try to parse: filename sentence_index word_index content
            if (sscanf(p, "%255s %d %d %767[^\n\r]", fname, &sidx, &widx, content) < 4) {
                net_send_line(cfd, "ERR bad args");
                continue;
            }
            
            FileLock *fl = get_file_lock(fname);
            if (!fl) { net_send_line(cfd, "ERR file not found"); continue; }
            
            pthread_mutex_lock(&fl->file_mutex);
            // Verify this connection owns the lock
            if (sidx >= 2048 || fl->locked_sentences[sidx] != cfd) {
                pthread_mutex_unlock(&fl->file_mutex);
                release_file_lock(fl);
                net_send_line(cfd, "ERR not locked by this session");
                continue;
            }
            pthread_mutex_unlock(&fl->file_mutex);
            
            // load from SWAP file (not the real file!)
            // This ensures STREAM always reads the original file
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            char swappath[512]; snprintf(swappath, sizeof(swappath), "%s/%s.swap.%d", data_root, fname, cfd);
            char *buf=NULL; int len=0; if (read_file_all(swappath, &buf, &len)!=0) { buf=strdup(""); len=0; }
            // split into sentences by .!? keeping delimiters as terminators
            // build dynamic array
            char *text = buf; int n_sent=0; char *sents[2048]; int slens[2048];
            char *start = text;
            // Skip leading whitespace
            while (start < text+len && (*start==' ' || *start=='\t' || *start=='\n' || *start=='\r')) start++;
            for (int i=(int)(start-text);i<len;i++) {
                char ch = text[i];
                if (ch=='.' || ch=='!' || ch=='?') {
                    int seglen = (int)((text+i+1) - start);
                    if (n_sent < 2048 && seglen > 0) { 
                        sents[n_sent] = (char*)malloc(seglen+1); 
                        memcpy(sents[n_sent], start, seglen); 
                        sents[n_sent][seglen]='\0'; 
                        slens[n_sent]=seglen; 
                        n_sent++; 
                    }
                    start = text+i+1;
                    // Skip whitespace after delimiter
                    while (start < text+len && (*start==' ' || *start=='\t' || *start=='\n' || *start=='\r')) start++;
                }
            }
            if (start < text+len) {
                int seglen = (int)((text+len) - start);
                // Trim trailing whitespace
                while (seglen > 0 && (start[seglen-1]==' ' || start[seglen-1]=='\t' || start[seglen-1]=='\n' || start[seglen-1]=='\r')) seglen--;
                if (n_sent < 2048 && seglen > 0) { 
                    sents[n_sent] = (char*)malloc(seglen+1); 
                    memcpy(sents[n_sent], start, seglen); 
                    sents[n_sent][seglen]='\0'; 
                    slens[n_sent]=seglen; 
                    n_sent++; 
                }
            }
            if (n_sent==0) { sents[n_sent]=strdup(""); slens[n_sent]=0; n_sent++; }

// ✅ CORRECTED: Auto-create sentences if needed (don't validate!)
// During a WRITE session, new sentences can be created via delimiters
// So we need to allow dynamic sentence creation up to sidx
while (sidx >= n_sent && n_sent < 2048) {
    sents[n_sent] = strdup("");
    slens[n_sent] = 0;
    n_sent++;
}
if (sidx >= n_sent) {
    // Shouldn't happen, but handle gracefully
    sidx = n_sent - 1;
}

            // words split by spaces, insert content at widx
            char *sent = sents[sidx];
            // Make a copy of sentence to avoid modifying original
            char sent_copy[4096]; strncpy(sent_copy, sent, sizeof(sent_copy)-1); sent_copy[sizeof(sent_copy)-1]='\0';
            // build words array from copy
            char *wp = sent_copy; char *words[4096]; int wc=0;
            while (*wp) {
                while (*wp==' ') wp++;
                if (!*wp) break;
                words[wc++] = wp;
                while (*wp && *wp!=' ') wp++;
                if (*wp) { *wp='\0'; wp++; }
            }
            
            if (widx < 0) {
    net_send_line(cfd, "ERR: Word index cannot be negative");
    for (int i=0;i<n_sent;i++) free(sents[i]);
    free(buf);
    release_file_lock(fl);
    continue;
}

int max_word_index = wc + 1;
if (widx > max_word_index) {
    char err[256];
    snprintf(err, sizeof(err), "ERR: Word index out of range (max: %d)", max_word_index);
    net_send_line(cfd, err);
    for (int i=0;i<n_sent;i++) free(sents[i]);
    free(buf);
    release_file_lock(fl);
    continue;
}

int insert_pos = widx;
if (insert_pos > wc) insert_pos = wc;

            // build new sentence with inserted content tokens
            char newsent[4096]; newsent[0]='\0';
            // copy words before widx
            for (int i=0;i<insert_pos;i++) {
                if (i>0) strcat(newsent, " ");
                strcat(newsent, words[i]);
            }
            // Add space before content if there are words before it
            if (insert_pos > 0) strcat(newsent, " ");
            // Add the new content
            strcat(newsent, content);
            // Add space after content if there are words after it
            if (wc > insert_pos) strcat(newsent, " ");
            // Add words after insertion point
            for (int i=insert_pos;i<wc;i++) {
                strcat(newsent, words[i]);
                if (i < wc-1) strcat(newsent, " ");
            }
            // Replace the sentence with the new content
            // Note: If newsent contains delimiters, they will be preserved in the sentence
            // The file will be rebuilt and when read back, it will be split naturally
            free(sents[sidx]); 
            sents[sidx] = strdup(newsent); 
            slens[sidx] = (int)strlen(newsent);
            // rebuild full text - sentences already contain their delimiters
            // Need to ensure proper spacing between sentences
            char rebuilt[1<<20]; rebuilt[0]='\0';
            for (int i=0;i<n_sent;i++) {
                if (strlen(sents[i]) == 0) continue; // Skip empty sentences
                if (strlen(rebuilt) > 0) {
                    // Check if we need to add space between sentences
                    int prev_len = (int)strlen(rebuilt);
                    // If previous doesn't end with space and current doesn't start with space, add one
                    if (prev_len > 0 && rebuilt[prev_len-1] != ' ' && rebuilt[prev_len-1] != '\t' && 
                        sents[i][0] != ' ' && sents[i][0] != '\t') {
                        strcat(rebuilt, " ");
                    }
                }
                strcat(rebuilt, sents[i]);
            }
            // persist to SWAP file (not the real file!)
            pthread_mutex_lock(&fl->file_mutex);
            write_file_all(swappath, rebuilt, (int)strlen(rebuilt));
            pthread_mutex_unlock(&fl->file_mutex);
            
            for (int i=0;i<n_sent;i++) free(sents[i]);
            free(buf);
            release_file_lock(fl);
            char write_update_log[512]; snprintf(write_update_log, sizeof(write_update_log), "file=%s sentence=%d word=%d", fname, sidx, widx);
            log_write("SS", "WRITE_UPDATE", "client", write_update_log, 0);
            net_send_line(cfd, "OK updated");
        } else if (strncmp(line, "WRITE_END", 9)==0) {
            // Parse: WRITE_END <filename> <sentence_index>
            char fname[256]; int sidx=-1;
            if (sscanf(line+9, "%255s %d", fname, &sidx) >= 2) {
                // Move swap file to real file (atomically commit the write)
                char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
                char swappath[512]; snprintf(swappath, sizeof(swappath), "%s/%s.swap.%d", data_root, fname, cfd);
                
                // Read swap file and write to real file
                char *buf=NULL; int len=0;
                if (read_file_all(swappath, &buf, &len) == 0) {
                    write_file_all(path, buf, len);
                    free(buf);
                }
                // Clean up swap file
                remove(swappath);
                
                FileLock *fl = get_file_lock(fname);
                if (fl) {
                    pthread_mutex_lock(&fl->file_mutex);
                    if (sidx >= 0 && sidx < 2048 && fl->locked_sentences[sidx] == cfd) {
                        fl->locked_sentences[sidx] = -1;  // Release lock
                    }
                    pthread_mutex_unlock(&fl->file_mutex);
                    release_file_lock(fl);
                }
                char write_end_log[512]; snprintf(write_end_log, sizeof(write_end_log), "file=%s sentence=%d", fname, sidx);
                log_write("SS", "WRITE_END", "client", write_end_log, 0);
            }
            net_send_line(cfd, "OK end");
        } else if (strncmp(line, "STREAM ", 7)==0) {
            char *fname = line+7; char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            char *buf=NULL; int len=0;
            if (read_file_all(path, &buf, &len) != 0) { 
                log_write("SS", "STREAM", "client", fname, -1);
                net_send_line(cfd, "ERR not found"); 
            }
            else {
                net_send_line(cfd, "OK");
                // stream word by word (split by spaces)
                char *p = buf; while (*p) {
                    while (*p==' '||*p=='\t'||*p=='\n' || *p=='\r') p++;
                    if (!*p) break;
                    char word[256]; int wi=0; while (*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r' && wi<255) { word[wi++]=*p++; }
                    word[wi]='\0';
                    net_send_line(cfd, word);
#ifdef _WIN32
                    Sleep(100);
#else
                    usleep(100000);
#endif
                }
                net_send_line(cfd, "STOP");
                free(buf);
                
                // Log successful STREAM
                log_write("SS", "STREAM", "client", fname, 0);
            }
        } else if (strcmp(line, "QUIT")==0) { net_send_line(cfd, "BYE"); break; }
        else { net_send_line(cfd, "ERR unknown"); }
    }
    net_close(cfd);
    return NULL;
}

static int handle_admin_conn(int afd) {
    char line[1024];
    if (net_recv_line(afd, line, sizeof(line)) <= 0) { net_close(afd); return -1; }
    if (strncmp(line, "CREATE ", 7)==0) {
        char *fname = line+7; 
        // Validate filename
        if (!is_valid_filename(fname)) { net_send_line(afd, "ERR invalid filename (must be alphanumeric with extension, no spaces)"); }
        else {
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            // Create directory structure if path has folders
            char *p = path;
            while (*p) {
                if (*p == '/' || *p == '\\') {
                    char save = *p; *p = '\0';
                    mkpath(path);
                    *p = save;
                }
                p++;
            }
            const char *empty = "";
            if (write_file_all(path, empty, 0) != 0) { 
                log_write("SS", "CREATE", "admin", fname, -1);
                net_send_line(afd, "ERR create"); 
            }
            else { 
                log_write("SS", "CREATE", "admin", fname, 0);
                net_send_line(afd, "OK created"); 
            }
        }
    } else if (strncmp(line, "CHECKLOCK ", 10)==0) {
        char *fname = line+10;
        pthread_mutex_lock(&locks_table_mutex);
        int has_lock = 0;
        for (int i=0; i<file_locks_count; i++) {
            if (strcmp(file_locks[i].filename, fname) == 0) {
                pthread_mutex_lock(&file_locks[i].file_mutex);
                for (int j=0; j<2048; j++) {
                    if (file_locks[i].locked_sentences[j] != -1) {
                        has_lock = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&file_locks[i].file_mutex);
                break;
            }
        }
        pthread_mutex_unlock(&locks_table_mutex);
        if (has_lock) net_send_line(afd, "ERR file locked");
        else net_send_line(afd, "OK not locked");
    } else if (strncmp(line, "DELETE ", 7)==0) {
        char *fname = line+7; char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
        if (remove(path)==0) { 
            log_write("SS", "DELETE", "admin", fname, 0);
            net_send_line(afd, "OK deleted"); 
        } else { 
            log_write("SS", "DELETE", "admin", fname, -1);
            net_send_line(afd, "ERR delete");
        }
    } else if (strncmp(line, "CREATEFOLDER ", 13)==0) {
        char *fname = line+13;
        // Trim leading/trailing whitespace
        while (*fname == ' ' || *fname == '\t') fname++;
        char *end = fname + strlen(fname) - 1;
        while (end > fname && (*end == ' ' || *end == '\t')) *end-- = '\0';
        if (*fname == '\0') {
            net_send_line(afd, "ERR folder name required");
        } else {
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            // Create directory structure if path has folders
            char *p = path;
            while (*p) {
                if (*p == '/' || *p == '\\') {
                    char save = *p; *p = '\0';
                    mkpath(path);
                    *p = save;
                }
                p++;
            }
            // Create the folder directory itself
            if (mkpath(path) == 0) {
                log_write("SS", "CREATEFOLDER", "admin", fname, 0);
                net_send_line(afd, "OK created");
            } else {
                log_write("SS", "CREATEFOLDER", "admin", fname, -1);
                net_send_line(afd, "ERR create");
            }
        }
    } else if (strncmp(line, "INFO ", 5)==0) {
        char *fname = line+5; char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
        FILE *f = fopen(path, "rb");
        if (!f) { 
            log_write("SS", "INFO", "admin", fname, -1);
            net_send_line(afd, "SIZE 0 WORDS 0 CHARS 0");
        } else {
            fseek(f,0,SEEK_END); long sz = ftell(f); fseek(f,0,SEEK_SET);
            char *content = (char*)malloc(sz+1);
            if (content) {
                fread(content, 1, sz, f);
                content[sz] = '\0';
                // Count words and chars
                int words = 0, chars = (int)sz;
                const char *p = content;
                int in_word = 0;
                while (*p) {
                    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                        if (in_word) { words++; in_word = 0; }
                    } else {
                        in_word = 1;
                    }
                    p++;
                }
                if (in_word) words++;
                char resp[128]; snprintf(resp, sizeof(resp), "SIZE %ld WORDS %d CHARS %d", sz, words, chars);
                log_write("SS", "INFO", "admin", fname, 0);
                net_send_line(afd, resp);
                free(content);
            } else {
                char resp[64]; snprintf(resp, sizeof(resp), "SIZE %ld WORDS 0 CHARS 0", sz);
                log_write("SS", "INFO", "admin", fname, 0);
                net_send_line(afd, resp);
            }
            fclose(f);
        }
    } else if (strncmp(line, "FETCH ", 7)==0) {
        char *fname = line+7; char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
        FILE *f = fopen(path, "rb");
        if (!f) { 
            log_write("SS", "FETCH", "admin", fname, -1);
            net_send_line(afd, "ERR not found"); 
        } else {
            log_write("SS", "FETCH", "admin", fname, 0);
            net_send_line(afd, "BEGIN");
            char lbuf[900];
            while (fgets(lbuf, sizeof(lbuf), f)) {
                // strip trailing newlines to keep protocol line-based
                lbuf[strcspn(lbuf, "\r\n")] = 0;
                char out[1024]; snprintf(out, sizeof(out), "L %s", lbuf);
                net_send_line(afd, out);
            }
            fclose(f);
            net_send_line(afd, "END");
        }
    } else if (strncmp(line, "UNDO ", 5)==0) {
        char *fname = line+5; char upath[512]; snprintf(upath, sizeof(upath), "%s/%s.bak", undo_root, fname);
        char *buf=NULL; int len=0; 
        if (read_file_all(upath, &buf, &len)!=0) { 
            log_write("SS", "UNDO", "admin", fname, -1);
            net_send_line(afd, "ERR undo"); 
        } else { 
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname); 
            write_file_all(path, buf, len); 
            free(buf); 
            remove(upath); 
            log_write("SS", "UNDO", "admin", fname, 0);
            net_send_line(afd, "OK undo"); 
        }
    } else if (strncmp(line, "CHECKPOINT ", 11)==0) {
        char fname[256], tag[64];
        if (sscanf(line+11, "%255s %63s", fname, tag) != 2) { 
            log_write("SS", "CHECKPOINT", "admin", fname, -1);
            net_send_line(afd, "ERR bad args"); 
        } else {
            char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            char *buf=NULL; int len=0;
            if (read_file_all(path, &buf, &len) != 0) { 
                log_write("SS", "CHECKPOINT", "admin", fname, -1);
                net_send_line(afd, "ERR not found"); 
            } else {
                char cpath[512]; snprintf(cpath, sizeof(cpath), "%s/%s/%s", checkpoint_root, fname, tag);
                mkpath(cpath);
                snprintf(cpath, sizeof(cpath), "%s/%s/%s/file", checkpoint_root, fname, tag);
                write_file_all(cpath, buf, len);
                free(buf);
                log_write("SS", "CHECKPOINT", "admin", fname, 0);
                net_send_line(afd, "OK checkpoint created");
            }
        }
    } else if (strncmp(line, "VIEWCHECKPOINT ", 15)==0) {
        char fname[256], tag[64];
        if (sscanf(line+15, "%255s %63s", fname, tag) != 2) { net_send_line(afd, "ERR bad args"); }
        else {
            char cpath[512]; snprintf(cpath, sizeof(cpath), "%s/%s/%s/file", checkpoint_root, fname, tag);
            char *buf=NULL; int len=0;
            if (read_file_all(cpath, &buf, &len) != 0) { net_send_line(afd, "ERR not found"); }
            else { net_send_line(afd, "OK"); net_send_line(afd, buf); free(buf); }
        }
    } else if (strncmp(line, "REVERT ", 7)==0) {
        char fname[256], tag[64];
        if (sscanf(line+7, "%255s %63s", fname, tag) != 2) { 
            log_write("SS", "REVERT", "admin", fname, -1);
            net_send_line(afd, "ERR bad args"); 
        } else {
            char cpath[512]; snprintf(cpath, sizeof(cpath), "%s/%s/%s/file", checkpoint_root, fname, tag);
            char *buf=NULL; int len=0;
            if (read_file_all(cpath, &buf, &len) != 0) { 
                log_write("SS", "REVERT", "admin", fname, -1);
                net_send_line(afd, "ERR not found"); 
            } else {
                char path[512]; snprintf(path, sizeof(path), "%s/%s", data_root, fname);
                write_file_all(path, buf, len);
                free(buf);
                log_write("SS", "REVERT", "admin", fname, 0);
                net_send_line(afd, "OK reverted");
            }
        }
    } else if (strncmp(line, "LISTCHECKPOINTS ", 16)==0) {
        char fname[256];
        if (sscanf(line+16, "%255s", fname) != 1) { 
            log_write("SS", "LISTCHECKPOINTS", "admin", fname, -1);
            net_send_line(afd, "ERR bad args"); 
        } else {
            char dir[512]; snprintf(dir, sizeof(dir), "%s/%s", checkpoint_root, fname);
            log_write("SS", "LISTCHECKPOINTS", "admin", fname, 0);
            net_send_line(afd, "CHECKPOINTS:");
            // Simple: list directory entries (platform-specific, simplified)
            char cmd[1024];
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd), "dir /b \"%s\" 2>nul", dir);
#else
            snprintf(cmd, sizeof(cmd), "ls \"%s\" 2>/dev/null", dir);
#endif
            FILE *fp = popen(cmd, "r");
            if (fp) {
                char tag[256];
                while (fgets(tag, sizeof(tag), fp)) {
                    tag[strcspn(tag, "\r\n")] = 0;
                    if (strlen(tag) > 0) {
                        char out[512]; snprintf(out, sizeof(out), "--> %s", tag);
                        net_send_line(afd, out);
                    }
                }
                pclose(fp);
            }
            net_send_line(afd, "END");
        }
    } else if (strncmp(line, "MOVE ", 5)==0) {
        char oldpath[512], newpath[512];
        if (sscanf(line+5, "%511s %511s", oldpath, newpath) != 2) { 
            log_write("SS", "MOVE", "admin", oldpath, -1);
            net_send_line(afd, "ERR bad args"); 
        } else {
            char opath[512], npath[512];
            snprintf(opath, sizeof(opath), "%s/%s", data_root, oldpath);
            snprintf(npath, sizeof(npath), "%s/%s", data_root, newpath);
            // Create directory for new path
            char *p = npath;
            while (*p) {
                if (*p == '/' || *p == '\\') {
                    char save = *p; *p = '\0';
                    mkpath(npath);
                    *p = save;
                }
                p++;
            }
            // Rename/move file
            if (rename(opath, npath) == 0) {
                log_write("SS", "MOVE", "admin", oldpath, 0);
                net_send_line(afd, "OK moved");
            } else {
                log_write("SS", "MOVE", "admin", oldpath, -1);
                net_send_line(afd, "ERR move failed");
            }
        }
    } else if (strncmp(line, "SYNC ", 5)==0) {
        // SYNC <filename> - receives file content and writes it
        // Format: SYNC <filename>
        // Then receives content lines until END
        char fname[256];
        if (sscanf(line+5, "%255s", fname) != 1) {
            log_write("SS", "SYNC", "admin", "", -1);
            net_send_line(afd, "ERR bad args");
        } else {
            log_write("SS", "SYNC", "admin", fname, 0);
            net_send_line(afd, "OK");
            // Read content until END
            char content[65536] = "";
            int first_line = 1;
            while (1) {
                char line_buf[4096];
                if (net_recv_line(afd, line_buf, sizeof(line_buf)) <= 0) break;
                if (strcmp(line_buf, "END") == 0) break;
                if (!first_line) {
                    strncat(content, "\n", sizeof(content) - strlen(content) - 1);
                }
                first_line = 0;
                strncat(content, line_buf, sizeof(content) - strlen(content) - 1);
            }
            // Write file
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", data_root, fname);
            // Create directory structure if needed
            char *p = path;
            while (*p) {
                if (*p == '/' || *p == '\\') {
                    char save = *p; *p = '\0';
                    mkpath(path);
                    *p = save;
                }
                p++;
            }
            if (write_file_all(path, content, (int)strlen(content)) == 0) {
                net_send_line(afd, "OK synced");
            } else {
                net_send_line(afd, "ERR sync failed");
            }
        }
    } else if (strncmp(line, "SEARCH ", 7)==0) {
        char keyword[256];
        if (sscanf(line+7, "%255s", keyword) != 1) {
            log_write("SS", "SEARCH", "admin", "", -1);
            net_send_line(afd, "ERR bad args");
        } else {
            log_write("SS", "SEARCH", "admin", keyword, 0);
            // Search through all files in data_root
            // List all files and search their contents
            int match_count = 0;
            char results[1024][512];  // Store matching filenames
            
            // Use a simple directory listing approach
            // For each file in data_root, check if it contains the keyword
            char search_cmd[1024];
#ifdef _WIN32
            snprintf(search_cmd, sizeof(search_cmd), "dir /b /s \"%s\" 2>nul", data_root);
#else
            snprintf(search_cmd, sizeof(search_cmd), "find \"%s\" -type f 2>/dev/null", data_root);
#endif
            FILE *fp = popen(search_cmd, "r");
            if (fp) {
                char filepath[512];
                while (fgets(filepath, sizeof(filepath), fp) && match_count < 1024) {
                    // Remove trailing newline
                    filepath[strcspn(filepath, "\r\n")] = 0;
                    if (strlen(filepath) == 0) continue;
                    
                    // Read file and search for keyword
                    char *buf = NULL;
                    int len = 0;
                    if (read_file_all(filepath, &buf, &len) == 0 && buf != NULL) {
                        // Case-insensitive search
                        char *content_lower = (char*)malloc(len + 1);
                        char *keyword_lower = (char*)malloc(strlen(keyword) + 1);
                        if (content_lower && keyword_lower) {
                            // Convert to lowercase for case-insensitive search
                            for (int i = 0; i < len; i++) {
                                content_lower[i] = (char)tolower((unsigned char)buf[i]);
                            }
                            content_lower[len] = '\0';
                            for (int i = 0; keyword[i]; i++) {
                                keyword_lower[i] = (char)tolower((unsigned char)keyword[i]);
                            }
                            keyword_lower[strlen(keyword)] = '\0';
                            
                            // Search for keyword
                            if (strstr(content_lower, keyword_lower) != NULL) {
                                // Extract relative path from data_root
                                const char *rel_path = filepath;
                                int data_root_len = (int)strlen(data_root);
                                if (strncmp(filepath, data_root, data_root_len) == 0) {
                                    rel_path = filepath + data_root_len;
                                    if (*rel_path == '/' || *rel_path == '\\') rel_path++;
                                }
                                strncpy(results[match_count], rel_path, sizeof(results[match_count])-1);
                                results[match_count][sizeof(results[match_count])-1] = '\0';
                                match_count++;
                            }
                            free(content_lower);
                            free(keyword_lower);
                        }
                        free(buf);
                    }
                }
                pclose(fp);
            }
            
            // Send results
            if (match_count > 0) {
                net_send_line(afd, "OK");
                for (int i = 0; i < match_count; i++) {
                    net_send_line(afd, results[i]);
                }
            } else {
                net_send_line(afd, "OK");
            }
            net_send_line(afd, "END");
        }
    } else {
        net_send_line(afd, "ERR unknown");
    }
    net_close(afd); return 0;
}

static void print_ss_usage(const char *prog) {
    printf("Usage: %s [--host IP] [--client-port PORT] [--admin-port PORT] [--nm-ip IP] [--nm-port PORT] [--ss-id NAME] [--advertise-ip IP] [--verbose]\n", prog);
    printf("Defaults: host=0.0.0.0, client-port=9000, admin-port=9100, nm-ip=127.0.0.1, nm-port=8000\n");
}

int main(int argc, char **argv) {
    char bind_host[64] = "0.0.0.0";
    uint16_t client_port = 9000;
    uint16_t admin_port = 9100;
    char nm_ip[64] = "127.0.0.1";
    uint16_t nm_reg_port = 8000;
    char ss_id[64] = "ss1";
    char advertise_ip[64] = "";
    int verbose = 0;

    char cfg_host[64];
    if (config_get_string("ss.host", cfg_host, sizeof(cfg_host))) {
        strncpy(bind_host, cfg_host, sizeof(bind_host)-1);
        bind_host[sizeof(bind_host)-1] = '\0';
    }
    uint16_t cfg_client_port;
    if (config_get_uint16("ss.client_port", &cfg_client_port) && cfg_client_port != 0) {
        client_port = cfg_client_port;
    }
    char cfg_nm_ip[64];
    if (config_get_string("nm.host", cfg_nm_ip, sizeof(cfg_nm_ip))) {
        strncpy(nm_ip, cfg_nm_ip, sizeof(nm_ip)-1);
        nm_ip[sizeof(nm_ip)-1] = '\0';
    }
    uint16_t cfg_nm_port;
    if (config_get_uint16("nm.port", &cfg_nm_port) && cfg_nm_port != 0) {
        nm_reg_port = cfg_nm_port;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(bind_host, argv[++i], sizeof(bind_host)-1);
            bind_host[sizeof(bind_host)-1] = '\0';
        } else if (strcmp(argv[i], "--client-port") == 0 && i + 1 < argc) {
            client_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--admin-port") == 0 && i + 1 < argc) {
            admin_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nm-ip") == 0 && i + 1 < argc) {
            strncpy(nm_ip, argv[++i], sizeof(nm_ip)-1);
            nm_ip[sizeof(nm_ip)-1] = '\0';
        } else if (strcmp(argv[i], "--nm-port") == 0 && i + 1 < argc) {
            nm_reg_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ss-id") == 0 && i + 1 < argc) {
            strncpy(ss_id, argv[++i], sizeof(ss_id)-1);
            ss_id[sizeof(ss_id)-1] = '\0';
        } else if (strcmp(argv[i], "--advertise-ip") == 0 && i + 1 < argc) {
            strncpy(advertise_ip, argv[++i], sizeof(advertise_ip)-1);
            advertise_ip[sizeof(advertise_ip)-1] = '\0';
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_ss_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_ss_usage(argv[0]);
            return 1;
        }
    }

    if (admin_port == 0) {
        admin_port = client_port + 100;
    }

    net_set_verbose(verbose);
    mkpath(data_root);
    mkpath(undo_root);
    mkpath(checkpoint_root);
    
    // Initialize logging
    log_init("logs/ss.log");
    log_write("SS", "STARTUP", "SYSTEM", "Storage Server started", 0);
    
    // Initialize locks table mutex for thread-safe concurrent access
    pthread_mutex_init(&locks_table_mutex, NULL);
    
    int cfd = net_listen_addr(bind_host, client_port);
    int afd = net_listen_addr(bind_host, admin_port);
    if (cfd<0||afd<0){ fprintf(stderr, "SS failed to listen\n"); return 1; }
    printf("Storage Server registering to NM at %s:%u and listening for clients on %s:%u (admin %s:%u)\n",
           nm_ip, nm_reg_port, bind_host, client_port, bind_host, admin_port);
    // register with NM
    int rfd = net_connect(nm_ip, nm_reg_port);
    if (rfd >= 0) {
        if (advertise_ip[0] == '\0') {
            char local_ip[64] = "";
            if (net_get_local_addr(rfd, local_ip, sizeof(local_ip), NULL) == 0 && local_ip[0] != '\0') {
                strncpy(advertise_ip, local_ip, sizeof(advertise_ip)-1);
                advertise_ip[sizeof(advertise_ip)-1] = '\0';
            }
        }
        if (advertise_ip[0] == '\0') {
            strncpy(advertise_ip, bind_host, sizeof(advertise_ip)-1);
            advertise_ip[sizeof(advertise_ip)-1] = '\0';
        }
        char reg[256]; snprintf(reg, sizeof(reg), "REGISTER %s %u %u %s", ss_id, client_port, admin_port, advertise_ip);
        net_send_line(rfd, reg);
        char resp[256]; net_recv_line(rfd, resp, sizeof(resp));
        net_close(rfd);
        printf("SS registered: %s\n", resp);
    } else {
        printf("SS WARN: could not register to NM\n");
    }
    printf("SS listening client=%u admin=%u\n", client_port, admin_port);
    
    // Start heartbeat thread to send periodic heartbeats to NM
    pthread_t heartbeat_thread;
    HeartbeatArgs *hb_args = malloc(sizeof(HeartbeatArgs));
    strncpy(hb_args->nm_ip, nm_ip, sizeof(hb_args->nm_ip)-1);
    hb_args->nm_ip[sizeof(hb_args->nm_ip)-1] = '\0';
    hb_args->nm_reg_port = nm_reg_port;
    hb_args->client_port = client_port;
    hb_args->admin_port = admin_port;
    strncpy(hb_args->ss_id, ss_id, sizeof(hb_args->ss_id)-1);
    hb_args->ss_id[sizeof(hb_args->ss_id)-1] = '\0';
    strncpy(hb_args->advertise_ip, advertise_ip, sizeof(hb_args->advertise_ip)-1);
    hb_args->advertise_ip[sizeof(hb_args->advertise_ip)-1] = '\0';
    pthread_create(&heartbeat_thread, NULL, (void*(*)(void*))send_heartbeat, hb_args);
    pthread_detach(heartbeat_thread);
    
    while (1) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = (cfd>afd?cfd:afd);
        FD_SET(cfd, &rfds); FD_SET(afd, &rfds);
        struct timeval tv; tv.tv_sec=1; tv.tv_usec=0;
        int rc = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (rc > 0) {
            if (FD_ISSET(cfd, &rfds)) { 
                char ip[64]; uint16_t p; int cl = net_accept(cfd, ip, sizeof(ip), &p); 
                if (cl>=0) {
                    // Handle each client in a separate thread for true concurrency
                    // This allows multiple clients to edit different sentences simultaneously
                    // All threads share the same lock table (file_locks array)
                    pthread_t thread;
                    int *client_fd = (int*)malloc(sizeof(int));
                    *client_fd = cl;
                    if (pthread_create(&thread, NULL, handle_client_conn, client_fd) != 0) {
                        // Thread creation failed, handle in main thread
                        int *cfd_ptr = (int*)malloc(sizeof(int));
                        *cfd_ptr = cl;
                        handle_client_conn(cfd_ptr);
                    } else {
                        pthread_detach(thread);  // Don't wait for thread to finish
                    }
                }
            }
            if (FD_ISSET(afd, &rfds)) { 
                char ip[64]; uint16_t p; int al = net_accept(afd, ip, sizeof(ip), &p); 
                if (al>=0) handle_admin_conn(al); 
            }
        }
    }
}


