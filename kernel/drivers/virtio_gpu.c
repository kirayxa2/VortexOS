/* =============================================================================
 * VortexOS — kernel/drivers/virtio_gpu.c
 * Минимальный virtio-gpu (2D) драйвер для аппаратного present без разрывов.
 *
 * Поддерживается СОВРЕМЕННЫЙ virtio (virtio 1.0, PCI device id 0x1050) — именно
 * такой даёт QEMU при `-vga virtio` (virtio-vga) или `-device virtio-gpu-pci`.
 * Конфиг устройства лежит в MMIO BAR, адресуется через PCI vendor-cap'ы.
 *
 * Поток инициализации (всё с таймаутами, любой сбой → init возвращает 0):
 *   найти PCI 1AF4:1050 → распарсить cap'ы (common/notify/isr/device cfg) →
 *   замапить BAR'ы → reset → ACK|DRIVER → принять VERSION_1 → FEATURES_OK →
 *   поднять controlq (queue 0) → DRIVER_OK →
 *   RESOURCE_CREATE_2D → выделить fb-backing → ATTACH_BACKING → SET_SCANOUT.
 *
 * present(x,y,w,h): TRANSFER_TO_HOST_2D(rect) + RESOURCE_FLUSH(rect).
 * ============================================================================= */

#include "virtio_gpu.h"
#include "pci.h"
#include "fb.h"
#include "pmm.h"
#include "vmm.h"

extern uint64_t hhdm_offset;   /* из vmm.c — смещение HHDM */

/* --- порты/инлайны для barrier --------------------------------------------- */
static inline void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

/* --- MMIO accessors -------------------------------------------------------- */
static inline uint8_t  mmio_r8 (volatile void *p)            { return *(volatile uint8_t *)p; }
static inline uint16_t mmio_r16(volatile void *p)            { return *(volatile uint16_t *)p; }
static inline uint32_t mmio_r32(volatile void *p)            { return *(volatile uint32_t *)p; }
static inline void     mmio_w8 (volatile void *p, uint8_t v) { *(volatile uint8_t  *)p = v; }
static inline void     mmio_w16(volatile void *p, uint16_t v){ *(volatile uint16_t *)p = v; }
static inline void     mmio_w32(volatile void *p, uint32_t v){ *(volatile uint32_t *)p = v; }
static inline void     mmio_w64(volatile void *p, uint64_t v){ *(volatile uint64_t *)p = v; }

/* --- virtio PCI capability cfg_type ---------------------------------------- */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* --- virtio device status -------------------------------------------------- */
#define VIRTIO_STAT_ACKNOWLEDGE  1
#define VIRTIO_STAT_DRIVER       2
#define VIRTIO_STAT_DRIVER_OK    4
#define VIRTIO_STAT_FEATURES_OK  8
#define VIRTIO_STAT_FAILED       128

/* --- common_cfg byte offsets ----------------------------------------------- */
#define CC_DEV_FEAT_SEL  0x00
#define CC_DEV_FEAT      0x04
#define CC_DRV_FEAT_SEL  0x08
#define CC_DRV_FEAT      0x0C
#define CC_NUM_QUEUES    0x12
#define CC_DEV_STATUS    0x14
#define CC_QUEUE_SEL     0x16
#define CC_QUEUE_SIZE    0x18
#define CC_QUEUE_ENABLE  0x1C
#define CC_QUEUE_NOTIFY  0x1E
#define CC_QUEUE_DESC    0x20
#define CC_QUEUE_DRIVER  0x28
#define CC_QUEUE_DEVICE  0x30

/* --- split virtqueue ------------------------------------------------------- */
#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

typedef struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } vq_desc_t;
typedef struct { uint16_t flags; uint16_t idx; uint16_t ring[];     } vq_avail_t;
typedef struct { uint32_t id; uint32_t len; }                         vq_used_elem_t;
typedef struct { uint16_t flags; uint16_t idx; vq_used_elem_t ring[]; } vq_used_t;

#define QSIZE 16  /* мы сами уменьшаем очередь до 16 — кольца влезают в 1 страницу */

/* --- virtio-gpu protocol --------------------------------------------------- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF        0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM     1

typedef struct { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id, padding; } gpu_hdr_t;
typedef struct { uint32_t x, y, width, height; } gpu_rect_t;

typedef struct { gpu_hdr_t hdr; uint32_t resource_id, format, width, height; } gpu_create_2d_t;
typedef struct { gpu_hdr_t hdr; uint32_t resource_id, nr_entries; } gpu_attach_backing_t;
typedef struct { uint64_t addr; uint32_t length, padding; } gpu_mem_entry_t;
typedef struct { gpu_hdr_t hdr; gpu_rect_t r; uint32_t scanout_id, resource_id; } gpu_set_scanout_t;
typedef struct { gpu_hdr_t hdr; gpu_rect_t r; uint64_t offset; uint32_t resource_id, padding; } gpu_xfer_2d_t;
typedef struct { gpu_hdr_t hdr; gpu_rect_t r; uint32_t resource_id, padding; } gpu_flush_t;
typedef struct { gpu_hdr_t hdr; uint32_t resource_id, padding; } gpu_res_unref_t;

/* --- driver state ---------------------------------------------------------- */
#define MAX_ENTRY_PAGES 16   /* потолок: 16*256 entry = 4096 страниц = 16 МБ fb */

/* Backing выделяется ОДИН раз под максимальный режим: смена разрешения не
 * трогает память (и user-mapping в vwm остаётся валидным), меняется только
 * GPU-ресурс + scanout. 1920x1200x4 = ~9.2 МБ. */
#define VGPU_MAX_W 1920
#define VGPU_MAX_H 1200

static int       g_active = 0;
static uint64_t  g_hhdm   = 0;

static volatile uint8_t *cc_base   = 0;  /* common cfg  */
static volatile uint8_t *notify_base = 0;
static uint32_t  notify_mult = 0;
static uint16_t  q_notify_off = 0;

/* virtqueue (одна страница: desc | avail | used) */
static uint64_t  ring_phys = 0;
static vq_desc_t  *vq_desc  = 0;
static vq_avail_t *vq_avail = 0;
static vq_used_t  *vq_used  = 0;
static uint16_t   avail_idx = 0;
static uint16_t   used_seen = 0;

/* буферы команд (по странице) */
static uint64_t  cmd_phys = 0;  static uint8_t *cmd_buf = 0;
static uint64_t  resp_phys = 0; static uint8_t *resp_buf = 0;

/* framebuffer backing */
static uint32_t *g_fb = 0;
static uint32_t  g_w = 0, g_h = 0, g_pitch = 0;
static uint64_t  g_fb_pages = 0;                  /* страниц у ТЕКУЩЕГО режима */
static uint64_t  g_fb_max_bytes = 0;              /* размер backing (под max) */
static uint64_t  g_entry_pages[MAX_ENTRY_PAGES];  /* phys страниц с mem_entry */
static uint32_t  g_entry_page_count = 0;
static uint32_t  g_res_id = 1;                    /* текущий GPU-ресурс */

/* --- утилиты --------------------------------------------------------------- */
static void *phys_to_virt(uint64_t phys) { return (void *)(phys + g_hhdm); }

static void invlpg_all(void) {
    uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}

/* Замапить MMIO-регион BAR (phys) в HHDM-адрес как uncached, вернуть указатель. */
static volatile uint8_t *map_mmio(uint64_t phys, uint64_t len) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end   = (phys + len + 0xFFF) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000)
        vmm_map(vmm_kernel_pml4, p + g_hhdm, p,
                VMM_PRESENT | VMM_WRITABLE | (1ULL << 4) /*PCD*/);
    invlpg_all();
    return (volatile uint8_t *)(phys + g_hhdm);
}

/* --- virtqueue submit ------------------------------------------------------ */
/* segs[]: {phys, len, write?}. Строит цепочку дескрипторов, кладёт в avail,
 * нотифицирует устройство и ждёт used (с таймаутом). 0 = ok. */
typedef struct { uint64_t phys; uint32_t len; int write; } vq_seg_t;

static int vq_submit(vq_seg_t *segs, int n) {
    if (n <= 0 || n > QSIZE) return -1;
    for (int i = 0; i < n; i++) {
        vq_desc[i].addr  = segs[i].phys;
        vq_desc[i].len   = segs[i].len;
        vq_desc[i].flags = (segs[i].write ? VRING_DESC_F_WRITE : 0)
                         | ((i < n - 1) ? VRING_DESC_F_NEXT : 0);
        vq_desc[i].next  = (uint16_t)(i + 1);
    }
    vq_avail->ring[avail_idx % QSIZE] = 0;  /* head = desc 0 */
    mfence();
    avail_idx++;
    vq_avail->idx = avail_idx;
    mfence();
    /* notify queue 0 */
    mmio_w16(notify_base + (uint32_t)q_notify_off * notify_mult, 0);

    /* ждём used */
    uint64_t guard = 200000000ULL;
    while (vq_used->idx == used_seen && --guard) { __asm__ volatile("pause"); }
    if (!guard) {
        /* Диагностика: молчаливый таймаут = чёрный экран без единого следа.
         * Печатаем только первые случаи, чтобы не зафлудить serial. */
        static int timeouts = 0;
        if (timeouts < 8) { timeouts++; fb_puts("[virtio-gpu] vq_submit TIMEOUT (device not responding)\n"); }
        return -2;
    }
    used_seen = vq_used->idx;
    return 0;
}

/* Отправить простую команду (req в cmd_buf) и проверить тип ответа. */
static int gpu_cmd(uint32_t req_len, uint32_t resp_len, uint32_t want_type) {
    vq_seg_t s[2] = {
        { cmd_phys,  req_len,  0 },
        { resp_phys, resp_len, 1 },
    };
    if (vq_submit(s, 2) != 0) return -1;
    gpu_hdr_t *r = (gpu_hdr_t *)resp_buf;
    return (r->type == want_type) ? 0 : -2;
}

/* --- cap parsing ----------------------------------------------------------- */
static uint64_t bar_base(pci_device_t *d, uint8_t bar) {
    uint32_t lo = d->bar[bar];
    if (lo & 0x1) return 0;                 /* IO BAR — не наш */
    uint64_t base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 0x3) == 0x2 && bar < 5) /* 64-bit */
        base |= ((uint64_t)d->bar[bar + 1]) << 32;
    return base;
}

/* --- инициализация --------------------------------------------------------- */
static void put_dec(uint32_t n) {
    char b[12]; int i = 11; b[11] = 0;
    if (!n) b[--i] = '0'; else while (n) { b[--i] = '0' + n % 10; n /= 10; }
    fb_puts(&b[i]);
}

/* Заполнить mem_entry таблицы под bytes байт backing'а (phys по страницам
 * через page-table walk — fb лежит в kernel-heap, НЕ в HHDM). Возвращает
 * число страниц или 0 при ошибке. */
static uint64_t build_entries(uint64_t bytes) {
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    uint32_t per_page = 0x1000 / sizeof(gpu_mem_entry_t);
    if ((pages + per_page - 1) / per_page > g_entry_page_count) return 0;
    gpu_mem_entry_t *ent = 0;
    for (uint64_t pg = 0; pg < pages; pg++) {
        uint32_t ep = (uint32_t)(pg / per_page), eo = (uint32_t)(pg % per_page);
        if (eo == 0) ent = (gpu_mem_entry_t *)phys_to_virt(g_entry_pages[ep]);
        uint64_t v = (uint64_t)g_fb + pg * 0x1000;
        ent[eo].addr    = vmm_virt_to_phys(vmm_kernel_pml4, v);
        ent[eo].length  = 0x1000;
        ent[eo].padding = 0;
    }
    return pages;
}

/* CREATE_2D(res, w*h) + ATTACH_BACKING(страницы под w*h*4) + SET_SCANOUT(res).
 * 0 = ok. На успехе обновляет g_fb_pages. */
static int gpu_setup_scanout(uint32_t res, uint32_t w, uint32_t h) {
    /* RESOURCE_CREATE_2D */
    gpu_create_2d_t *cr = (gpu_create_2d_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*cr); i++) cmd_buf[i] = 0;
    cr->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cr->resource_id = res; cr->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    cr->width = w; cr->height = h;
    if (gpu_cmd(sizeof(*cr), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        fb_puts("[virtio-gpu] RESOURCE_CREATE_2D fail\n"); return -1;
    }

    /* ATTACH_BACKING (hdr-страница + entry-страницы) */
    uint64_t pages = build_entries((uint64_t)w * h * 4);
    if (!pages) return -1;
    gpu_attach_backing_t *ab = (gpu_attach_backing_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*ab); i++) cmd_buf[i] = 0;
    ab->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id = res; ab->nr_entries = (uint32_t)pages;
    {
        vq_seg_t s[2 + MAX_ENTRY_PAGES]; int n = 0;
        s[n++] = (vq_seg_t){ cmd_phys, sizeof(*ab), 0 };
        uint64_t remain = pages * sizeof(gpu_mem_entry_t);
        for (uint32_t i = 0; i < g_entry_page_count && remain; i++) {
            uint32_t l = remain > 0x1000 ? 0x1000 : (uint32_t)remain;
            s[n++] = (vq_seg_t){ g_entry_pages[i], l, 0 };
            remain -= l;
        }
        s[n++] = (vq_seg_t){ resp_phys, sizeof(gpu_hdr_t), 1 };
        if (vq_submit(s, n) != 0 ||
            ((gpu_hdr_t *)resp_buf)->type != VIRTIO_GPU_RESP_OK_NODATA) {
            fb_puts("[virtio-gpu] ATTACH_BACKING fail\n"); return -1;
        }
    }

    /* SET_SCANOUT 0 -> ресурс */
    gpu_set_scanout_t *sc = (gpu_set_scanout_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*sc); i++) cmd_buf[i] = 0;
    sc->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    sc->r.x = 0; sc->r.y = 0; sc->r.width = w; sc->r.height = h;
    sc->scanout_id = 0; sc->resource_id = res;
    if (gpu_cmd(sizeof(*sc), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        fb_puts("[virtio-gpu] SET_SCANOUT fail\n"); return -1;
    }
    g_fb_pages = pages;
    return 0;
}

int virtio_gpu_init(void) {
    g_hhdm = hhdm_offset;

    /* 1. найти устройство 1AF4:1050 (modern virtio-gpu) */
    pci_device_t *d = 0;
    uint32_t cnt = pci_device_count_get();
    for (uint32_t i = 0; i < cnt; i++) {
        pci_device_t *c = pci_get_device(i);
        if (c && c->vendor == 0x1AF4 && c->device == 0x1050) { d = c; break; }
    }
    if (!d) { fb_puts("[virtio-gpu] device not found (run QEMU with -vga virtio)\n"); return 0; }
    fb_puts("[virtio-gpu] device found, initializing...\n");

    /* включить Bus Master + Memory Space в PCI command (bit 1|2) */
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd | 0x6);

    /* 2. пройти список capability */
    uint8_t cap = pci_read8(d->bus, d->dev, d->func, 0x34) & 0xFC;
    int guard = 48;
    while (cap && guard--) {
        uint8_t id = pci_read8(d->bus, d->dev, d->func, cap + 0);
        if (id == 0x09) {                                  /* vendor-specific */
            uint8_t cfg_type = pci_read8(d->bus, d->dev, d->func, cap + 3);
            uint8_t bar      = pci_read8(d->bus, d->dev, d->func, cap + 4);
            uint32_t off     = pci_read32(d->bus, d->dev, d->func, cap + 8);
            uint32_t len     = pci_read32(d->bus, d->dev, d->func, cap + 12);
            uint64_t base    = bar_base(d, bar);
            if (base) {
                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
                    cc_base = map_mmio(base + off, len ? len : 0x1000);
                else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify_base = map_mmio(base + off, len ? len : 0x1000);
                    notify_mult = pci_read32(d->bus, d->dev, d->func, cap + 16);
                }
            }
        }
        cap = pci_read8(d->bus, d->dev, d->func, cap + 1) & 0xFC;
    }
    if (!cc_base || !notify_base) { fb_puts("[virtio-gpu] caps not found\n"); return 0; }

    /* 3. reset + ACK|DRIVER */
    mmio_w8(cc_base + CC_DEV_STATUS, 0);
    while (mmio_r8(cc_base + CC_DEV_STATUS) != 0) { }
    mmio_w8(cc_base + CC_DEV_STATUS, VIRTIO_STAT_ACKNOWLEDGE);
    mmio_w8(cc_base + CC_DEV_STATUS, VIRTIO_STAT_ACKNOWLEDGE | VIRTIO_STAT_DRIVER);

    /* 4. features: принимаем только VIRTIO_F_VERSION_1 (bit 32) */
    mmio_w32(cc_base + CC_DRV_FEAT_SEL, 0); mmio_w32(cc_base + CC_DRV_FEAT, 0);
    mmio_w32(cc_base + CC_DRV_FEAT_SEL, 1); mmio_w32(cc_base + CC_DRV_FEAT, 1u);
    uint8_t st = VIRTIO_STAT_ACKNOWLEDGE | VIRTIO_STAT_DRIVER | VIRTIO_STAT_FEATURES_OK;
    mmio_w8(cc_base + CC_DEV_STATUS, st);
    if (!(mmio_r8(cc_base + CC_DEV_STATUS) & VIRTIO_STAT_FEATURES_OK)) {
        fb_puts("[virtio-gpu] FEATURES_OK rejected\n");
        mmio_w8(cc_base + CC_DEV_STATUS, VIRTIO_STAT_FAILED); return 0;
    }

    /* 5. controlq (queue 0): одна страница под desc|avail|used */
    mmio_w16(cc_base + CC_QUEUE_SEL, 0);
    uint16_t qsz = mmio_r16(cc_base + CC_QUEUE_SIZE);
    if (qsz < QSIZE) { fb_puts("[virtio-gpu] controlq too small\n"); return 0; }
    if (qsz > QSIZE) { mmio_w16(cc_base + CC_QUEUE_SIZE, QSIZE); }

    ring_phys = pmm_alloc_zero();
    if (!ring_phys) return 0;
    uint8_t *rp = (uint8_t *)phys_to_virt(ring_phys);
    vq_desc  = (vq_desc_t  *)(rp + 0);                 /* 16*16 = 256 B */
    vq_avail = (vq_avail_t *)(rp + 256);               /* ~40 B */
    vq_used  = (vq_used_t  *)(rp + 512);               /* ~136 B */
    avail_idx = 0; used_seen = 0;

    mmio_w64(cc_base + CC_QUEUE_DESC,   ring_phys + 0);
    mmio_w64(cc_base + CC_QUEUE_DRIVER, ring_phys + 256);
    mmio_w64(cc_base + CC_QUEUE_DEVICE, ring_phys + 512);
    q_notify_off = mmio_r16(cc_base + CC_QUEUE_NOTIFY);
    mmio_w16(cc_base + CC_QUEUE_ENABLE, 1);

    /* command/response страницы */
    cmd_phys  = pmm_alloc_zero(); resp_phys = pmm_alloc_zero();
    if (!cmd_phys || !resp_phys) return 0;
    cmd_buf  = (uint8_t *)phys_to_virt(cmd_phys);
    resp_buf = (uint8_t *)phys_to_virt(resp_phys);

    /* 6. DRIVER_OK */
    mmio_w8(cc_base + CC_DEV_STATUS, st | VIRTIO_STAT_DRIVER_OK);

    /* 7. размер экрана берём из реального Limine framebuffer */
    extern uint32_t fb_width, fb_height;
    g_w = fb_width; g_h = fb_height;
    if (!g_w || !g_h) { g_w = 1024; g_h = 768; }
    g_pitch = g_w * 4;

    /* 8. backing: выделяем ОДИН раз под максимальный режим (VGPU_MAX_*).
     * Смена разрешения потом не двигает память — только GPU-ресурс. */
    g_fb_max_bytes = (uint64_t)VGPU_MAX_W * VGPU_MAX_H * 4;
    if ((uint64_t)g_pitch * g_h > g_fb_max_bytes) {
        fb_puts("[virtio-gpu] boot mode larger than max backing\n"); return 0;
    }
    {
        uint64_t max_pages = (g_fb_max_bytes + 0xFFF) / 0x1000;
        uint32_t need_ep = (uint32_t)((max_pages * sizeof(gpu_mem_entry_t) + 0xFFF) / 0x1000);
        if (need_ep > MAX_ENTRY_PAGES) { fb_puts("[virtio-gpu] max mode too large\n"); return 0; }

        extern void *kmalloc(uint64_t);
        uint8_t *raw = (uint8_t *)kmalloc(g_fb_max_bytes + 0x1000);
        if (!raw) { fb_puts("[virtio-gpu] out of memory for fb\n"); return 0; }
        g_fb = (uint32_t *)(((uint64_t)raw + 0xFFF) & ~0xFFFULL);
        for (uint64_t i = 0; i < g_fb_max_bytes / 4; i++) g_fb[i] = 0xFF000000;

        for (uint32_t i = 0; i < need_ep; i++) {
            g_entry_pages[i] = pmm_alloc_zero();
            if (!g_entry_pages[i]) return 0;
        }
        g_entry_page_count = need_ep;
    }

    /* 9. CREATE_2D + ATTACH_BACKING + SET_SCANOUT для стартового режима */
    if (gpu_setup_scanout(g_res_id, g_w, g_h) != 0) {
        fb_puts("[virtio-gpu] scanout setup fail\n"); return 0;
    }

    g_active = 1;
    fb_puts("[virtio-gpu] OK - scanout switched to GPU resource ");
    put_dec(g_w); fb_puts("x"); put_dec(g_h); fb_puts("\n");
    /* первый present всего экрана */
    virtio_gpu_flush(0, 0, (int)g_w, (int)g_h);
    return 1;
}

int       virtio_gpu_active(void)      { return g_active; }
uint64_t  virtio_gpu_backing_bytes(void){ return g_fb_max_bytes; }
uint32_t *virtio_gpu_framebuffer(void) { return g_active ? g_fb : 0; }
uint32_t  virtio_gpu_width(void)       { return g_w; }
uint32_t  virtio_gpu_height(void)      { return g_h; }
uint32_t  virtio_gpu_pitch(void)       { return g_pitch; }

void virtio_gpu_flush(int x, int y, int w, int h) {
    if (!g_active) return;
    /* клип */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_w) w = (int)g_w - x;
    if (y + h > (int)g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0) return;

    /* TRANSFER_TO_HOST_2D: переслать прямоугольник из backing в GPU-ресурс */
    gpu_xfer_2d_t *tx = (gpu_xfer_2d_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*tx); i++) cmd_buf[i] = 0;
    tx->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    tx->r.x = x; tx->r.y = y; tx->r.width = w; tx->r.height = h;
    tx->offset = (uint64_t)y * g_pitch + (uint64_t)x * 4;
    tx->resource_id = g_res_id;
    if (gpu_cmd(sizeof(*tx), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA) != 0) return;

    /* RESOURCE_FLUSH: аппаратный present (атомарно, без разрывов) */
    gpu_flush_t *fl = (gpu_flush_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*fl); i++) cmd_buf[i] = 0;
    fl->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    fl->r.x = x; fl->r.y = y; fl->r.width = w; fl->r.height = h;
    fl->resource_id = g_res_id;
    gpu_cmd(sizeof(*fl), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA);
}

/* =============================================================================
 * Смена видеорежима «на лету».
 * Backing НЕ трогаем (он под максимальный режим, mapping'и юзерспейса живы):
 * создаём новый GPU-ресурс нужного размера, цепляем тот же backing, щёлкаем
 * scanout, старый ресурс освобождаем. 0 = ok, -1 = ошибка (старый режим
 * остаётся активным).
 * ============================================================================= */
int virtio_gpu_set_mode(uint32_t w, uint32_t h) {
    if (!g_active) return -1;
    if (w < 320 || h < 200) return -1;
    if ((uint64_t)w * h * 4 > g_fb_max_bytes) return -1;
    if (w == g_w && h == g_h) return 0;

    /* чистим backing заранее, чтобы новый кадр не показал мусор */
    uint64_t px = (uint64_t)w * h;
    for (uint64_t i = 0; i < px; i++) g_fb[i] = 0xFF000000;

    uint32_t old_res = g_res_id;
    uint32_t new_res = g_res_id + 1;
    if (gpu_setup_scanout(new_res, w, h) != 0) {
        /* best effort: вернуть старый scanout (ресурс old_res ещё жив) */
        gpu_set_scanout_t *sc = (gpu_set_scanout_t *)cmd_buf;
        for (uint32_t i = 0; i < sizeof(*sc); i++) cmd_buf[i] = 0;
        sc->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        sc->r.width = g_w; sc->r.height = g_h;
        sc->scanout_id = 0; sc->resource_id = old_res;
        gpu_cmd(sizeof(*sc), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA);
        return -1;
    }

    g_res_id = new_res;
    g_w = w; g_h = h; g_pitch = w * 4;

    /* старый ресурс больше не нужен */
    gpu_res_unref_t *un = (gpu_res_unref_t *)cmd_buf;
    for (uint32_t i = 0; i < sizeof(*un); i++) cmd_buf[i] = 0;
    un->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    un->resource_id = old_res;
    gpu_cmd(sizeof(*un), sizeof(gpu_hdr_t), VIRTIO_GPU_RESP_OK_NODATA);

    fb_puts("[virtio-gpu] mode set ");
    put_dec(g_w); fb_puts("x"); put_dec(g_h); fb_puts("\n");
    virtio_gpu_flush(0, 0, (int)g_w, (int)g_h);
    return 0;
}
