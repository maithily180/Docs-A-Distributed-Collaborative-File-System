#include "../../lib/include/error_codes.h"

const char* errcode_to_string(int code) {
    switch (code) {
        case ERR_SUCCESS: return "OK";
        case ERR_FILE_NOT_FOUND: return "ERR not found";
        case ERR_NO_ACCESS: return "ERR no access";
        case ERR_NO_WRITE_ACCESS: return "ERR no write access";
        case ERR_FILE_EXISTS: return "ERR file exists";
        case ERR_INVALID_ARGS: return "ERR bad args";
        case ERR_SENTENCE_LOCKED: return "ERR sentence locked";
        case ERR_SENTENCE_OUT_OF_RANGE: return "ERR sentence index out of range";
        case ERR_WORD_OUT_OF_RANGE: return "ERR word index out of range";
        case ERR_SS_NOT_AVAILABLE: return "ERR no storage server available";
        case ERR_SS_NOT_REACHABLE: return "ERR SS not reachable";
        case ERR_SS_NO_RESPONSE: return "ERR SS no response";
        case ERR_NOT_LOGGED_IN: return "ERR please LOGIN first";
        case ERR_ONLY_OWNER: return "ERR only owner can perform this operation";
        case ERR_UNKNOWN_COMMAND: return "ERR unknown command";
        case ERR_SYSTEM_ERROR: return "ERR system error";
        default: return "ERR unknown error";
    }
}

