ifndef SDK_DIR
    $(error "Run python3 scripts/prepare_vendor.py from the repository root, then execute its printed make command")
endif

PROJECT_MAKE_FILE := $(SDK_DIR)/tools/build_tools/build_files/project_main.mk
ifeq ($(wildcard $(PROJECT_MAKE_FILE)),)
    $(error "$(PROJECT_MAKE_FILE) not found; set SDK_DIR to the bk_avdk_smp checkout")
endif

# aec_v3_algorithm_v2.c keeps `len` and `interleave_bytes` for its ADK
# debug-dump path (CONFIG_ADK_DEBUG_DUMP_UTIL). With the dump off they are set
# but never read, and the SDK's project-wide -Werror turns that into a build
# failure. The SDK documents EXTRA_CFLAGS as the project-level hook for adding
# compiler options, so the suppression lives here rather than as an edit to the
# pinned SDK tree.
export EXTRA_CFLAGS += -Wno-unused-but-set-variable

include $(PROJECT_MAKE_FILE)
