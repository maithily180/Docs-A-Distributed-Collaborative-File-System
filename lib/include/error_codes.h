#ifndef ERROR_CODES_H
#define ERROR_CODES_H

// Universal error codes for the system
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_NO_ACCESS 2
#define ERR_NO_WRITE_ACCESS 3
#define ERR_FILE_EXISTS 4
#define ERR_INVALID_ARGS 5
#define ERR_SENTENCE_LOCKED 6
#define ERR_SENTENCE_OUT_OF_RANGE 7
#define ERR_WORD_OUT_OF_RANGE 8
#define ERR_SS_NOT_AVAILABLE 9
#define ERR_SS_NOT_REACHABLE 10
#define ERR_SS_NO_RESPONSE 11
#define ERR_NOT_LOGGED_IN 12
#define ERR_ONLY_OWNER 13
#define ERR_UNKNOWN_COMMAND 14
#define ERR_SYSTEM_ERROR 15

// Error code to string
const char* errcode_to_string(int code);

#endif

