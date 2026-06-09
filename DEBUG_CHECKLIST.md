# VOS Page Fault Debug Checklist

## Текущая ситуация
Page Fault при rip=0x400000 (entry point userspace программы):
```
[EXCEPTION] Page Fault err=0x0000000000000004 rip=0x0000000000400000 cs=0x0000000000000023
```

## Что проверяем

### 1. Маппинг работает?
**Ожидаемый вывод:**
```
[ELF] Map page vaddr=0x0000000000400000 -> paddr=0x0000000000113000
```

**Возможные ошибки:**
- `[VMM] Failed to get/create PDPT/PD/PT` — не удается создать промежуточные таблицы
- `[VMM ERROR] PT entry write failed!` — запись в PT не работает

### 2. Страница видна в user PML4?
**Добавлено в kmain.c:**
```
[TASK] Verifying entry point mapping in user PML4...
[TASK] Entry point maps to phys 0x0000000000113000
```

**Возможные ошибки:**
- `[TASK] ERROR: Entry point ... NOT MAPPED` — страница не видна через vmm_virt_to_phys
- `[VMM] PML4/PDPT/PD/PT entry not present` — на каком уровне page table walk ломается

### 3. Флаг USER установлен?
**Ожидаемый вывод:**
- Нет warning о USER флаге

**Возможные ошибки:**
- `[VMM] WARNING: PT entry ... does NOT have USER flag!` — страница замаплена без USER флага

## Возможные проблемы и решения

### Проблема 1: vmm_map не создает промежуточные таблицы
**Причина:** PMM не может выделить физическую память для PDPT/PD/PT
**Решение:** Увеличить размер heap или проверить PMM

### Проблема 2: Страница замаплена но не видна через vmm_virt_to_phys
**Причина:** 
- `table_phys` возвращает неправильный физический адрес
- `phys_to_virt` не может преобразовать обратно
**Решение:** Проверить что hhdm_offset инициализирован правильно

### Проблема 3: Флаг USER не установлен
**Причина:** В `vmm_map` флаги не передаются правильно
**Решение:** Проверить что вызов `vmm_map(user_pml4, vaddr, paddr, VMM_PRESENT | VMM_WRITABLE | VMM_USER)` передает все флаги

### Проблема 4: Page Fault происходит после успешного маппинга
**Возможные причины:**
1. **TLB не сброшен** после маппинга
   - Решение: `vmm_switch` уже сбрасывает TLB
   
2. **CR3 указывает не на тот PML4**
   - Решение: Проверить что `table_phys(user_pml4)` возвращает правильный физический адрес
   - Добавить отладку: вывести CR3 перед iretq
   
3. **Промежуточные таблицы не имеют флага USER**
   - Решение: В `get_or_create` используется `table_flags = VMM_PRESENT | VMM_WRITABLE | VMM_USER`
   - Проверить что флаги действительно устанавливаются

4. **Процессор не поддерживает пользовательский режим**
   - Маловероятно, но проверить GDT селекторы 0x1B и 0x23

## Следующие шаги

1. **Пересобрать** с новой отладкой:
   ```bash
   make clean
   make userspace
   make disk-with-apps
   make iso
   ```

2. **Запустить** и собрать вывод:
   ```bash
   make run > debug_output.txt 2>&1
   ```

3. **Проанализировать** вывод по чеклисту выше

4. Если все проверки проходят но Page Fault все равно есть:
   - Добавить вывод CR3 перед enter_usermode
   - Проверить что CPU features поддерживают user mode (CPUID)
   - Дамп первых байт physical memory 0x113000 чтобы проверить что код там есть

## Справка: Error Code биты

Page Fault Error Code = 0x4:
- Бит 0 (P): 0 = Page not present ← **ГЛАВНАЯ ПРОБЛЕМА**
- Бит 1 (W/R): 0 = Read access
- Бит 2 (U/S): 1 = User mode ← правильно
- Бит 3 (RSVD): 0 = No reserved bit violation
- Бит 4 (I/D): 0 = Not instruction fetch (может быть неточно)

Значит процессор считает что страница 0x400000 **NOT PRESENT** в user page table!
