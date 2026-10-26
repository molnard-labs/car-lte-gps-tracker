# Makefile for host-side unit tests
# Usage:
#   make          — build and run all tests
#   make test     — same
#   make clean    — remove test binaries

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -std=c11
LIBS    = -lm

TESTS   = test_tracker_core test_tracker_store test_tracker_policy test_setup_policy test_setup_listener test_setup_wifi_config test_net_policy test_at_response
TRACKER_CORE = ../components/tracker_core
TRACKER_STORE = ../components/tracker_store
TRACKER_SETUP = ../components/tracker_setup
TRACKER_INCLUDES = -I$(TRACKER_CORE)/include -I$(TRACKER_STORE)/include

ifeq ($(OS),Windows_NT)
EXE_SUFFIX = .exe
TEST_BINS = $(addsuffix $(EXE_SUFFIX),$(TESTS))
RUN_ALL = powershell -NoProfile -Command "$$pass=0; $$fail=0; Write-Host ''; Write-Host '========================================'; Write-Host 'Running all unit tests...'; Write-Host '========================================'; foreach ($$t in '$(TEST_BINS)'.Split(' ')) { $$exe = '.\' + $$t; & $$exe; if ($$LASTEXITCODE -eq 0) { $$pass++ } else { $$fail++ } }; Write-Host '========================================'; Write-Host ('Test suites: ' + ($$pass + $$fail) + ' total, ' + $$pass + ' passed, ' + $$fail + ' failed'); Write-Host '========================================'; if ($$fail -ne 0) { exit 1 }"
RM_CMD = powershell -NoProfile -Command "$$paths = '$(TESTS) $(TEST_BINS)'.Split(' '); foreach ($$p in $$paths) { if ($$p -and (Test-Path -LiteralPath $$p)) { Remove-Item -LiteralPath $$p -Force } }"
else
EXE_SUFFIX =
TEST_BINS = $(TESTS)
RUN_ALL = PASS=0; FAIL=0; \
	for t in $(TESTS); do \
	    ./$$t; \
	    if [ $$? -eq 0 ]; then PASS=$$((PASS+1)); else FAIL=$$((FAIL+1)); fi; \
	done; \
	echo "========================================";\
	echo "Test suites: $$((PASS+FAIL)) total, $$PASS passed, $$FAIL failed"; \
	echo "========================================"; \
	[ $$FAIL -eq 0 ]
RM_CMD = rm -f $(TESTS)
endif

.PHONY: all test clean

all: test

test: $(TEST_BINS)
	@$(RUN_ALL)

test_tracker_core$(EXE_SUFFIX): test_tracker_core.c $(TRACKER_CORE)/tracker_core.c $(TRACKER_CORE)/include/tracker_core.h $(TRACKER_CORE)/include/tracker_types.h
	$(CC) $(CFLAGS) $(TRACKER_INCLUDES) -o $@ test_tracker_core.c $(TRACKER_CORE)/tracker_core.c $(LIBS)

test_tracker_store$(EXE_SUFFIX): test_tracker_store.c $(TRACKER_STORE)/tracker_store.c $(TRACKER_CORE)/tracker_core.c $(TRACKER_STORE)/include/tracker_store.h $(TRACKER_CORE)/include/tracker_types.h
	$(CC) $(CFLAGS) $(TRACKER_INCLUDES) -o $@ test_tracker_store.c $(TRACKER_STORE)/tracker_store.c $(TRACKER_CORE)/tracker_core.c $(LIBS)

test_tracker_policy$(EXE_SUFFIX): test_tracker_policy.c ../main/tracker_policy.c ../main/tracker_policy.h $(TRACKER_CORE)/include/tracker_types.h
	$(CC) $(CFLAGS) $(TRACKER_INCLUDES) -I../main -o $@ test_tracker_policy.c ../main/tracker_policy.c

test_setup_policy$(EXE_SUFFIX): $(TRACKER_SETUP)/test/test_setup_policy.c $(TRACKER_SETUP)/setup_policy.c $(TRACKER_SETUP)/setup_policy.h
	$(CC) $(CFLAGS) -I$(TRACKER_SETUP) -o $@ $(TRACKER_SETUP)/test/test_setup_policy.c $(TRACKER_SETUP)/setup_policy.c

test_setup_listener$(EXE_SUFFIX): $(TRACKER_SETUP)/test/test_setup_listener.c $(TRACKER_SETUP)/setup_listener.c $(TRACKER_SETUP)/setup_listener.h $(TRACKER_SETUP)/test/setup_listener_test_platform.h
	$(CC) $(CFLAGS) -I$(TRACKER_SETUP) -o $@ $(TRACKER_SETUP)/test/test_setup_listener.c $(TRACKER_SETUP)/setup_listener.c

test_setup_wifi_config$(EXE_SUFFIX): $(TRACKER_SETUP)/test/test_setup_wifi_config.c $(TRACKER_SETUP)/setup_wifi_config.h $(TRACKER_CORE)/tracker_core.c $(TRACKER_CORE)/include/tracker_types.h
	$(CC) $(CFLAGS) $(TRACKER_INCLUDES) -I$(TRACKER_SETUP) -o $@ $(TRACKER_SETUP)/test/test_setup_wifi_config.c $(TRACKER_CORE)/tracker_core.c $(LIBS)

test_net_policy$(EXE_SUFFIX): ../components/tracker_net/test/test_net_policy.c ../components/tracker_net/net_policy.h
	$(CC) $(CFLAGS) -I../components/tracker_net -o $@ ../components/tracker_net/test/test_net_policy.c

test_at_response$(EXE_SUFFIX): ../components/tracker_net/test/test_at_response.c ../components/tracker_net/at_response.h $(TRACKER_CORE)/tracker_core.c $(TRACKER_CORE)/include/tracker_types.h
	$(CC) $(CFLAGS) $(TRACKER_INCLUDES) -I../components/tracker_net -o $@ ../components/tracker_net/test/test_at_response.c $(TRACKER_CORE)/tracker_core.c $(LIBS)

clean:
	-@$(RM_CMD)
