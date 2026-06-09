#!/bin/bash
# =============================================================================
# VortexOS - Build & Run Script
# Запуск из MSYS2: bash /d/VOS/dev.sh [build|run|watch|clean|qemu]
# =============================================================================

PROJECT_DIR="/d/VOS"
QEMU="/d/qemu/qemu-system-x86_64.exe"

# Цвета
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;37m'
BOLD='\033[1m'
NC='\033[0m' # No Color

ACTION="${1:-run}"

cd "$PROJECT_DIR" || { echo -e "${RED}[ERROR] Cannot cd to $PROJECT_DIR${NC}"; exit 1; }

# ============================================================
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

# ============================================================
build_all() {
    header "VortexOS Build  $(date '+%H:%M:%S')"

    step 1 4 "Cleaning..."
    make clean > /dev/null 2>&1
    ok "Clean done"

    step 2 4 "Building kernel..."
    if ! make 2>&1; then
        fail "Kernel build FAILED"
        return 1
    fi
    ok "Kernel built"

    step 3 4 "Building userspace..."
    if ! make userspace 2>&1; then
        fail "Userspace build FAILED"
        return 1
    fi
    ok "Userspace built"

    step 4 4 "Creating disk image..."
    if ! make disk-with-apps 2>&1; then
        fail "Disk creation FAILED"
        return 1
    fi
    ok "Disk image ready"

    echo ""
    echo -e "  ${GREEN}${BOLD}Build successful!${NC}"
    return 0
}

# ============================================================
run_qemu() {
    header "Launching VortexOS in QEMU"
    echo -e "  ${GRAY}Press Ctrl+C or close QEMU to stop${NC}"
    echo ""
    make run
}

# ============================================================
watch_mode() {
    header "Watch Mode - Auto-rebuild on file changes"
    echo -e "  ${GRAY}Watching kernel/**/*.{c,h,asm} and userspace/*.c${NC}"
    echo -e "  ${GRAY}Press Ctrl+C to stop${NC}"
    echo ""

    # Первая сборка
    build_all
    echo ""
    echo -e "  ${CYAN}Waiting for changes...${NC}"

    # Запоминаем контрольные суммы
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
            sleep 0.3  # debounce
            build_all
            echo ""
            echo -e "  ${CYAN}Waiting for changes...${NC}"
        fi
    done
}

# ============================================================
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
        header "Launching QEMU (no rebuild)"
        run_qemu
        ;;
    *)
        echo -e "${YELLOW}Usage: bash dev.sh [build|run|watch|clean|qemu]${NC}"
        echo -e "  ${GRAY}build${NC}  - Clean + build kernel + userspace + disk"
        echo -e "  ${GRAY}run${NC}    - Build everything + launch QEMU"
        echo -e "  ${GRAY}watch${NC}  - Auto-rebuild on .c/.h/.asm changes"
        echo -e "  ${GRAY}clean${NC}  - Remove build artifacts"
        echo -e "  ${GRAY}qemu${NC}   - Launch QEMU without rebuilding"
        ;;
esac
