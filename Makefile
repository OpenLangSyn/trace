CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O2 -fPIC
INCLUDES := -Iinclude
LIBS     := -lpthread

SRC      := src/Trace.cpp
OBJ      := $(SRC:.cpp=.o)
LIB      := libtrace.a
SO       := libtrace.so
PREFIX   := /usr/local

TRACED_SRC := src/traced.cpp
TRACED_BIN := traced

TEST_SRC := tests/test_trace.cpp
TEST_BIN := test_trace

UNIT_SRC := tests/test_libtrace.cpp
UNIT_BIN := test_libtrace

.PHONY: all lib clean install install-lib install-daemon uninstall test unit-test

all: $(LIB) $(SO) $(TRACED_BIN)

lib: $(LIB) $(SO)

$(LIB): $(OBJ)
	ar rcs $@ $^

$(SO): $(OBJ)
	$(CXX) -shared -o $@ $^ $(LIBS)

src/%.o: src/%.cpp include/trace/Trace.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TRACED_BIN): $(TRACED_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L. -ltrace -lsqlite3 -lpthread

$(TEST_BIN): $(TEST_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L. -ltrace -lsqlite3 -lpthread

$(UNIT_BIN): $(UNIT_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L. -ltrace -lpthread

test: $(TEST_BIN) $(TRACED_BIN)
	./$(TEST_BIN)

unit-test: $(UNIT_BIN)
	./$(UNIT_BIN)

install-lib: $(LIB) $(SO)
	install -d $(PREFIX)/lib $(PREFIX)/include/trace
	install -m 644 $(LIB) $(PREFIX)/lib/
	install -m 755 $(SO) $(PREFIX)/lib/
	install -m 644 include/trace/Trace.h $(PREFIX)/include/trace/
	ldconfig

clean:
	rm -f $(OBJ) $(LIB) $(SO) $(TRACED_BIN) $(TEST_BIN) $(UNIT_BIN)

install-daemon: $(TRACED_BIN)
	install -m 755 $(TRACED_BIN) $(PREFIX)/bin/
	install -m 644 traced.service /etc/systemd/system/

install: $(LIB) $(SO) install-daemon
	install -d $(PREFIX)/lib $(PREFIX)/include/trace
	install -m 644 $(LIB) $(PREFIX)/lib/
	install -m 755 $(SO) $(PREFIX)/lib/
	install -m 644 include/trace/Trace.h $(PREFIX)/include/trace/
	ldconfig

uninstall:
	rm -f $(PREFIX)/bin/$(TRACED_BIN)
	rm -f $(PREFIX)/lib/$(LIB) $(PREFIX)/lib/$(SO) $(PREFIX)/include/trace/Trace.h
	rm -f /etc/systemd/system/traced.service
	rmdir --ignore-fail-on-non-empty $(PREFIX)/include/trace
