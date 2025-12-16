#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "../../lib/include/net.h"
#include "../../lib/include/util.h"

static void print_client_usage(const char *prog) {
    printf("Usage: %s --nm-ip IP --nm-port PORT [--username NAME] [--client-port PORT] [--bind-ip IP] [--verbose]\n", prog);
    printf("Defaults: nm-ip=127.0.0.1, nm-port=8000\n");
}

int main(int argc, char **argv) {
    char nm_ip[64] = "127.0.0.1";
    uint16_t nm_port = 8000;
    char username[128] = "";
    char bind_ip[64] = "";
    uint16_t client_port = 0;
    int verbose = 0;

    char cfg_host[64];
    if (config_get_string("nm.host", cfg_host, sizeof(cfg_host))) {
        strncpy(nm_ip, cfg_host, sizeof(nm_ip)-1);
        nm_ip[sizeof(nm_ip)-1] = '\0';
    }
    uint16_t cfg_port;
    if (config_get_uint16("nm.port", &cfg_port) && cfg_port != 0) {
        nm_port = cfg_port;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nm-ip") == 0 && i + 1 < argc) {
            strncpy(nm_ip, argv[++i], sizeof(nm_ip)-1);
            nm_ip[sizeof(nm_ip)-1] = '\0';
        } else if (strcmp(argv[i], "--nm-port") == 0 && i + 1 < argc) {
            nm_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--username") == 0 && i + 1 < argc) {
            strncpy(username, argv[++i], sizeof(username)-1);
            username[sizeof(username)-1] = '\0';
        } else if (strcmp(argv[i], "--client-port") == 0 && i + 1 < argc) {
            client_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bind-ip") == 0 && i + 1 < argc) {
            strncpy(bind_ip, argv[++i], sizeof(bind_ip)-1);
            bind_ip[sizeof(bind_ip)-1] = '\0';
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_client_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_client_usage(argv[0]);
            return 1;
        }
    }

    net_set_verbose(verbose);
    if (username[0]) {
        printf("Client will connect to NM at %s:%u as '%s'\n", nm_ip, nm_port, username);
    } else {
        printf("Client will connect to NM at %s:%u (username prompt pending)\n", nm_ip, nm_port);
    }
    const char *bind_ptr = (bind_ip[0] != '\0') ? bind_ip : NULL;
    if (bind_ptr || client_port != 0) {
        printf("Client local bind preference %s:%u\n",
               bind_ptr ? bind_ptr : "0.0.0.0",
               client_port ? client_port : 0);
    }
    int fd = net_connect_ex(nm_ip, nm_port, bind_ptr, client_port);
    if (fd < 0) { fprintf(stderr, "Cannot connect to NM\n"); return 1; }
    char local_ip[64] = "";
    uint16_t local_port = 0;
    if (net_get_local_addr(fd, local_ip, sizeof(local_ip), &local_port) == 0 && local_port != 0) {
        printf("Client local endpoint %s:%u\n", local_ip[0] ? local_ip : "0.0.0.0", local_port);
    }
    char buf[1024];
    if (net_recv_line(fd, buf, sizeof(buf)) > 0) printf("%s\n", buf);
    // Prompt username
    if (username[0] == '\0') {
        printf("username> "); fflush(stdout);
        if (!fgets(username, sizeof(username), stdin)) return 0;
        username[strcspn(username, "\r\n")] = 0;
    }
    char login[256]; snprintf(login, sizeof(login), "LOGIN %s %u", username, (unsigned)(local_port ? local_port : client_port));
    net_send_line(fd, login);
    if (net_recv_line(fd, buf, sizeof(buf)) > 0) printf("%s\n", buf);
    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\r\n")] = 0;
        if (strcmp(buf, "") == 0) continue;
        net_send_line(fd, buf);
        if (strncmp(buf, "READ ", 5)==0 || strncmp(buf, "STREAM ", 7)==0) {
    char resp[256]; 
    if (net_recv_line(fd, resp, sizeof(resp)) <= 0) { 
        printf("<no response>\n"); 
        continue; 
    }
    
    // Check if error from NM
    if (strncmp(resp, "ERR", 3)==0) {
        printf("%s\n", resp);
        continue;
    }
    
    if (strncmp(resp, "SS ", 3)==0) {
        char ip[64]; 
        unsigned port; 
        sscanf(resp+3, "%63s %u", ip, &port);
        
        int sfd = net_connect(ip, (uint16_t)port);
        if (sfd<0) { 
            printf("ERR: cannot connect to SS\n"); 
            continue; 
        }
        
        // Receive welcome message from SS
        char welcome[256]; 
        net_recv_line(sfd, welcome, sizeof(welcome));
        
        // Send READ or STREAM command to SS
        net_send_line(sfd, buf);
        
        // Receive response from SS
        char ss_resp[1024];
        if (net_recv_line(sfd, ss_resp, sizeof(ss_resp)) <= 0) {
            printf("ERR: no response from SS\n");
            net_close(sfd);
            continue;
        }
        
        // Check SS response
        if (strncmp(ss_resp, "OK", 2) == 0) {
            // For READ: receive all content until END or empty line
            if (strncmp(buf, "READ ", 5) == 0) {
                while (1) {
                    char content[4096];
                    int n = net_recv_line(sfd, content, sizeof(content));
                    if (n <= 0) break;  // Connection closed
                    if (strcmp(content, "END") == 0) break;  // End marker
                    printf("%s\n", content);
                }
            }
            // For STREAM: receive word-by-word until STOP
            else {
                while (1) {
                    char word[1024];
                    int n = net_recv_line(sfd, word, sizeof(word));
                    if (n <= 0) break;
                    if (strcmp(word, "STOP") == 0 || strcmp(word, "END") == 0) break;
                    printf("%s ", word);
                    fflush(stdout);
                }
                printf("\n");
            }
        } else {
            // Error from SS
            printf("%s\n", ss_resp);
        }
        
        // Close SS connection
        net_send_line(sfd, "QUIT");
        net_recv_line(sfd, welcome, sizeof(welcome));
        net_close(sfd);
    } else {
        // Unexpected response from NM
        printf("%s\n", resp);
    }

        } else if (strncmp(buf, "WRITE ", 6)==0) {
            // Step 1: ask NM for SS address
            char nmresp[256]; if (net_recv_line(fd, nmresp, sizeof(nmresp)) <= 0) { printf("<no response>\n"); continue; }
            // Check for error response from NM
            if (strncmp(nmresp, "ERR", 3)==0) { printf("%s\n", nmresp); continue; }
            // printf("%s\n", nmresp);
            if (strncmp(nmresp, "SS ", 3)!=0) continue;
            char ip[64]; unsigned port; sscanf(nmresp+3, "%63s %u", ip, &port);
            int sfd = net_connect(ip, (uint16_t)port);
            if (sfd<0){ printf("ERR connect SS\n"); continue; }
            char welcome[256]; if (net_recv_line(sfd, welcome, sizeof(welcome))>0) {}
            // parse filename and sentence index to send WRITE_BEGIN
            char fname[256]; int sidx=-1; if (sscanf(buf+6, "%255s %d", fname, &sidx) < 2) { printf("ERR bad args\n"); net_close(sfd); continue; }
            char cmd[512]; snprintf(cmd, sizeof(cmd), "WRITE_BEGIN %s %d", fname, sidx);
            net_send_line(sfd, cmd);
            char sresp[256]; if (net_recv_line(sfd, sresp, sizeof(sresp))<=0) { printf("ERR no response\n"); net_close(sfd); continue; }
            if (strncmp(sresp, "OK", 2)!=0) { printf("%s\n", sresp); net_close(sfd); continue; }
            // Parse lock response: "OK lock <filename> <sentence_index>"
            char lock_fname[256]; int lock_sidx=-1;
            if (sscanf(sresp, "OK lock %255s %d", lock_fname, &lock_sidx) >= 2) {
                // Use the confirmed filename and sentence index
                strncpy(fname, lock_fname, sizeof(fname)-1);
                sidx = lock_sidx;
            }
            printf("Enter '<word_index> <content>' lines, end with 'ETIRW'\n");
            while (1) {
                char ln[1024]; if (!fgets(ln, sizeof(ln), stdin)) break; 
                ln[strcspn(ln, "\r\n")] = 0; 
                // Check for ETIRW (case-insensitive)
                int is_etirw = 1;
                if (strlen(ln) == 5) {
                    for (int i=0; i<5; i++) {
                        if (toupper((unsigned char)ln[i]) != "ETIRW"[i]) { is_etirw = 0; break; }
                    }
                } else is_etirw = 0;
                if (is_etirw) break; 
                if (strlen(ln)==0) continue;
                // Check if it's a command that should break out of WRITE mode
                if (strncmp(ln, "VIEWCHECKPOINT", 14)==0 || strncmp(ln, "READ", 4)==0 || strncmp(ln, "CREATE", 6)==0) {
                    // Break out and send the command to NM instead
                    char end_cmd[512]; snprintf(end_cmd, sizeof(end_cmd), "WRITE_END %s %d", fname, sidx);
                    net_send_line(sfd, end_cmd);
                    net_recv_line(sfd, sresp, sizeof(sresp));
                    net_send_line(sfd, "QUIT");
                    net_recv_line(sfd, welcome, sizeof(welcome));
                    net_close(sfd);
                    // Now send to NM
                    net_send_line(fd, ln);
                    goto handle_normal;
                }
                int widx; char content[900];
                if (sscanf(ln, "%d %899[\x20-\x7E]", &widx, content) < 2) { printf("ERR format: <word_index> <content>\n"); continue; }
                // Include filename and sentence index in WRITE_UPDATE
                char ucmd[1024]; snprintf(ucmd, sizeof(ucmd), "WRITE_UPDATE %s %d %d %s", fname, sidx, widx, content);
                net_send_line(sfd, ucmd);
                if (net_recv_line(sfd, sresp, sizeof(sresp))>0) printf("%s\n", sresp);
            }
            char end_cmd[512]; snprintf(end_cmd, sizeof(end_cmd), "WRITE_END %s %d", fname, sidx);
            net_send_line(sfd, end_cmd); if (net_recv_line(sfd, sresp, sizeof(sresp))>0) printf("%s\n", sresp);
            net_send_line(sfd, "QUIT"); net_recv_line(sfd, welcome, sizeof(welcome)); net_close(sfd);
            continue;
        } else {
            handle_normal:
            // print multi-line until END or single line
            char resp[512];
            if (strncmp(buf, "EXEC ", 5)==0) {
                // EXEC protocol: NM sends OK then streams output lines and END
                if (net_recv_line(fd, resp, sizeof(resp)) <=0) { printf("<no response>\n"); }
                else if (strncmp(resp, "OK", 2)==0) {
                    while (1) {
                        if (net_recv_line(fd, resp, sizeof(resp)) <=0) { printf("<no response>\n"); break; }
                        if (strcmp(resp, "END")==0) break;
                        printf("%s\n", resp);
                    }
                } else {
                    printf("%s\n", resp);
                }
            } else {
                while (1) {
                    if (net_recv_line(fd, resp, sizeof(resp)) <=0) { printf("<no response>\n"); break; }
                    printf("%s\n", resp);
                    if (strcmp(resp, "END")==0 || strncmp(resp, "OK", 2)==0 || strncmp(resp, "ERR", 3)==0 || strcmp(resp, "BYE")==0) break;
                }
            }
            if (strcmp(buf, "QUIT")==0) break;
        }
    }
    net_close(fd);
    return 0;
}


