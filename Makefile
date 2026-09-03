CXX ?= g++
NAME ?= mail
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wswitch-enum -Wnull-dereference
GCCONLY = -Wlogical-op -Wduplicated-cond -Wduplicated-branches  # clang has none of these
CXXFLAGS += $(shell $(CXX) --version 2>/dev/null | grep -qi clang || echo $(GCCONLY))
VERSION ?= 0.1
CPPFLAGS += -DAPP_NAME='"$(NAME)"' -DVERSION='"$(VERSION)"' -Isrc/core -Isrc/sources -Isrc/view
DEV ?= 1
ifeq ($(DEV),1)
CXXFLAGS += -Werror
endif
PKGS = libcurl sqlite3 simdjson  # homebrew/BSD keep these outside the default search paths
LIBS := $(shell pkg-config --libs $(PKGS) 2>/dev/null || echo -lcurl -lsqlite3 -lsimdjson)
CPPFLAGS += $(shell pkg-config --cflags $(PKGS) 2>/dev/null)

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)  # command-line -jN still wins (make takes the last)

PREFIX ?= $(HOME)/.local
BUILD = build
COMMON = $(filter-out src/sources/corpus_check.cpp,$(wildcard src/core/*.cpp src/sources/*.cpp src/view/*.cpp))  # corpus_check has its own main
COMMON_OBJ = $(COMMON:%.cpp=$(BUILD)/%.o)

all: $(BUILD)/$(NAME)d $(BUILD)/$(NAME)c $(BUILD)/$(NAME)t

$(BUILD)/$(NAME)d: $(BUILD)/src/maild/main.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/$(NAME)c: $(BUILD)/src/cli/cli.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/$(NAME)t: $(BUILD)/src/tui/tui.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/$(NAME)-check: $(BUILD)/src/tests/main.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

corpus: $(BUILD)/corpus_check
	$(BUILD)/corpus_check

$(BUILD)/corpus_check: $(BUILD)/src/sources/corpus_check.o $(BUILD)/src/sources/classify.o
	$(CXX) $(CXXFLAGS) -o $@ $^

check: $(BUILD)/$(NAME)-check corpus
	$(BUILD)/$(NAME)-check

check-asan:  # system gcc lacks the sanitize USE flag
	$(MAKE) clean
	$(MAKE) check CXX=clang++ CXXFLAGS="-O1 -g -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS"
	$(MAKE) clean

install: all  # not install -D/-t: bsd and macos install have neither
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m755 $(BUILD)/$(NAME)d $(BUILD)/$(NAME)c $(BUILD)/$(NAME)t $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME)d $(DESTDIR)$(PREFIX)/bin/$(NAME)c $(DESTDIR)$(PREFIX)/bin/$(NAME)t

clean:
	rm -rf $(BUILD)

-include $(COMMON_OBJ:.o=.d) $(BUILD)/src/maild/main.d $(BUILD)/src/cli/cli.d $(BUILD)/src/tui/tui.d $(BUILD)/src/tests/main.d

.PHONY: all corpus check check-asan install uninstall clean
