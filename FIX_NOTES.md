# VOS Page Fault Fix — Решение проблемы

## Проблема
Page Fault при запуске userspace программы `/vgraph`:
```
[EXCEPTION] Page Fault err=0x0000000000000004 rip=0x0000000000400000 cs=0x0000000000000023
```

- `err=0x4` = USER mode, page NOT PRESENT
- `rip=0x400000` = первая инструкция в `_start`
- Страница кода **замаплена**, но **недоступна**

## Корень проблемы

В `kernel/mm/vmm.c` функция `vmm_switch(pte_t *pml4)` использовала:

```c
uint64_t phys = table_phys(pml4);  // НЕПРАВИЛЬНО для user PML4!
```

`table_phys()` работает через **HHDM арифметику** (`virt - hhdm_offset`), которая корректна только для kernel mappings в HHDM области (`0xFFFF800000000000+`).

Но **user PML4 создается через `alloc_table()` в heap** (`0xFFFFFFFF81000000+`), который **не в HHDM!**

Результат: `CR3` загружался с **неправильным физическим адресом**, процессор не мог найти page table, все userspace страницы казались unmapped → Page Fault.

## Решение

Исправлено в `kernel/mm/vmm.c`:

```c
void vmm_switch(pte_t *pml4) {
    uint64_t phys;
    /* Если это kernel PML4 (в HHDM), используем арифметику HHDM.
     * Если это user PML4 (в heap), используем page table walk. */
    if ((uint64_t)pml4 == (uint64_t)vmm_kernel_pml4) {
        phys = table_phys(pml4);
    } else {
        phys = vmm_virt_to_phys(vmm_kernel_pml4, (uint64_t)pml4);
        if (!phys) {
            fb_puts("[VMM] ERROR: Cannot get physical address of user PML4\n");
            return;
        }
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}
```

Теперь для user PML4 используется **page table walk через `vmm_virt_to_phys()`**, который корректно получает физический адрес heap-allocated структуры.

## Дополнительные исправления

1. **kernel/kmain.c**: Исправлен kernel stack размер:
   ```c
   // Было: kmalloc_aligned(16384 + 4096, 4096) + ... + 16384
   // Стало: kmalloc_aligned(32768, 4096) + ... + 32768
   ```

2. **kernel/fs/elf.c**: Уже исправлен ранее — каждая страница сегмента маппится отдельно через `vmm_virt_to_phys()`, т.к. heap pages физически не непрерывны.

3. **userspace/vortexgraph.c**: `hlt` заменен на `pause` (hlt — привилегированная инструкция).

## Сборка

```bash
make clean
make userspace
make disk-with-apps
make iso
make run
```

Или:
```bash
./rebuild.bat  # Windows
make run
```

## Проверка

После исправления userspace программы должны запускаться без Page Fault:

```
[TASK] Entering usermode...
VortexGraph: Display server starting...
VortexGraph: Framebuffer mapped at 0x80000000
VortexGraph: Test pattern drawn!
```

## Технические детали

### Почему heap не в HHDM?

- **HHDM** (Higher Half Direct Mapping) = linear mapping всей физической памяти начиная с `0xFFFF800000000000`
- **Kernel heap** = динамическая область на `0xFFFFFFFF81000000`, страницы которой выделяются через PMM и маппятся через VMM
- Heap pages **не гарантируют физическую непрерывность**, поэтому нужен page table walk

### Почему table_phys() работает для kernel PML4?

Kernel PML4 создается **до включения paging** из pre-allocated физической памяти и маппится в HHDM. Поэтому `kernel_pml4_virt - hhdm_offset = kernel_pml4_phys` работает.

User PML4 создается **после** инициализации heap через `kmalloc → alloc_table()`, и находится в heap области, где нужен page table walk.

## Ссылки на исходники VortexOS

Для справки посмотри:
- `VortexOS/src/mem/paging.c` — правильная реализация с `v2p()` через page table walk
- `VortexOS/src/sys/process.c` — как создается user process с правильным маппингом страниц
