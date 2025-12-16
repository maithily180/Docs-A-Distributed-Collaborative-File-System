CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

BIN_DIR = bin
LIB_DIR = lib
NM_DIR = nm
SS_DIR = ss
CLIENT_DIR = client

INCLUDES = -I$(LIB_DIR)/include

LIB_SRC = \
  $(LIB_DIR)/src/net.c \
  $(LIB_DIR)/src/util.c \
  $(LIB_DIR)/src/log.c \
  $(LIB_DIR)/src/hashmap.c \
  $(LIB_DIR)/src/lru_cache.c \
  $(LIB_DIR)/src/persist.c \
  $(LIB_DIR)/src/error_codes.c

LIB_OBJ = $(LIB_SRC:.c=.o)

NM_SRC = $(NM_DIR)/src/main.c
SS_SRC = $(SS_DIR)/src/main.c
CLIENT_SRC = $(CLIENT_DIR)/src/main.c

all: dirs nm ss client

dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LIB_DIR)/src $(LIB_DIR)/include
	@mkdir -p $(NM_DIR)/src $(SS_DIR)/src $(CLIENT_DIR)/src
	@mkdir -p logs nm

lib: $(LIB_OBJ)

nm: lib $(NM_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/nm $(NM_SRC) $(LIB_OBJ)

ss: lib $(SS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/ss $(SS_SRC) $(LIB_OBJ)

client: lib $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/client $(CLIENT_SRC) $(LIB_OBJ)

clean:
	rm -f $(LIB_OBJ) $(BIN_DIR)/nm $(BIN_DIR)/ss $(BIN_DIR)/client

.PHONY: all dirs lib nm ss client clean

