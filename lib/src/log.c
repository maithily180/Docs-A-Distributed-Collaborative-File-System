#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../include/log.h"
#include "../include/util.h"

static FILE *log_fp = NULL;

void log_init(const char *log_file) {
    if (log_file) {
        // Create logs directory if it doesn't exist
        mkpath("logs");
        log_fp = fopen(log_file, "a");
        if (!log_fp) {
            fprintf(stderr, "WARNING: Could not open log file %s\n", log_file);
        }
    }
}

void log_write(const char *component, const char *op, const char *user, const char *details, int result) {
    // Safety checks
    if (!component || !op) {
        return;
    }
    
    // Get timestamp in IST (UTC + 5:30 = UTC + 19800 seconds)
    time_t now = time(NULL);
    time_t ist_time = now + (5 * 3600 + 30 * 60);  // Add 5.5 hours for IST
    struct tm *tm_info = gmtime(&ist_time);  // Use gmtime on the adjusted time
    char timestamp[64] = "[NO-TIME]";
    
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }
    
    // Always print to console for NM operations
    if (strcmp(component, "NM") == 0) {
        printf("[%s] %s: %s user=%s %s\n", 
               timestamp, component, op, 
               user ? user : "anonymous", 
               details ? details : "");
        fflush(stdout);
    }
    
    // Write to log file if available
    if (log_fp) {
        fprintf(log_fp, "[%s] %s: %s user=%s details=%s result=%d\n", 
                timestamp, component, op, 
                user ? user : "anonymous", 
                details ? details : "", 
                result);
        fflush(log_fp);
    }
}

void log_close(void) {
    if (log_fp) { fclose(log_fp); log_fp = NULL; }
}

