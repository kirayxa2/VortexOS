#!/usr/bin/env bash
# =============================================================================
# VortexOS — setup.sh
# Запускать из MSYS2 UCRT терминала ОДИН РАЗ для установки зависимостей.
# =============================================================================

set -e

echo "=== Устанавливаем зависимости через pacman ==="

# NASM — ассемблер
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-nasm

# QEMU — эмулятор
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-qemu

# make
pacman -S --needed --noconfirm make

# xorriso + grub (нужны для make iso, необязательно для make run-direct)
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-xorriso || true
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-grub    || true

echo ""
echo "=== Проверяем инструменты ==="
echo -n "nasm:  "; nasm --version 2>/dev/null || echo "НЕ НАЙДЕН"
echo -n "qemu:  "; qemu-system-x86_64 --version 2>/dev/null | head -1 || echo "НЕ НАЙДЕН"
echo -n "make:  "; make --version 2>/dev/null | head -1 || echo "НЕ НАЙДЕН"
echo -n "gcc:   "; /d/x86_64-elf-tools/bin/x86_64-elf-gcc --version 2>/dev/null | head -1 || echo "НЕ НАЙДЕН"

echo ""
echo "=== Добавляем toolchain в PATH текущей сессии ==="
export PATH="/d/x86_64-elf-tools/bin:$PATH"

echo ""
echo "=== Готово! Теперь запускай: ==="
echo "  cd /d/VOS"
echo "  make run-direct"
