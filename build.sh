#!/usr/bin/env bash
# =============================================================================
# VortexOS — build.sh
# Быстрая сборка + запуск из MSYS2 UCRT терминала.
# Использование:
#   bash build.sh          — только сборка
#   bash build.sh run      — сборка + QEMU
#   bash build.sh clean    — очистка
# =============================================================================

set -e

# Добавляем toolchain в PATH
export PATH="/d/x86_64-elf-tools/bin:$PATH"

cd "$(dirname "$0")"

case "${1:-build}" in
    build)
        make all
        ;;
    run)
        make run-direct
        ;;
    iso)
        make iso
        ;;
    clean)
        make clean
        ;;
    *)
        echo "Использование: bash build.sh [build|run|iso|clean]"
        exit 1
        ;;
esac
