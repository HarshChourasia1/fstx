# fstx coretest: the engine in src/ (no R) plus coretest.cpp, built into one program (GNU make).
#   make [CXX=..] [CC=..] [BUILD=build/<name>] [FLAGS="-O2 -Wall -pedantic"] [LDFLAGS=-static]
#   $(BUILD)/coretest <scratch folder> [rows]
# The object list and the preprocessor flags come from src/Makevars.in (R glue files left out; r_arrow.cpp is kept with FSTX_ARROW_NO_R, as CMake does), the version from
# DESCRIPTION, as configure does. Not in the package tarball.
SRC := ../../src
BUILD ?= build/native
FLAGS ?= -O2 -Wall -pedantic
EXE := $(if $(filter Windows_NT,$(OS)),.exe,)
VERSION := $(shell sed -n 's/^Version:[[:space:]]*//p' ../../DESCRIPTION | tr -d '\r')
OBJS := r_arrow.o $(filter-out r_%.o cpp11.o,$(shell awk '/^OBJECTS/{f=1} f{print; if ($$0 !~ /\\$$/) exit}' $(SRC)/Makevars.in | tr ' \\\r' '\n\n\n' | grep '\.o$$'))
PKGFLAGS := $(shell awk '/^PKG_CPPFLAGS/{f=1} f{l=$$0; sub(/\\$$/, "", l); printf "%s ", l; if ($$0 !~ /\\$$/) exit}' $(SRC)/Makevars.in | sed 's/PKG_CPPFLAGS *=//' | tr -d '\r')
CPPFLAGS := $(subst -I,-I$(SRC)/,$(PKGFLAGS)) -DNDEBUG -DFSTX_VERSION=$(VERSION) -DFSTX_ARROW_NO_R
CXXFLAGS := -std=gnu++17 $(FLAGS) -ffp-contract=off
CFLAGS := -std=gnu11 $(FLAGS) -ffp-contract=off

$(BUILD)/coretest$(EXE): $(addprefix $(BUILD)/,$(OBJS)) $(BUILD)/coretest.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -pthread
$(BUILD)/coretest.o: coretest.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(SRC)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: clean
