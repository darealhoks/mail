CXX ?= g++
NAME ?= mail
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wswitch-enum -Wlogical-op \
            -Wduplicated-cond -Wduplicated-branches -Wnull-dereference
CPPFLAGS += -DAPP_NAME='"$(NAME)"' -Isrc/core -Isrc/sources -Isrc/view
DEV ?= 1
ifeq ($(DEV),1)
CXXFLAGS += -Werror
endif
LIBS = -lcurl -lsqlite3 -lsimdjson

MAKEFLAGS += -j$(shell nproc)  # command-line -jN still wins (make takes the last)

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

corpus: $(BUILD)/corpus_check
	$(BUILD)/corpus_check

$(BUILD)/corpus_check: $(BUILD)/src/sources/corpus_check.o $(BUILD)/src/sources/classify.o
	$(CXX) $(CXXFLAGS) -o $@ $^

check: all corpus
	$(BUILD)/$(NAME)d --selfcheck
	$(BUILD)/$(NAME)c --selfcheck
	$(BUILD)/$(NAME)t --selfcheck

check-asan:  # system gcc lacks the sanitize USE flag
	$(MAKE) clean
	$(MAKE) check CXX=clang++ CXXFLAGS="-O1 -g -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS"
	$(MAKE) clean

install: all
	install -Dm755 $(BUILD)/$(NAME)d $(BUILD)/$(NAME)c $(BUILD)/$(NAME)t -t $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME)d $(DESTDIR)$(PREFIX)/bin/$(NAME)c $(DESTDIR)$(PREFIX)/bin/$(NAME)t

clean:
	rm -rf $(BUILD)

-include $(COMMON_OBJ:.o=.d) $(BUILD)/src/maild/main.d $(BUILD)/src/cli/cli.d $(BUILD)/src/tui/tui.d

.PHONY: all corpus check check-asan install uninstall clean
