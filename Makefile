.PHONY: clean
.DEFAULT: all

# Get common build settings
include include/Makefile.config

all: info ext engine doc sim

info:
	@echo "Build using $(CXX)"

ext:
	@$(SCRIPTS_DIR)/fetch-externals.sh

doc:
#	@doxygen

engine: base
	@cd $(SRC_ROOT_DIR)/engine && make all

base:
	@cd $(SRC_ROOT_DIR)/base && make all

sim: base misc
	@cd $(SRC_ROOT_DIR)/sim && make all

misc:
	@cd $(SRC_ROOT_DIR)/misc && make all

nonext: engine doc

test:
	@mkdir -p $(BUILD_DIR)
	@$(CC) test.cc -o $(BUILD_DIR)/test $(LIBS)
	env HEAPCHECK=normal $(BUILD_DIR)/test

clean:
	@rm -rf build
	@rm -rf src/generated/*
