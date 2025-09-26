# Makefile for the Cortez Terminal Project

# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 -std=c99
LDFLAGS =

# SDL flags for the frontend
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs) -lSDL2_ttf

# Source files
FRONTEND_SRC = cortez_frontend.c
BACKEND_SRC = cortez_backend.c
NETRUNNER_SRC = net_runner.c

# Executables
FRONTEND_EXE = cortez_frontend
BACKEND_EXE = cortez_backend
NETRUNNER_EXE = net-runner

# Tools directory
TOOLS_DIR = ../tools

# Default target
all: $(FRONTEND_EXE) $(BACKEND_EXE) net-runner-tool

# Target to build and install the net-runner tool
net-runner-tool: $(NETRUNNER_EXE)
	@mkdir -p $(TOOLS_DIR)
	@mv $(NETRUNNER_EXE) $(TOOLS_DIR)/
	@echo "net-runner installed in $(TOOLS_DIR)/"

# Rule to build the frontend
$(FRONTEND_EXE): $(FRONTEND_SRC)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(SDL_LIBS)
	@echo "Frontend built successfully."

# Rule to build the backend
$(BACKEND_EXE): $(BACKEND_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -lutil -lpthread
	@echo "Backend built successfully."

# Rule to build the net-runner tool
$(NETRUNNER_EXE): $(NETRUNNER_SRC)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "net-runner built successfully."

# Clean up build files
clean:
	rm -f $(FRONTEND_EXE) $(BACKEND_EXE)
	rm -f $(TOOLS_DIR)/$(NETRUNNER_EXE)
	@echo "Cleanup complete."

.PHONY: all clean net-runner-tool
