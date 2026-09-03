#!/usr/bin/env bash
# ============================================================
# efmod 动态库构建脚本
#
# 用法:
#   ./make.sh <源码目录> [选项]
#   ./make.sh -h | --help             # 显示本帮助
#
# 选项:
#   -t, --target <目标>  构建目标，多个用逗号分隔（默认 android,native,mingw）
#                        android  使用 NDK 交叉编译 Android .so
#                        native   直接编译（不使用 NDK，本机 Linux .so）
#                        mingw    MinGW 交叉编译 Windows .dll（32/64 位）
#   -a, --abi <abi>     目标 ABI，多个用逗号分隔（默认 arm64-v8a，仅 android）
#                        arm64-v8a, armeabi-v7a, x86_64, x86
#   -m, --mingw-arch <a>  MinGW 架构，多个用逗号分隔（默认 x86_64,i686，仅 mingw）
#   -n, --ndk <路径>    指定 NDK 路径（默认自动探测，仅 android）
#   -p, --platform <n>  Android API 级别（默认 27，仅 android）
#   -c, --clean         清理构建目录后重新构建
#   -s, --no-strip      构建完成后不精简动态库
#   -h, --help          显示本帮助
#
# 说明:
#   - 源码路径必须由命令行参数传入，mod 名称不限
#   - 构建目录固定为 <源码根>/build（out-of-source，不污染源码）
#     native 用 build-native，mingw 用 build-mingw/<arch>
#   - 自动精简生成的动态库（去调试信息、保留动态导出符号）
#   - 自动导出 compile_commands.json 供 clangd（nvim LSP）使用
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------- 默认配置 ----------
DEFAULT_TARGETS="android,native,mingw"
DEFAULT_ABI="arm64-v8a"
DEFAULT_MINGW_ARCH="x86_64,i686"
DEFAULT_PLATFORM="android-27"
FALLBACK_NDK="/home/lzup/Android/Sdk/ndk/28.0.13004108/"

TARGETS="$DEFAULT_TARGETS"
ABI="$DEFAULT_ABI"
MINGW_ARCH="$DEFAULT_MINGW_ARCH"
PLATFORM="$DEFAULT_PLATFORM"
NDK_PATH=""
CLEAN=0
STRIP_ENABLED=1

# ---------- 帮助 ----------
help() {
    cat <<'EOF'
用法:
  ./make.sh <源码目录> [选项]

选项:
  -t, --target <目标>   构建目标，多个用逗号分隔（默认 android,native,mingw）
                        android  使用 NDK 交叉编译 Android .so
                        native   直接编译（不使用 NDK，本机 Linux .so）
                        mingw    MinGW 交叉编译 Windows .dll（32/64 位）
  -a, --abi <abi>       目标 ABI，多个用逗号分隔（默认 arm64-v8a，仅 android）
                        arm64-v8a, armeabi-v7a, x86_64, x86
  -m, --mingw-arch <a>  MinGW 架构，多个用逗号分隔（默认 x86_64,i686，仅 mingw）
  -n, --ndk <路径>      指定 NDK 路径（默认自动探测，仅 android）
  -p, --platform <n>    Android API 级别（默认 27，仅 android）
  -c, --clean           清理构建目录后重新构建
  -s, --no-strip        构建完成后不精简动态库
  -h, --help            显示本帮助

示例:
  ./make.sh ./manalock                       # 全部三个目标
  ./make.sh ./doublespeed -t android -a arm64-v8a,armeabi-v7a -c
  ./make.sh ./manalock -t native
  ./make.sh ./manalock -t mingw
  ./make.sh ./manalock -t mingw -m x86_64 -c
EOF
    exit 0
}

# ---------- NDK 自动探测 ----------
# 优先级: ANDROID_NDK_HOME > ANDROID_NDK_ROOT > SDK/ndk 最新版 > 内置默认路径
detect_ndk() {
    local sdk newest

    if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        echo "$ANDROID_NDK_HOME"
        return 0
    fi
    if [ -n "${ANDROID_NDK_ROOT:-}" ] && [ -d "$ANDROID_NDK_ROOT" ]; then
        echo "$ANDROID_NDK_ROOT"
        return 0
    fi

    sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$sdk" ] && [ -d "$sdk/ndk" ]; then
        newest="$(ls -1 "$sdk/ndk" 2>/dev/null | grep -E '^[0-9]+(\.[0-9]+)*$' | sort -V | tail -n 1 || true)"
        if [ -n "$newest" ] && [ -d "$sdk/ndk/$newest" ]; then
            echo "$sdk/ndk/$newest"
            return 0
        fi
    fi

    if [ -d "$FALLBACK_NDK" ]; then
        echo "$FALLBACK_NDK"
        return 0
    fi

    return 1
}

# ---------- 参数解析 ----------
PROJECT_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            help ;;
        -t|--target)
            [ $# -ge 2 ] || { echo "错误: $1 需要参数" >&2; exit 1; }
            TARGETS="$2"; shift 2 ;;
        -a|--abi)
            [ $# -ge 2 ] || { echo "错误: $1 需要参数" >&2; exit 1; }
            ABI="$2"; shift 2 ;;
        -m|--mingw-arch)
            [ $# -ge 2 ] || { echo "错误: $1 需要参数" >&2; exit 1; }
            MINGW_ARCH="$2"; shift 2 ;;
        -n|--ndk)
            [ $# -ge 2 ] || { echo "错误: $1 需要参数" >&2; exit 1; }
            NDK_PATH="$2"; shift 2 ;;
        -p|--platform)
            [ $# -ge 2 ] || { echo "错误: $1 需要参数" >&2; exit 1; }
            PLATFORM="$2"; shift 2 ;;
        -c|--clean)
            CLEAN=1; shift ;;
        -s|--no-strip)
            STRIP_ENABLED=0; shift ;;
        -*)
            echo "错误: 未知选项: $1" >&2
            echo "运行 $0 --help 查看用法" >&2
            exit 1 ;;
        *)
            if [ -n "$PROJECT_DIR" ]; then
                echo "错误: 只能指定一个源码目录" >&2
                exit 1
            fi
            PROJECT_DIR="$1"; shift ;;
    esac
done

# ---------- 前置检查 ----------
if [ -z "$PROJECT_DIR" ]; then
    echo "错误: 必须传入源码目录路径" >&2
    echo "用法: $0 <源码目录> [选项]（运行 $0 --help 查看更多）" >&2
    exit 1
fi

if [ ! -d "$PROJECT_DIR" ]; then
    echo "错误: 源码目录不存在: $PROJECT_DIR" >&2
    exit 1
fi
PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"

if [ ! -f "$PROJECT_DIR/CMakeLists.txt" ]; then
    echo "错误: 目录中没有 CMakeLists.txt（不是有效源码根）: $PROJECT_DIR" >&2
    exit 1
fi

command -v cmake >/dev/null 2>&1 || {
    echo "错误: 未找到 cmake，请先安装（sudo pacman -S cmake）" >&2
    exit 1
}

IFS=',' read -ra TARGET_LIST <<< "$TARGETS"
IFS=',' read -ra ABIS <<< "$ABI"
IFS=',' read -ra MINGW_ARCHS <<< "$MINGW_ARCH"

# ---------- 校验目标与工具 ----------
for target in "${TARGET_LIST[@]}"; do
    case "$target" in
        android) ;;
        native)
            command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || {
                echo "错误: 未找到本机 C 编译器（cc/gcc），请先安装（sudo pacman -S gcc）" >&2
                exit 1
            } ;;
        mingw)
            for arch in "${MINGW_ARCHS[@]}"; do
                command -v "$arch-w64-mingw32-gcc" >/dev/null 2>&1 || {
                    echo "错误: 未找到 MinGW 交叉编译器 $arch-w64-mingw32-gcc" >&2
                    echo "  请先安装（sudo pacman -S mingw-w64-toolchain）" >&2
                    exit 1
                }
            done ;;
        *)
            echo "错误: 未知目标: $target（可选: android, native, mingw）" >&2
            exit 1 ;;
    esac
done

# ---------- 目标相关配置 ----------
# NDK 定位（仅当需要 android 时）
if printf '%s\n' "${TARGET_LIST[@]}" | grep -qx "android"; then
    if [ -z "$NDK_PATH" ]; then
        if ! NDK_PATH="$(detect_ndk)"; then
            echo "错误: 未找到 NDK" >&2
            echo "  请通过 --ndk <路径> 指定，或设置 ANDROID_NDK_HOME 环境变量" >&2
            exit 1
        fi
    fi
    TOOLCHAIN="$NDK_PATH/build/cmake/android.toolchain.cmake"
    if [ ! -f "$TOOLCHAIN" ]; then
        echo "错误: 找不到 NDK toolchain: $TOOLCHAIN" >&2
        exit 1
    fi
fi

# ---------- 清理 ----------
if [ "$CLEAN" -eq 1 ]; then
    CLEAN_DIRS=()
    for target in "${TARGET_LIST[@]}"; do
        case "$target" in
            android) CLEAN_DIRS+=("$PROJECT_DIR/build") ;;
            native)  CLEAN_DIRS+=("$PROJECT_DIR/build-native") ;;
            mingw)   CLEAN_DIRS+=("$PROJECT_DIR/build-mingw") ;;
        esac
    done
    for dir in "${CLEAN_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            rm -rf "$dir"
            echo "🧹 已清理: $dir"
        fi
    done
    [ "${#CLEAN_DIRS[@]}" -gt 0 ] || echo "（无需清理，构建目录不存在）"
fi

# ---------- 工具函数 ----------
# 导出编译数据库到源码根，供 clangd（nvim LSP）使用
export_compile_commands() {
    local build_dir="$1"
    local link="$PROJECT_DIR/compile_commands.json"
    local target="$build_dir/compile_commands.json"

    [ -f "$target" ] || return 0

    # 已是正确链接则跳过
    if [ -L "$link" ] && [ "$(readlink -f "$link")" = "$(readlink -f "$target")" ]; then
        return 0
    fi
    # 存在旧链接/普通文件则替换
    if [ -e "$link" ] || [ -L "$link" ]; then
        rm -f "$link"
        echo "↻ 已更新 compile_commands.json 链接"
    fi
    ln -s "$target" "$link"
    echo "✓ 已导出 compile_commands.json 到源码根（clangd 用）"
}

# 精简动态库：去调试信息、保留动态导出符号
strip_libs() {
    local build_dir="$1"
    local strip_bin="$2"
    local ext="$3"
    local lib found=0

    if [ ! -x "$(command -v "$strip_bin" 2>/dev/null)" ]; then
        echo "警告: 未找到 strip 工具，跳过精简: $strip_bin" >&2
        return 0
    fi

    while IFS= read -r -d '' lib; do
        "$strip_bin" --strip-unneeded "$lib"
        echo "✓ 已精简: ${lib#"$build_dir"/}"
        found=1
    done < <(find "$build_dir" -name "*.$ext" -type f -print0)

    if [ "$found" -eq 0 ]; then
        echo "警告: 构建目录中未找到 .$ext 文件" >&2
    fi
}

# 构建单个目标
# build_one <target> <variant>
#   android: variant = ABI；native: 忽略；mingw: variant = MinGW 架构
build_one() {
    local target="$1"
    local variant="$2"
    local build_dir
    local cmake_args=()
    local c_flags="-ffunction-sections -fdata-sections"
    local strip_bin ext

    case "$target" in
        android)
            # 多 ABI 时按 ABI 分子目录，互不干扰；单 ABI 保持原 build/ 路径
            if [ "${#ABIS[@]}" -gt 1 ]; then
                build_dir="$PROJECT_DIR/build/$variant"
            else
                build_dir="$PROJECT_DIR/build"
            fi
            cmake_args=(
                -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
                -DANDROID_ABI="$variant"
                -DANDROID_PLATFORM="$PLATFORM"
                -DANDROID_NDK="$NDK_PATH"
            )
            strip_bin="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
            ext="so"
            echo "▶ ABI:      $variant"
            echo "▶ Platform: $PLATFORM"
            ;;
        native)
            build_dir="$PROJECT_DIR/build-native"
            strip_bin="strip"
            ext="so"
            ;;
        mingw)
            build_dir="$PROJECT_DIR/build-mingw/$variant"
            cmake_args=(
                -DCMAKE_SYSTEM_NAME=Windows
                -DCMAKE_SYSTEM_PROCESSOR="$variant"
                -DCMAKE_C_COMPILER="$variant-w64-mingw32-gcc"
                -DCMAKE_CXX_COMPILER="$variant-w64-mingw32-g++"
                -DCMAKE_RC_COMPILER="$variant-w64-mingw32-windres"
            )
            strip_bin="$variant-w64-mingw32-strip"
            ext="dll"
            # Windows/mingw 需要定义 BUILDING_DLL 以导出符号（__declspec(dllexport)）
            c_flags="-DBUILDING_DLL $c_flags"
            echo "▶ 架构:     $variant"
            ;;
    esac

    echo ""
    echo "▶ 源码目录: $PROJECT_DIR"
    echo "▶ 构建目录: $build_dir"
    echo "▶ 目标:     $target"

    cmake -S "$PROJECT_DIR" -B "$build_dir" \
        "${cmake_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_C_FLAGS="$c_flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--gc-sections"

    cmake --build "$build_dir" -j"$(nproc)"

    export_compile_commands "$build_dir"

    if [ "$STRIP_ENABLED" -eq 1 ]; then
        strip_libs "$build_dir" "$strip_bin" "$ext"
    fi

    echo ""
    echo "=== 构建完成: $target ${variant:+/$variant} ==="
    find "$build_dir" -name "*.$ext" -type f -exec ls -lh {} \;
}

# ---------- 执行构建 ----------
echo "▶ 目标列表: ${TARGET_LIST[*]}"
for target in "${TARGET_LIST[@]}"; do
    case "$target" in
        android)
            for abi in "${ABIS[@]}"; do
                build_one "android" "$abi"
            done ;;
        native)
            build_one "native" "" ;;
        mingw)
            for arch in "${MINGW_ARCHS[@]}"; do
                build_one "mingw" "$arch"
            done ;;
    esac
done
