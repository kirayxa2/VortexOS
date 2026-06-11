#!/bin/bash
# =============================================================================
# VortexOS — dev.sh
# Быстрый цикл разработки: pull → build → run
#
# Использование:
#   bash dev.sh              — pull + build + run (обычный дисплей)
#   bash dev.sh --gpu        — pull + build + run-gpu (virtio-gpu)
#   bash dev.sh build        — pull + build only (без запуска)
#   bash dev.sh build --gpu  — то же
#   bash dev.sh clean        — очистка build/
#   bash dev.sh watch        — pull + авто-пересборка при изменении файлов
#   bash dev.sh qemu         — только QEMU (без pull/build)
#   bash dev.sh qemu --gpu   — только QEMU с virtio-gpu
# =============================================================================

PROJECT_DIR="/d/VOS"
QEMU="/d/qemu/qemu-system-x86_64.exe"

# --- Цвета -------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;37m'
BOLD='\033[1m'
NC='\033[0m'

# --- Разбор аргументов -------------------------------------------------------
ACTION="run"
GPU=false

for arg in "$@"; do
    case "$arg" in
        --gpu)  GPU=true ;;
        build|run|watch|clean|qemu) ACTION="$arg" ;;
        -h|--help)
            echo -e "${YELLOW}Usage: bash dev.sh [ACTION] [--gpu]${NC}"
            echo ""
            echo -e "  ${GRAY}run${NC}    [default] Pull + build + QEMU"
            echo -e "  ${GRAY}build${NC}  Pull + build (без запуска)"
            echo -e "  ${GRAY}watch${NC}  Pull + авто-rebuild при изменении файлов"
            echo -e "  ${GRAY}clean${NC}  Удалить build/"
            echo -e "  ${GRAY}qemu${NC}   Запустить QEMU без пересборки"
            echo ""
            echo -e "  ${GRAY}--gpu${NC}  Запуск с virtio-gpu (run-gpu)"
            exit 0
            ;;
        *)
            echo -e "${RED}Неизвестный аргумент: $arg${NC}"
            echo "Попробуй: bash dev.sh --help"
            exit 1
            ;;
    esac
done

cd "$PROJECT_DIR" || { echo -e "${RED}[ERROR] Cannot cd to $PROJECT_DIR${NC}"; exit 1; }

# --- Утилиты -----------------------------------------------------------------
header() {
    echo ""
    echo -e "${CYAN}============================================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}============================================================${NC}"
}

step() {
    echo -e "${YELLOW}[$1/$2]${NC} $3"
}

ok() {
    echo -e "      ${GREEN}[OK]${NC} $1"
}

fail() {
    echo -e "      ${RED}[FAIL]${NC} $1"
}

# --- Git Pull -----------------------------------------------------------------
do_pull() {
    step "$1" "$2" "Git pull..."
    if git pull --ff-only 2>&1; then
        ok "Git pull done"
    else
        echo -e "      ${YELLOW}[WARN]${NC} git pull --ff-only не удался, пробую git pull..."
        if git pull 2>&1; then
            ok "Git pull done (merge)"
        else
            fail "Git pull FAILED"
            return 1
        fi
    fi
}

# --- Полная сборка ------------------------------------------------------------
build_all() {
    local total=5
    header "VortexOS Build  $(date '+%H:%M:%S')"

    do_pull 1 $total || return 1

    step 2 $total "Cleaning..."
    make clean > /dev/null 2>&1
    ok "Clean done"

    step 3 $total "Building kernel..."
    if ! make 2>&1; then
        fail "Kernel build FAILED"
        return 1
    fi
    ok "Kernel built"

    step 4 $total "Building userspace..."
    if ! make userspace 2>&1; then
        fail "Userspace build FAILED"
        return 1
    fi
    ok "Userspace built"

    step 5 $total "Creating disk image with apps..."
    make disk-clean > /dev/null 2>&1
    if ! make disk-with-apps 2>&1; then
        fail "Disk creation FAILED"
        return 1
    fi
    ok "Disk image ready"

    echo ""
    echo -e "  ${GREEN}${BOLD}Build successful!${NC}"
    return 0
}

# --- QEMU запуск -------------------------------------------------------------
run_qemu() {
    if $GPU; then
        header "Launching VortexOS in QEMU (virtio-gpu)"
    else
        header "Launching VortexOS in QEMU"
    fi
    echo -e "  ${GRAY}Press Ctrl+C or close QEMU to stop${NC}"
    echo ""

    if $GPU; then
        make run-gpu
    else
        make run
    fi
}

# --- Watch mode ---------------------------------------------------------------
watch_mode() {
    header "Watch Mode — Auto-rebuild on file changes"
    echo -e "  ${GRAY}Watching kernel/**/*.{c,h,asm} and userspace/*.c${NC}"
    echo -e "  ${GRAY}Press Ctrl+C to stop${NC}"
    echo ""

    build_all || return 1
    echo ""
    echo -e "  ${CYAN}Waiting for changes...${NC}"

    get_checksums() {
        find kernel userspace -name "*.c" -o -name "*.h" -o -name "*.asm" -o -name "*.ld" \
            2>/dev/null | sort | xargs md5sum 2>/dev/null
    }

    last="$(get_checksums)"

    while true; do
        sleep 1
        current="$(get_checksums)"
        if [ "$current" != "$last" ]; then
            last="$current"
            echo ""
            echo -e "  ${YELLOW}Change detected! $(date '+%H:%M:%S')${NC}"
            sleep 0.3
            build_all
            echo ""
            echo -e "  ${CYAN}Waiting for changes...${NC}"
        fi
    done
}

# --- Main dispatch ------------------------------------------------------------
case "$ACTION" in
    build)
        build_all
        ;;
    run)
        build_all && run_qemu
        ;;
    watch)
        watch_mode
        ;;
    clean)
        header "Cleaning"
        make clean
        ok "Done"
        ;;
    qemu)
        run_qemu
        ;;
esac
