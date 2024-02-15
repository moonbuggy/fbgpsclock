INCDIRS := fonts include

SRCEXTS := c
HDREXTS := h

INCLUDES := $(foreach dir, $(INCDIRS), $(foreach ext, $(HDREXTS), $(wildcard $(dir)/*.$(ext))))

CC = gcc
CFLAGS = -Wall -pedantic -march=native -O3
LDFLAGS = -O3
# CFLAGS = -Wall -pedantic -march=native -O0 -g
# LDFLAGS = -O0 -g
LIBS = -lgps -lm -lpthread
SOURCE = fbgpsclock.c
EXTRASOURCES = include/ini.c include/log.c
SOURCES = ${SOURCE} ${EXTRASOURCES}
OBJECTS = $(SOURCES:.c=.o)
EXE = fbgpsclock
CPPCHECK := cppcheck
INCFLAGS := $(INCDIRS:%=-I%)
override CPPCHECKFLAGS += --enable=all --suppress=missingIncludeSystem

$(EXE): $(OBJECTS)
	$(CC) $^ $(LDFLAGS) $(LIBS) $(INCFLAGS) -o $(EXE)
.c.o:
	$(CC) $(CFLAGS) $(LIBS) $(INCFLAGS) -c $< -o $@

.PHONY : clean
clean:
	rm -f *.o $(EXE)

check:
	$(CPPCHECK) $(CPPCHECKFLAGS) $(SOURCE) $(INCFLAGS)

install: $(EXE)
	/usr/bin/mkdir -p '/usr/local/bin'
	/usr/bin/install -c fbgpsclock '/usr/local/bin'
	/usr/bin/mkdir -p '/usr/local/etc'
	/usr/bin/install -c -m 644 fbgpsclock.ini '/usr/local/etc'
	/usr/bin/mkdir -p '/lib/systemd/system'
	/usr/bin/install -c -m 644 fbgpsclock.service '/lib/systemd/system'

uninstall:
	rm -rfv '/usr/local/bin/fbgpsclock'
	rm -rfv '/usr/local/etc/fbgpsclock.ini'
	rm -rfv '/lib/systemd/system/fbgpsclock.service'

# Print commands
help:
	@echo "Targets:"
	@echo " make             - build fbgpsclock"
	@echo " make check       - run cppcheck on fbgpsclock.c"
	@echo " make install     - install fbgpsclock (for systemd only)"
	@echo " make uninstall   - unstall fbgpsclock"
	@echo " make clean       - Remove all build output"
	@echo " make help        - this help"
	@echo ""
