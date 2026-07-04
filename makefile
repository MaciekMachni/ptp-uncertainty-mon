CC      := gcc
CFLAGS  := -Wall -Wextra -g -O2 -fPIC
BIN_DIR := bin

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDFLAGS := -lrt
SHLIB_FLAGS := -shared -Wl,-soname,libptp_unc.so
else
LDFLAGS :=
SHLIB_FLAGS := -dynamiclib -install_name @rpath/libptp_unc.dylib
endif

.PHONY: all clean help

all: $(BIN_DIR)/ptp_unc_dmn $(BIN_DIR)/libptp_unc.so $(BIN_DIR)/watch_uncertainty

help:
	@echo "Targets: all, clean"

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/ptp_unc_dmn: dmn/ptp_unc_dmn.c dmn/ptp4l_client.c dmn/ptp4l_client.h dmn/ptp_unc_ipc.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -I dmn/ -o $@ dmn/ptp_unc_dmn.c dmn/ptp4l_client.c $(LDFLAGS)

$(BIN_DIR)/libptp_unc.so: lib/ptp_unc_lib.c lib/ptp_unc_api.h dmn/ptp_unc_ipc.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SHLIB_FLAGS) -I dmn/ -I lib/ -o $@ lib/ptp_unc_lib.c $(LDFLAGS)

$(BIN_DIR)/watch_uncertainty: app/watch_uncertainty.c $(BIN_DIR)/libptp_unc.so | $(BIN_DIR)
	$(CC) $(CFLAGS) -I lib/ -I dmn/ -o $@ app/watch_uncertainty.c \
		-L$(BIN_DIR) -lptp_unc -Wl,-rpath,$(CURDIR)/$(BIN_DIR) $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
