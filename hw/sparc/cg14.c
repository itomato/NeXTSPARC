/*
 * QEMU CG14 Frame buffer
 *
 * Copyright (c) 2010 Bob Breuer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to 
deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ui/console.h"
#include "ui/sysbus.h"

#ifdef DEBUG
#define DPRINTF(fmt, ...)                                       \
    do { printf("CG14: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define CG14_INFO(fmt, ...)                                     \
    do { printf("CG14: " fmt , ## __VA_ARGS__); } while (0)
#define CG14_ERROR(fmt, ...)                                    \
    do { printf("CG14: " fmt , ## __VA_ARGS__); } while (0)

/*
 * A[28:26] = slot number (4 to 7)
 * regs: size   0x10000 @ 0x09c000000  (0x80000000 + slot * 64M)
 * vmem: size upto 16MB @ 0x0fc000000  (0xE0000000 + slot * 64M)
 */

/*
 * memory map:
 * reg+0x0000 = control registers
 * reg+0x1000 = cursor registers
 * reg+0x2000 = dac registers (ADV7152)
 * reg+0x3000 = xlut
 * reg+0x4000 = clut1
 * reg+0x5000 = clut2
 * reg+0x6000 = clut3 (if implemented)
 *
 * mem+0x0000000 = XBGR (01234567)
 * mem+0x1000000 = BGR  (.123.567)
 * mem+0x2000000 = X16  (0246)
 * mem+0x2800000 = C16  (1357)
 * mem+0x3000000 = X32  (04)
 * mem+0x3400000 = B32  (15)
 * mem+0x3800000 = G32  (26)
 * mem+0x3c00000 = R32  (37)
 */

#define CG14_REG_SIZE         0x10000
#define CG14_VMEM_SLOTSIZE    (64<<20)

#define CG14_MONID_1024x768   0
#define CG14_MONID_1600x1280  1
#define CG14_MONID_1280x1024  2
#define CG14_MONID_1152x900   7

#define CG14_MONID_DEFAULT    CG14_MONID_1024x768

#define MCR_PIXMODE_MASK  0x30
#define   MCR_PIXMODE_8     0x00
#define   MCR_PIXMODE_16    0x20  /* 8+8 (X16,C16) */
#define   MCR_PIXMODE_32    0x30  /* XBGR */


struct ADV7152_state {
    uint8_t mode;
    uint8_t address;
    int rgb_seq;
};

typedef struct CG14State {
    SysBusDevice busdev;
    DisplayState *ds;

    uint8_t *vram;
    uint32_t vram_amask;
    int width, height;
    int dirty, size_changed;
    struct {
        uint8_t mcr;
        uint8_t ppr;
    } ctrl;
    struct ADV7152_state dac;
    struct {
        uint16_t hblank_start;
        uint16_t hblank_clear;
        uint16_t vblank_start;
        uint16_t vblank_clear;
    } timing;
    uint8_t xlut[256];
    uint32_t clut1[256];
    uint32_t clut2[256];
} CG14State;

static void cg14_screen_dump(void *opaque, const char *filename);
static void cg14_invalidate_display(void *opaque);

static inline uint32_t bgr_to_rgb(uint32_t bgr)
{
    uint32_t rgb;

    /* swap r & b */
    rgb = (bgr & 0x00FF00)
        | (bgr & 0x0000FF) << 16
        | (bgr & 0xFF0000) >> 16;
    return rgb;
}

static void cg14_draw_line32(const CG14State *s, void *dst, const uint8_t 
*src, int pixmode, int is_bgr)
{
    int i;
    int x, r, g, b;
    uint8_t xlut_val;
    uint32_t dval;
    uint32_t abgr;

    xlut_val = s->ctrl.ppr;

    for (i=0; i<s->width; i++) {
        x = *src++;
        if (pixmode == 8) {
            b = x;
        } else {
            b = *src++;
            xlut_val = s->xlut[x];
        }
        if (pixmode != 32) {
            r = g = b;
        } else {
            g = *src++;
            r = *src++;
        }
        if (xlut_val == 0) {
            abgr = b << 16 | g << 8 | r;
        } else if (xlut_val == 0x40) {
            abgr = s->clut1[x];
        } else {
            abgr = 0;
        }
        /* dac lookup ? */

        /* to surface format */
        dval = is_bgr ? (abgr & 0xFFFFFF) : bgr_to_rgb(abgr);
        ((uint32_t*)dst)[i] = dval;
    }
}

static void cg14_update_display(void *opaque)
{
    CG14State *s = opaque;
    int h, pixmode;
    uint8_t *pix;
    uint8_t *data;
    int new_width, new_height;

    if (s->size_changed) {
        new_width = 4 * (s->timing.hblank_start - s->timing.hblank_clear);
        new_height = s->timing.vblank_start - s->timing.vblank_clear;
        s->size_changed = 0;
        if ((new_width != s->width || new_height != s->height) && new_width > 
0 && new_height > 0) {
            s->width = new_width;
            s->height = new_height;
            CG14_INFO("new resolution = %d x %d\n", new_width, new_height);
            qemu_console_resize(s->ds, s->width, s->height);
            s->dirty = 1;
        }
    }

    if (!s->dirty || !s->width || !s->height) {
        return;
    }

    if (ds_get_bits_per_pixel(s->ds) != 32) {
        CG14_ERROR("cg14_update: FIXME: bpp (%d) != 32, linesize %d\n",
            ds_get_bits_per_pixel(s->ds), ds_get_linesize(s->ds));
        return;
    }

    switch (s->ctrl.mcr & MCR_PIXMODE_MASK) {
    case MCR_PIXMODE_32:
        pixmode = 32;
        break;
    case MCR_PIXMODE_16:
        pixmode = 16;
        break;
    case MCR_PIXMODE_8:
    default:
        pixmode = 8;
        break;
    }

    pix = s->vram;
    data = ds_get_data(s->ds);

    for (h=0; h<s->height; h++) {
        cg14_draw_line32(s, data, pix, pixmode, 
is_surface_bgr(s->ds->surface));
        pix += s->width * (pixmode / 8);
        data += ds_get_linesize(s->ds);
    }
    dpy_update(s->ds, 0, 0, s->width, s->height);
    s->dirty = 0;
}

static void cg14_invalidate_display(void *opaque)
{
    CG14State *s = opaque;

    s->dirty = 1;
}

static void ADV7152_write(struct ADV7152_state *s, unsigned int reg, unsigned 
int val)
{
    switch (reg) {
    case 0: /* address register */
        DPRINTF("ADV7152 Write address %02x\n", val);
        s->address = val;
        s->rgb_seq = 0;
        break;
    case 1: /* look up table */
        DPRINTF("ADV7152 Write %02x to lookup table\n", val);
        s->rgb_seq++;
        break;
    case 2: /* control registers */
        DPRINTF("ADV7152 Write %02x to control reg %d\n", val, s->address);
        switch (s->address) {
        default:
            break;
        }
        break;
    case 3: /* mode register */
        CG14_INFO("ADV7152 Write mode %02x (%d bit DAC, %d bit bus)\n",
            val, (val & 2) ? 10 : 8, (val & 4) ? 10 : 8);
        if (!val & 0x01) {
            // reset the dac
            s->rgb_seq = 0;
        }
        s->mode = val;
        break;
    }
}

static uint32_t cg14_reg_readb(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    unsigned int val;

    switch (addr & 0xffff) {
    case 0x0000:
        val = s->ctrl.mcr;
        break;
    case 0x0001:
        val = s->ctrl.ppr;
        break;
    case 0x0004: /* status ? */
        /* monitor code in bits 1..3 */
        val = CG14_MONID_DEFAULT << 1;
        break;
    case 0x0006: /* hw version */
        //val = 0x00; /* old version */
        val = 0x30;
        break;
    default:
        val = 0;
        break;
    }
    CG14_INFO("readb %02x from reg %x\n", val, (int)addr);

    return val;
}

static void cg14_reg_writeb(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;
    uint32_t i;

    if ((addr & 0xfcff) == 0x2000) {
        i = (addr & 0x300) >> 8;
        ADV7152_write(&s->dac, i, val);
        return;
    }
    if ((addr & 0xff00) == 0x3000) {
        /* xlut */
        i = addr & 0xff;
        if (s->xlut[i] != val) {
            s->dirty = 1;
            s->xlut[i] = val;
            if (val && val != 0x40)
                CG14_ERROR("writeb xlut[%d] = %02x\n", i, val);
        }
        return;
    }

    s->dirty = 1;

    switch (addr & 0xffff) {
    case 0x0000:
        s->ctrl.mcr = val;
        break;
    case 0x0001:
        s->ctrl.ppr = val & 0xF0;
        break;
    case 0x0007:
        /* clock control (ICS1562AM-001) */
        DPRINTF("write %02x to clock control\n", val);
        break;
    default:
        CG14_ERROR("writeb %02x to reg %x\n", val, (int)addr);
        break;
    }
}

static uint32_t cg14_reg_readw(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    unsigned int val;

    switch (addr & 0xffff) {
    case 0x0018:
        val = s->timing.hblank_start;
        break;
    case 0x001a:
        val = s->timing.hblank_clear;
        break;
    case 0x0022:
        val = s->timing.vblank_start;
        break;
    case 0x0024:
        val = s->timing.vblank_clear;
        break;
    default:
        val = 0;
        break;
    }
    CG14_INFO("readw 0x%08x from reg %x\n", val, (int)addr);

    return val;
}

static void cg14_reg_writew(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;

    CG14_INFO("writew %04x to reg %x\n", val, (int)addr);

    /* timing registers are 16bit */

    switch (addr & 0xffff) {
    case 0x0018:
        s->timing.hblank_start = val;
        break;
    case 0x001a:
        s->timing.hblank_clear = val;
       s->size_changed = 1;
        break;
    case 0x0022:
        s->timing.vblank_start = val;
        break;
    case 0x0024:
        s->timing.vblank_clear = val;
       s->size_changed = 1;
        break;
    case 0x001c: /* hsync_start */
    case 0x001e: /* hsync_clear */
    case 0x0020: /* csync_clear */
    case 0x0026: /* vsync_start */
    case 0x0028: /* vsync_clear */
    default:
        break;
    }
}

static uint32_t cg14_reg_readl(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    uint32_t val;
    uint32_t i;

    i = (addr & 0x3ff) >> 2;
    switch (addr & 0xfc00) {
    case 0x4000:
        val = s->clut1[i];
        break;
    case 0x5000:
        val = s->clut2[i];
        break;
    default:
        val = 0;
        CG14_ERROR("readl %08x from reg %x\n", val, (int)addr);
        break;
    }

    return val;
}

static void cg14_reg_writel(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;
    uint32_t i;

    s->dirty = 1;

    i = (addr & 0x3ff) >> 2;
    switch (addr & 0xfc00) {
    case 0x4000:
        s->clut1[i] = val;
        break;
    case 0x5000:
        s->clut2[i] = val;
        break;
    default:
        CG14_ERROR("writel %08x to reg %x\n", val, (int)addr);
        break;
    }
}

static CPUReadMemoryFunc *cg14_reg_read[3] = {
    cg14_reg_readb,
    cg14_reg_readw,
    cg14_reg_readl,
};

static CPUWriteMemoryFunc *cg14_reg_write[3] = {
    cg14_reg_writeb,
    cg14_reg_writew,
    cg14_reg_writel,
};

static uint32_t cg14_vram_readb(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    uint32_t offset;
    uint32_t val = 0;

    switch (addr & 0x3000000) {
    case 0x0000000:
        offset = addr & s->vram_amask;
        val =  ldub_p(s->vram+offset);
        break;
    case 0x1000000:
        offset = addr & s->vram_amask;
        val = 0; // FIXME
        break;
    case 0x2000000:
        offset = ((addr << 1) & s->vram_amask) + ((addr >> 23) & 1);
        val =  ldub_p(s->vram+offset);
        break;
    case 0x3000000:
        offset = ((addr << 2) & s->vram_amask) + ((addr >> 22) & 3);
        val =  ldub_p(s->vram+offset);
        break;
    }
    CG14_INFO("readb %02x from vram %x\n", val, (int)addr);

    return val;
}

static void cg14_vram_writeb(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;
    uint32_t offset;

    switch (addr & 0x3000000) {
    case 0x0000000:
        offset = addr & s->vram_amask;
        stb_p(s->vram+offset, val);
        if (offset < 4 * s->width * s->height) {
            s->dirty = 1;
        }
        break;
    default:
        CG14_ERROR("writeb %02x to vram %x\n", val, (int)addr);
        break;
    }
}

static uint32_t cg14_vram_readw(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    uint32_t offset;
    uint32_t val;

    switch (addr & 0x3000000) {
    default:
        offset = addr & s->vram_amask;
        val = 0;
        break;
    }
    CG14_ERROR("readw %04x from vram %x\n", val, (int)addr);

    return val;
}

static void cg14_vram_writew(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;

    CG14_ERROR("writew %04x to vram %x\n", val, (int)addr);

    s->dirty = 1;

    switch (addr & 0x3000000) {
    default:
        break;
    }
}

static uint32_t cg14_vram_readl(void *opaque, uint64_t addr)
{
    CG14State *s = opaque;
    uint32_t offset;
    uint32_t val = 0;

    switch (addr & 0x3000000) {
    case 0x0000000:
        offset = addr & s->vram_amask;
        val = ldl_be_p(s->vram+offset);
        break;
    case 0x1000000:
    case 0x2000000:
    case 0x3000000:
        CG14_ERROR("readl %08x from vram %x\n", val, (int)addr);
        break;
    }

    return val;
}

static void cg14_vram_writel(void *opaque, uint64_t addr, uint32_t 
val)
{
    CG14State *s = opaque;
    uint32_t offset;

    switch (addr & 0x3000000) {
    case 0x0000000:
        offset = addr & s->vram_amask;
        stl_be_p(s->vram+offset, val);
        if (offset < 4 * s->width * s->height) {
            s->dirty = 1;
        }
        break;
    case 0x1000000:
    case 0x2000000:
    case 0x3000000:
        CG14_ERROR("writel %08x to vram %x\n", val, (int)addr);
        break;
    }
}

static CPUReadMemoryFunc *cg14_vram_read[3] = {
    cg14_vram_readb,
    cg14_vram_readw,
    cg14_vram_readl,
};

static CPUWriteMemoryFunc *cg14_vram_write[3] = {
    cg14_vram_writeb,
    cg14_vram_writew,
    cg14_vram_writel,
};


/******** SX *********/

static uint32_t sx_reg_readb(void *opaque, uint64_t addr)
{
    //CG14State *s = opaque;
    int val;

    printf("SX readb reg " TARGET_FMT_plx "\n", addr);

    switch (addr & 0xffff) {
    default:
        val = 0;
        break;
    }
    return val;
}

static void sx_reg_writeb(void *opaque, uint64_t addr, uint32_t val)
{
    //CG14State *s = opaque;

    printf("SX writeb %02x to reg " TARGET_FMT_plx "\n", val, addr);

    switch (addr & 0xffff) {
    default:
        break;
    }
}

static uint32_t sx_reg_readw(void *opaque, uint64_t addr)
{
    //CG14State *s = opaque;
    int val;

    printf("SX readw reg " TARGET_FMT_plx "\n", addr);

    switch (addr & 0xffff) {
    default:
        val = 0;
        break;
    }
    return val;
}

static void sx_reg_writew(void *opaque, uint64_t addr, uint32_t val)
{
    //CG14State *s = opaque;

    printf("SX writew %04x to reg " TARGET_FMT_plx "\n", val, addr);

    switch (addr & 0xffff) {
    default:
        break;
    }
}

static uint32_t sx_reg_readl(void *opaque, uint64_t addr)
{
    //CG14State *s = opaque;
    int val;

    printf("SX readl reg " TARGET_FMT_plx "\n", addr);

    switch (addr & 0xffff) {
    default:
        val = 0;
        break;
    }
    return val;
}

static void sx_reg_writel(void *opaque, uint64_t addr, uint32_t val)
{
    //CG14State *s = opaque;

    printf("SX writel %08x to reg " TARGET_FMT_plx "\n", val, addr);

    switch (addr & 0xffff) {
    default:
        break;
    }
}

static CPUReadMemoryFunc *sx_reg_read[3] = {
    sx_reg_readb,
    sx_reg_readw,
    sx_reg_readl,
};

static CPUWriteMemoryFunc *sx_reg_write[3] = {
    sx_reg_writeb,
    sx_reg_writew,
    sx_reg_writel,
};

/*********************/

static uint32_t bad_mem_read(void *opaque, uint64_t addr)
{
    printf("Bad read from " TARGET_FMT_plx "\n", addr);
    //cpu_abort(cpu_single_env, "bad ram read access at " TARGET_FMT_plx "\n", 
addr);
    return 0;
}
static void bad_mem_write(void *opaque, uint64_t addr, uint32_t val)
{
    printf("Bad write of 0x%02x to " TARGET_FMT_plx "\n", val, addr);
    //cpu_abort(cpu_single_env, "bad ram write access at " TARGET_FMT_plx 
"\n", addr);
}
static CPUReadMemoryFunc *bad_memr[3] = { bad_mem_read, bad_mem_read, 
bad_mem_read };
static CPUWriteMemoryFunc *bad_memw[3] = { bad_mem_write, bad_mem_write, 
bad_mem_write };

void cg14_init(uint64_t ctrl_base, uint64_t vram_base,
                uint32_t vram_size)
{
//    DeviceState *dev;
//    SysBusDevice *s;

//    dev = qdev_create(NULL, "SUNW,cg14");
//    qdev_init(dev);
//    s = sysbus_from_qdev(dev);
//}

//static void cg14_init1(SysBusDevice *dev)
//{
    CG14State *s;// = FROM_SYSBUS(CG14State, dev);
    ram_addr_t vram_offset;
    uint8_t *vram;
    int ctrl_memory, vram_memory;
    int sx_registers;
    int bad_mem;

    s = qemu_mallocz(sizeof(CG14State));

    vram_offset = qemu_ram_alloc(vram_size);
    vram = qemu_get_ram_ptr(vram_offset);

    s->vram = vram;
    s->vram_amask = vram_size - 1;

    ctrl_memory = cpu_register_io_memory(cg14_reg_read, cg14_reg_write, s);
    cpu_register_physical_memory_offset(ctrl_base, CG14_REG_SIZE, ctrl_memory, 
ctrl_base);

    vram_memory = cpu_register_io_memory(cg14_vram_read, cg14_vram_write, s);
    cpu_register_physical_memory_offset(vram_base, CG14_VMEM_SLOTSIZE, 
vram_memory, vram_base);

    s->ds = graphic_console_init(cg14_update_display,
                                 cg14_invalidate_display,
                                 cg14_screen_dump, NULL, s);

    s->width = 640;
    s->height = 480;
    qemu_console_resize(s->ds, s->width, s->height);

    /* SX or SPAM (Sun Pixel Arithmetic Memory) */
    sx_registers = cpu_register_io_memory(sx_reg_read, sx_reg_write, s);
    cpu_register_physical_memory(0xf80000000ULL, 0x2000, sx_registers);

    bad_mem = cpu_register_io_memory(bad_memr, bad_memw, s);
    /* missing vsimms */
    cpu_register_physical_memory_offset(0x90000000, 0x2000, bad_mem, 
0x90000000);
    cpu_register_physical_memory_offset(0x94000000, 0x2000, bad_mem, 
0x94000000);
    cpu_register_physical_memory_offset(0x98000000, 0x2000, bad_mem, 
0x98000000);
    /* DBRI (audio) */
    cpu_register_physical_memory_offset(0xEE0001000ULL, 0x10000, bad_mem, 
0xE0001000);
}

/* save to file */
static void cg14_screen_dump(void *opaque, const char *filename)
{
    CG14State *s = opaque;
    FILE *f;
    int y, pixmode, linesize;
    void *buf;
    uint8_t *pix;

    f = fopen(filename, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n%d\n", s->width, s->height, 255);

    linesize = s->width * 3;
    buf = qemu_mallocz(linesize);
    pix = s->vram;

    switch (s->ctrl.mcr & MCR_PIXMODE_MASK) {
    case MCR_PIXMODE_32:
        pixmode = 32;
        break;
    case MCR_PIXMODE_16:
        pixmode = 16;
        break;
    case MCR_PIXMODE_8:
    default:
        pixmode = 8;
        break;
    }

    for (y=0; y<s->height; y++) {
        // cg14_draw_line24_bgr(s, buf, pix, pixmode);
        fwrite(buf, 1, linesize, f);
        pix += s->width * (pixmode / 8);
    }

    qemu_free(buf);
    fclose(f);
}
