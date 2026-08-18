CXX ?= g++
NAME ?= mail
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2
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

all: $(BUILD)/$(NAME)d $(BUILD)/$(NAME)c

$(BUILD)/$(NAME)d: $(BUILD)/src/maild/main.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/$(NAME)c: $(BUILD)/src/cli/cli.o $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

corpus: $(BUILD)/corpus_check
	$(BUILD)/corpus_check

$(BUILD)/corpus_check: $(BUILD)/src/sources/corpus_check.o $(BUILD)/src/sources/classify.o
	$(CXX) $(CXXFLAGS) -o $@ $^

check: all
	$(BUILD)/$(NAME)d --selfcheck
	$(BUILD)/$(NAME)c --selfcheck

install: all
	install -Dm755 $(BUILD)/$(NAME)d $(BUILD)/$(NAME)c -t $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME)d $(DESTDIR)$(PREFIX)/bin/$(NAME)c

clean:
	rm -rf $(BUILD)

-include $(COMMON_OBJ:.o=.d) $(BUILD)/src/maild/main.d $(BUILD)/src/cli/cli.d

.PHONY: all corpus check install uninstall clean
