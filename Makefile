# ============================================================================
#  Makefile — GUI 程序 (test_gui.exe) 独立构建脚本
#  Standalone GUI executable build script
#
#  编译器: GCC (MinGW-w64)  |  标准: C99  |  平台: Windows Win32 API
# ============================================================================

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -finput-charset=UTF-8
LDFLAGS = -mwindows
LIBS = -lcomctl32 -lcomdlg32 -lgdi32 -lmsimg32

SRC_DIR = src
OUTPUT = test_gui.exe

# 所有源文件 (GUI 模块 + 核心模块)
# All sources (GUI modules + core modules)
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:.c=.o)
RES  = $(SRC_DIR)/app.res

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): $(OBJS) $(RES)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo ""
	@echo "=== Build OK: $(OUTPUT) ==="

# 批量编译规则: src/%.c → src/%.o
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/%.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 无对应 .h 的源文件 (app.rc 编译)
$(SRC_DIR)/app.res: $(SRC_DIR)/app.rc $(SRC_DIR)/app.manifest
	windres -O coff -o $@ $<

clean:
	rm -f $(OBJS) $(RES) $(OUTPUT)
