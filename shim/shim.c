/*
 * The Tensor G5's boot shim: xnuloader started without a UEFI.
 *
 * There is no UEFI for the Pixel 10 (Project Silicium has no Google SoC, nobody else has one) and xnuloader is a UEFI
 * application that uses few of UEFI's services. This offers those few, over what Google's boot loader (ABL) hands a
 * kernel: its device tree. start.S has set EL2 up and dropped to EL1; this, at EL1:
 *
 *   1. reads the device tree (fdt.c): the RAM, the reservations, the command line, a UART, a framebuffer;
 *   2. maps the address space 1:1 (RAM cacheable in 2 MB blocks, everything else as a device's) and turns the MMU on;
 *   3. on the Tensor G5: stops the two watchdogs the boot loader may have left running, and finds its framebuffer
 *      (scion.fb= on the command line, a simple-framebuffer node, or the display controller's layer 0);
 *   4. copies the "XNUP" payload that follows the shim in the boot image to 0xF0000000, where the loader looks;
 *   5. loads the payload's loader.efi (PE32+, relocated) and calls it with a system table that has: boot services
 *      (pages and pool from the free RAM, the memory map, the framebuffer as Graphics Output, stall), runtime services
 *      (reset by PSCI), a console on the UART, and the boot loader's device tree as a configuration table.
 *
 * What the loader asks for that is not here (block I/O, files, events, USB) is answered "not found" or "unsupported",
 * and the loader goes on without it. The services stay while the kernel runs: the loader keeps them, as it does UEFI's.
 *
 * Sources, facts only: the UEFI specification 2.10 (the tables' layouts, as xnuloader/efi.h declares them; PE32+ and its
 * base relocations: the Microsoft PE format specification); the Arm ARM (DDI 0487: VMSAv8-64, 4 KB granule); PSCI
 * (DEN 0022: SYSTEM_RESET 0x84000009, SYSTEM_OFF 0x84000008). Tensor G5 addresses: the mainline device tree
 * (arch/arm64/boot/dts/google/lga.dtsi) and Google's kernel for the phone (README.md in this repository names the files).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "efi.h"
#include "fdt.h"
#include "font8x8.h"
#include "memmap.h"

#define PAYLOAD_BASE    0xF0000000ULL   /* xnuloader/loader.c: PAYLOAD_BASE, PAYLOAD_MAX_SIZE */
#define PAYLOAD_MAX     0x04000000ULL
#define PAYLOAD_MAGIC   0x50554E58u     /* "XNUP" */

#define EFI_SUCCESS             0
#define EFI_INVALID_PARAMETER   (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED         (EFI_ERROR_BIT | 3)
#define EFI_OUT_OF_RESOURCES    (EFI_ERROR_BIT | 9)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)

#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u

/* Tensor G5 (google,lga): the two application-core watchdogs (Google's kernel: lga-base.dtsi watchdog_cl0/cl1, google_wdt) */
#define LGA_WDT0                0x200C4000ULL
#define LGA_WDT1                0x200C5000ULL
#define WDT_ID                  0x00            /* 0x574454, "WDT" */
#define WDT_CONTROL             0x08            /* bit 0 enable */
#define WDT_KEY                 0x10            /* writes are taken only after the unlock key */
#define WDT_KEY_UNLOCK          0x00C0FFEE      /* the tree's key-unlock */
/* Tensor G5: the display controller (VeriSilicon DC9x00 at sswrp_dpu 0x0EC00000 + 0x200000) and its SMMUv3 */
#define LGA_DPU                 0x0EE00000ULL
#define DPU_L0_ADDRESS          0x01802C        /* layer 0's shadow registers (Google's vs_dc_reg_fe0.h) */
#define DPU_L0_HIGH_ADDRESS     0x018030        /* bits 7:0 */
#define DPU_L0_STRIDE           0x018044        /* bits 17:0 */
#define DPU_L0_SIZE             0x01805C        /* width 15:0, height 31:16 */
#define DPU_L0_CONFIG           0x018088        /* enable 0:0, format 29:24 (0: A8R8G8B8) */
#define DPU_LAYER_STEP          0x004000        /* layer 1 at 0x01C088 ... */
#define LGA_DPU_SMMU            0x0ED80000ULL
#define SMMU_CR0                0x20            /* bit 0 SMMUEN: 0, the DPU reads physical addresses */

extern uint64_t shim_entry_el;
extern uint64_t shim_el2_fault[4];
extern char _head[], __image_end[];

static struct fdt_info FI;
static struct memmap MM;
static const void *FDT;
static uint64_t image_base, image_bytes;        /* the shim and its payload, where the boot loader put them */

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }

/* ------------------------------------------------------------------------ */
/* Small helpers (freestanding: the compiler may call memcpy and memset)      */

void *
memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n--) {
		*p++ = (uint8_t)c;
	}
	return d;
}

void *
memcpy(void *d, const void *s, size_t n)
{
	uint8_t *p = d;
	const uint8_t *q = s;
	if ((((uintptr_t)p | (uintptr_t)q | n) & 7) == 0) {
		for (; n; n -= 8, p += 8, q += 8) {
			*(uint64_t *)p = *(const uint64_t *)q;
		}
		return d;
	}
	while (n--) {
		*p++ = *q++;
	}
	return d;
}

static int
memeq(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	while (n--) {
		if (*x++ != *y++) {
			return 0;
		}
	}
	return 1;
}

static uint32_t
slen_(const char *s)
{
	uint32_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
starts(const char *s, const char *p)
{
	while (*p) {
		if (*s++ != *p++) {
			return 0;
		}
	}
	return 1;
}

static uint64_t
parse_num(const char **sp)
{
	const char *s = *sp;
	uint64_t v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16, s += 2;
	}
	for (;; s++) {
		int d;
		if (*s >= '0' && *s <= '9') {
			d = *s - '0';
		} else if (base == 16 && *s >= 'a' && *s <= 'f') {
			d = *s - 'a' + 10;
		} else if (base == 16 && *s >= 'A' && *s <= 'F') {
			d = *s - 'A' + 10;
		} else {
			break;
		}
		v = v * (uint64_t)base + (uint64_t)d;
	}
	*sp = s;
	return v;
}

/* the value of `key` (e.g. "scion.fb=") among the boot loader's command line's words, or NULL */
static const char *
bootarg(const char *key)
{
	const char *s = FI.bootargs;
	for (; s && *s; s++) {
		if ((s == FI.bootargs || s[-1] == ' ') && starts(s, key)) {
			return s + slen_(key);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Console: the UART the tree names (a PL011 under QEMU; the Pixel's 16550 only when the boot loader enabled it)   */

static char logbuf[16384];
static uint32_t loglen;
static void screen_putc(char c);

static void
putc_(char c)
{
	if (loglen < sizeof(logbuf)) {
		logbuf[loglen++] = c;
	}
	screen_putc(c);
	if (FI.uart.kind == UART_PL011) {
		for (int i = 0; i < 100000 && (rd32(FI.uart.base + 0x18) & (1u << 5)); i++) {
		}
		wr32(FI.uart.base, (uint8_t)c);
	} else if (FI.uart.kind == UART_16550) {
		uint64_t lsr = FI.uart.base + (5ULL << FI.uart.shift);
		for (int i = 0; i < 100000 && !(rd32(lsr) & (1u << 5)); i++) {
		}
		wr32(FI.uart.base, (uint8_t)c);
	}
}

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (const char *f = fmt; *f; f++) {
		if (*f != '%') {
			if (*f == '\n') {
				putc_('\r');
			}
			putc_(*f);
			continue;
		}
		f++;
		if (*f == 's') {
			const char *s = va_arg(ap, const char *);
			for (s = s ? s : "(null)"; *s; s++) {
				putc_(*s);
			}
		} else if (*f == 'x') {
			uint64_t v = va_arg(ap, uint64_t);
			char b[17];
			int n = 0;
			do {
				b[n++] = "0123456789abcdef"[v & 15];
				v >>= 4;
			} while (v);
			putc_('0'), putc_('x');
			while (n) {
				putc_(b[--n]);
			}
		} else if (*f == 'd') {
			uint64_t v = va_arg(ap, uint64_t);
			char b[21];
			int n = 0;
			do {
				b[n++] = (char)('0' + v % 10);
				v /= 10;
			} while (v);
			while (n) {
				putc_(b[--n]);
			}
		} else {
			putc_(*f);
		}
	}
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* PSCI: by HVC to the shim's EL2, which passes it on; entered at EL1, the way the device tree's /psci says (QEMU: HVC) */

static int64_t
psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	if (shim_entry_el != 2 && FI.psci_smc) {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	} else {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	}
	return (int64_t)x0;
}

static void __attribute__((noreturn))
halt(const char *why)
{
	say("shim: %s: restarting in 10 s\n", why);
	uint64_t f = 0, t0 = 0, t;
	__asm__ volatile ("mrs %0, cntfrq_el0; mrs %1, cntpct_el0" : "=r" (f), "=r" (t0));
	do {
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r" (t));
	} while (f != 0 && t - t0 < 10 * f);
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) {
		__asm__ volatile ("wfe");
	}
}

/* ------------------------------------------------------------------------ */
/* The MMU: 1:1, 4 KB granule, 48-bit addresses (level 0 .. level 2 blocks)  */

#define MAIR_DEVICE_nGnRnE      0       /* attribute index 0: 0x00 */
#define MAIR_NORMAL             1       /* 1: 0xFF, write-back, read and write allocate */
#define MAIR_DEVICE_nGnRE       2       /* 2: 0x04 */
#define MAIR_NORMAL_NC          3       /* 3: 0x44, normal, not cached: the framebuffer, as UEFI maps a GOP's */
#define PTE_VALID               (1ULL << 0)
#define PTE_TABLE               (1ULL << 1)
#define PTE_AF                  (1ULL << 10)
#define PTE_SH_INNER            (3ULL << 8)
#define PTE_PXN                 (1ULL << 53)
#define PTE_UXN                 (1ULL << 54)
#define PTE_ATTR(i)             ((uint64_t)(i) << 2)
#define L2_TABLES               64      /* 64 GB of address space that holds RAM, in 2 MB blocks */

static uint64_t l0[512] __attribute__((aligned(4096)));
static uint64_t l1[2][512] __attribute__((aligned(4096)));     /* 0-512 GB, 512 GB-1 TB */
static uint64_t l2[L2_TABLES][512] __attribute__((aligned(4096)));
static int l2_used;

static void inval_dcache(uint64_t start, uint64_t end);

static uint64_t
block_desc(uint64_t pa, int kind)
{
	if (kind == BLOCK_NORMAL) {
		return pa | PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_NORMAL);
	}
	if (kind == BLOCK_NOMAP) {
		return pa | PTE_VALID | PTE_AF | PTE_ATTR(MAIR_DEVICE_nGnRnE) | PTE_PXN | PTE_UXN;
	}
	if (kind == BLOCK_FB) {
		return pa | PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_NORMAL_NC) | PTE_PXN | PTE_UXN;
	}
	return pa | PTE_VALID | PTE_AF | PTE_ATTR(MAIR_DEVICE_nGnRE) | PTE_PXN | PTE_UXN;
}

static int
mmu_on(void)
{
	for (int t = 0; t < 2; t++) {
		l0[t] = (uint64_t)(uintptr_t)l1[t] | PTE_VALID | PTE_TABLE;
		for (int i = 0; i < 512; i++) {
			uint64_t gb = ((uint64_t)t * 512 + (uint64_t)i) << 30;
			int ram = 0;
			for (int r = 0; r < MM.nram; r++) {
				ram |= gb < MM.ram[r].base + MM.ram[r].size && MM.ram[r].base < gb + (1ULL << 30);
			}
			for (int r = 0; r < MM.nrsv; r++) {
				ram |= (MM.rsv[r].flags & RSV_FB) && gb < MM.rsv[r].base + MM.rsv[r].size &&
				    MM.rsv[r].base < gb + (1ULL << 30);
			}
			if (!ram) {
				l1[t][i] = block_desc(gb, BLOCK_NONE);      /* devices, or nothing: never cached */
				continue;
			}
			if (l2_used == L2_TABLES) {
				return -1;
			}
			uint64_t *tab = l2[l2_used++];
			for (int j = 0; j < 512; j++) {
				tab[j] = block_desc(gb + ((uint64_t)j << 21), memmap_block(&MM, gb + ((uint64_t)j << 21)));
			}
			l1[t][i] = (uint64_t)(uintptr_t)tab | PTE_VALID | PTE_TABLE;
		}
	}
	uint64_t mmfr0, pa;
	__asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r" (mmfr0));
	pa = mmfr0 & 7;
	if (pa > 5) {
		pa = 5;                 /* 48 bits: what 4 KB pages from level 0 address */
	}
	uint64_t mair = 0x00ULL | 0xFFULL << 8 | 0x04ULL << 16 | 0x44ULL << 24;
	inval_dcache((uint64_t)(uintptr_t)_head, (uint64_t)(uintptr_t)__image_end);
	uint64_t tcr = 16 /* T0SZ: 48 bits */ | 1ULL << 8 /* IRGN0 WBWA */ | 1ULL << 10 /* ORGN0 WBWA */ |
	    3ULL << 12 /* SH0 inner */ | 0ULL << 14 /* TG0 4 KB */ | 1ULL << 23 /* EPD1: no TTBR1 walks */ | pa << 32;
	__asm__ volatile (
		"dsb sy\n"
		"msr mair_el1, %0\n"
		"msr tcr_el1, %1\n"
		"msr ttbr0_el1, %2\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		"mrs x9, sctlr_el1\n"
		"orr x9, x9, #(1 << 0)\n"       /* M */
		"orr x9, x9, #(1 << 2)\n"       /* C */
		"orr x9, x9, #(1 << 12)\n"      /* I */
		"bic x9, x9, #(1 << 1)\n"       /* A: no alignment faults */
		"bic x9, x9, #(1 << 19)\n"      /* WXN */
		"msr sctlr_el1, x9\n"
		"isb\n"
		: : "r" (mair), "r" (tcr), "r" ((uint64_t)(uintptr_t)l0) : "x9", "memory");
	return 0;
}

static uint64_t
dline(void)
{
	uint64_t ctr;
	__asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
	uint64_t d = 4ULL << ((ctr >> 16) & 15), i = 4ULL << (ctr & 15);
	return d < i ? d : i;
}

static void
sync_icache(uint64_t start, uint64_t end)
{
	uint64_t line = dline();
	for (uint64_t a = start & ~(line - 1); a < end; a += line) {
		__asm__ volatile ("dc cvau, %0" : : "r" (a) : "memory");
	}
	__asm__ volatile ("dsb ish; ic iallu; dsb ish; isb" : : : "memory");
}

/*
 * Before the caches are on: what the shim has written so far went to memory, past them; lines the boot loader left in
 * them for the same addresses (clean: it cleaned the image it loaded) would be read instead. Invalidated, not cleaned.
 */
static void
inval_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = dline();
	for (uint64_t a = start & ~(line - 1); a < end; a += line) {
		__asm__ volatile ("dc ivac, %0" : : "r" (a) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/* ------------------------------------------------------------------------ */
/* UEFI memory: the map, pages, pool                                         */

#define MAX_DESC 256
static efi_memory_descriptor DESC[MAX_DESC];
static int ndesc;
static uint64_t map_key = 1;

enum { EfiReservedMemoryType = 0, EfiLoaderCode = 1, EfiBootServicesData = 4, EfiRuntimeServicesData = 6 };

static int
desc_insert(int at, uint32_t type, uint64_t base, uint64_t pages)
{
	if (ndesc >= MAX_DESC) {
		return -1;
	}
	for (int i = ndesc; i > at; i--) {
		DESC[i] = DESC[i - 1];
	}
	memset(&DESC[at], 0, sizeof(DESC[at]));
	DESC[at].type = type;
	DESC[at].phys_start = base;
	DESC[at].virt_start = base;
	DESC[at].pages = pages;
	DESC[at].attribute = 0xF;               /* UC | WC | WT | WB */
	ndesc++;
	return 0;
}

/* carve [base, base + pages) out of conventional descriptor i, as `type` */
static int
desc_take(int i, uint64_t base, uint64_t pages, uint32_t type)
{
	efi_memory_descriptor d = DESC[i];
	uint64_t end = base + pages * 4096, dend = d.phys_start + d.pages * 4096;

	if (ndesc + 2 > MAX_DESC) {
		return -1;
	}
	DESC[i].phys_start = base;
	DESC[i].virt_start = base;
	DESC[i].pages = pages;
	DESC[i].type = type;
	if (end < dend) {
		desc_insert(i + 1, EfiConventionalMemory, end, (dend - end) / 4096);
	}
	if (base > d.phys_start) {
		desc_insert(i, EfiConventionalMemory, d.phys_start, (base - d.phys_start) / 4096);
	}
	map_key++;
	return 0;
}

static void
desc_merge(void)
{
	for (int i = 0; i + 1 < ndesc;) {
		if (DESC[i].type == EfiConventionalMemory && DESC[i + 1].type == EfiConventionalMemory &&
		    DESC[i].phys_start + DESC[i].pages * 4096 == DESC[i + 1].phys_start) {
			DESC[i].pages += DESC[i + 1].pages;
			for (int j = i + 1; j + 1 < ndesc; j++) {
				DESC[j] = DESC[j + 1];
			}
			ndesc--;
		} else {
			i++;
		}
	}
}

static efi_status EFIAPI
efi_allocate_pages(int type, int mem_type, uint64_t pages, uint64_t *addr)
{
	if (addr == NULL || pages == 0) {
		return EFI_INVALID_PARAMETER;
	}
	if (type == AllocateAddress) {
		uint64_t base = *addr, end = base + pages * 4096;
		if (base & 4095) {
			return EFI_INVALID_PARAMETER;
		}
		for (int i = 0; i < ndesc; i++) {
			uint64_t ds = DESC[i].phys_start, de = ds + DESC[i].pages * 4096;
			if (DESC[i].type == EfiConventionalMemory && base >= ds && end <= de) {
				return desc_take(i, base, pages, (uint32_t)mem_type) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
			}
		}
		return EFI_NOT_FOUND;
	}
	/*
	 * Anywhere (or below *addr): from the top of the highest free block below 4 GB that has room, else of any; the
	 * loader takes XNU's window from the largest free block, which this leaves alone where it can (the Pixel's high bank).
	 */
	uint64_t limit = type == AllocateMaxAddress ? *addr + 1 : ~0ULL;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = ndesc - 1; i >= 0; i--) {
			uint64_t ds = DESC[i].phys_start, de = ds + DESC[i].pages * 4096;
			if (DESC[i].type != EfiConventionalMemory) {
				continue;
			}
			if (pass == 0 && ds >= 0x100000000ULL) {
				continue;
			}
			if (de > limit) {
				de = limit & ~4095ULL;
			}
			if (de <= ds || (de - ds) / 4096 < pages) {
				continue;
			}
			uint64_t base = de - pages * 4096;
			if (desc_take(i, base, pages, (uint32_t)mem_type)) {
				return EFI_OUT_OF_RESOURCES;
			}
			*addr = base;
			return EFI_SUCCESS;
		}
	}
	return EFI_OUT_OF_RESOURCES;
}

static efi_status EFIAPI
efi_free_pages(uint64_t addr, uint64_t pages)
{
	for (int i = 0; i < ndesc; i++) {
		if (DESC[i].phys_start == addr && DESC[i].pages == pages && DESC[i].type != EfiConventionalMemory &&
		    DESC[i].type != EfiReservedMemoryType) {
			DESC[i].type = EfiConventionalMemory;
			desc_merge();
			map_key++;
			return EFI_SUCCESS;
		}
	}
	return EFI_NOT_FOUND;
}

static efi_status EFIAPI
efi_get_memory_map(uint64_t *size, void *map, uint64_t *key, uint64_t *desc_size, uint32_t *desc_ver)
{
	uint64_t need = (uint64_t)ndesc * sizeof(efi_memory_descriptor);
	if (desc_size) {
		*desc_size = sizeof(efi_memory_descriptor);
	}
	if (desc_ver) {
		*desc_ver = 1;
	}
	if (key) {
		*key = map_key;
	}
	if (size == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	if (*size < need || map == NULL) {
		*size = need;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(map, DESC, need);
	*size = need;
	return EFI_SUCCESS;
}

/* pool: whole pages, with the count in front */
static efi_status EFIAPI
efi_allocate_pool(int type, uint64_t size, void **buf)
{
	uint64_t pages = (size + 16 + 4095) / 4096, addr = 0;
	if (buf == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	efi_status s = efi_allocate_pages(AllocateAnyPages, type, pages, &addr);
	if (EFI_ERROR(s)) {
		return s;
	}
	*(uint64_t *)(uintptr_t)addr = pages;
	*buf = (void *)(uintptr_t)(addr + 16);
	return EFI_SUCCESS;
}

static efi_status EFIAPI
efi_free_pool(void *buf)
{
	if (buf == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	uint64_t addr = (uint64_t)(uintptr_t)buf - 16;
	return efi_free_pages(addr, *(uint64_t *)(uintptr_t)addr);
}

/* ------------------------------------------------------------------------ */
/* The rest of the boot services, the runtime services, the console          */

static const efi_guid GOP_GUID = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const efi_guid LOADED_IMAGE_GUID = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const efi_guid DTB_GUID = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

static struct {
	uint32_t revision;
	efi_handle parent;
	efi_system_table *st;
	efi_handle device;
	void *file_path, *reserved;
	uint32_t load_options_size;
	void *load_options;
	void *image_base;
	uint64_t image_size;
	uint32_t code_type, data_type;
	void *unload;
} LOADED_IMAGE;
static int IMAGE_HANDLE;                /* the loader's handle: the address of this */

static efi_gop GOP;
static efi_gop_mode GOP_MODE;
static efi_gop_mode_info GOP_INFO;
static int have_fb;

static efi_status EFIAPI
efi_handle_protocol(efi_handle h, const efi_guid *guid, void **iface)
{
	if (h == &IMAGE_HANDLE && guid != NULL && memeq(guid, &LOADED_IMAGE_GUID, sizeof(*guid)) && iface != NULL) {
		*iface = &LOADED_IMAGE;
		return EFI_SUCCESS;
	}
	return EFI_UNSUPPORTED;
}

static efi_status EFIAPI
efi_locate_protocol(const efi_guid *guid, void *registration, void **iface)
{
	if (have_fb && guid != NULL && memeq(guid, &GOP_GUID, sizeof(*guid)) && iface != NULL) {
		*iface = &GOP;
		return EFI_SUCCESS;
	}
	return EFI_NOT_FOUND;
}

static efi_status EFIAPI
efi_locate_handle_buffer(int type, const efi_guid *guid, void *key, uint64_t *count, efi_handle **buf)
{
	if (count) {
		*count = 0;
	}
	return EFI_NOT_FOUND;
}

static efi_status EFIAPI
efi_stall(uint64_t us)
{
	uint64_t f, t0, t;
	__asm__ volatile ("mrs %0, cntfrq_el0; mrs %1, cntpct_el0" : "=r" (f), "=r" (t0));
	if (f == 0) {
		return EFI_SUCCESS;
	}
	do {
		__asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
	} while ((t - t0) * 1000000 / f < us);
	return EFI_SUCCESS;
}

static efi_status EFIAPI efi_ok(void) { return EFI_SUCCESS; }
static efi_status EFIAPI efi_unsupported(void) { return EFI_UNSUPPORTED; }
static efi_status EFIAPI efi_set_watchdog(uint64_t t, uint64_t c, uint64_t s, char16 *d) { return EFI_SUCCESS; }
static efi_status EFIAPI efi_exit_boot_services(efi_handle image, uint64_t key) { return EFI_SUCCESS; }
static efi_status EFIAPI efi_get_time(struct efi_time *t, void *caps) { return EFI_UNSUPPORTED; }
static efi_status EFIAPI efi_event_op(void *event) { return EFI_UNSUPPORTED; }
static efi_status EFIAPI
efi_create_event_ex(uint32_t type, uint64_t tpl, void (EFIAPI *notify)(void *event, void *ctx), const void *ctx,
    const efi_guid *group, void **event)
{
	return EFI_UNSUPPORTED;
}

static void EFIAPI
efi_reset_system(int type, efi_status status, uint64_t size, void *data)
{
	say("shim: reset (%d)\n", (uint64_t)type);
	psci(type == 2 /* EfiResetShutdown */ ? PSCI_SYSTEM_OFF : PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) {
		__asm__ volatile ("wfe");
	}
}

static efi_status EFIAPI
efi_output_string(struct efi_text_output *self, const char16 *s)
{
	for (; s && *s; s++) {
		putc_(*s < 0x80 ? (char)*s : '?');
	}
	return EFI_SUCCESS;
}

static efi_status EFIAPI
gop_query_mode(struct efi_gop *self, uint32_t mode, uint64_t *size, efi_gop_mode_info **info)
{
	if (mode != 0 || size == NULL || info == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	*size = sizeof(GOP_INFO);
	*info = &GOP_INFO;
	return EFI_SUCCESS;
}

static efi_status EFIAPI
gop_set_mode(struct efi_gop *self, uint32_t mode)
{
	return mode == 0 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static efi_boot_services BS;
static efi_runtime_services RT;
static efi_system_table ST;
static efi_text_output CON;
static struct { efi_guid guid; void *table; } TABLES[1];

static void
tables_init(void)
{
	BS.hdr.signature = 0x56524553544f4f42ULL;       /* "BOOTSERV" */
	BS.hdr.revision = (2 << 16) | 70;
	BS.hdr.header_size = sizeof(BS);
	BS.raise_tpl = BS.restore_tpl = (void *)efi_unsupported;
	BS.allocate_pages = efi_allocate_pages;
	BS.free_pages = efi_free_pages;
	BS.get_memory_map = efi_get_memory_map;
	BS.allocate_pool = efi_allocate_pool;
	BS.free_pool = efi_free_pool;
	BS.create_event = BS.set_timer = BS.wait_for_event = (void *)efi_unsupported;
	BS.signal_event = efi_event_op;
	BS.close_event = efi_event_op;
	BS.check_event = (void *)efi_unsupported;
	BS.install_protocol_interface = BS.reinstall_protocol_interface = BS.uninstall_protocol_interface =
	    (void *)efi_unsupported;
	BS.handle_protocol = efi_handle_protocol;
	BS.register_protocol_notify = BS.locate_handle = BS.locate_device_path = BS.install_configuration_table =
	    (void *)efi_unsupported;
	BS.load_image = BS.start_image = BS.exit = BS.unload_image = (void *)efi_unsupported;
	BS.exit_boot_services = efi_exit_boot_services;
	BS.get_next_monotonic_count = (void *)efi_unsupported;
	BS.stall = efi_stall;
	BS.set_watchdog_timer = efi_set_watchdog;
	BS.connect_controller = BS.disconnect_controller = BS.open_protocol = BS.close_protocol =
	    BS.open_protocol_information = BS.protocols_per_handle = (void *)efi_unsupported;
	BS.locate_handle_buffer = efi_locate_handle_buffer;
	BS.locate_protocol = efi_locate_protocol;
	BS.install_multiple_protocol_interfaces = BS.uninstall_multiple_protocol_interfaces = (void *)efi_unsupported;
	BS.calculate_crc32 = BS.copy_mem = BS.set_mem = (void *)efi_unsupported;
	BS.create_event_ex = efi_create_event_ex;

	RT.hdr.signature = 0x56524553544e5552ULL;       /* "RUNTSERV" */
	RT.hdr.revision = (2 << 16) | 70;
	RT.hdr.header_size = sizeof(RT);
	RT.get_time = efi_get_time;
	RT.set_time = RT.get_wakeup_time = RT.set_wakeup_time = RT.set_virtual_address_map = RT.convert_pointer =
	    RT.get_variable = RT.get_next_variable_name = RT.set_variable = RT.get_next_high_mono_count =
	    (void *)efi_unsupported;
	RT.reset_system = efi_reset_system;

	CON.reset = (void *)efi_ok;
	CON.output_string = efi_output_string;

	TABLES[0].guid = DTB_GUID;
	TABLES[0].table = (void *)FDT;

	ST.hdr.signature = 0x5453595320494249ULL;       /* "IBI SYST" */
	ST.hdr.revision = (2 << 16) | 70;
	ST.hdr.header_size = sizeof(ST);
	ST.vendor = (char16 *)u"Scion lga shim";
	ST.revision = 1;
	ST.con_out_handle = &CON;
	ST.con_out = &CON;
	ST.std_err = &CON;
	ST.rt = &RT;
	ST.bs = &BS;
	ST.nr_tables = 1;
	ST.tables = (void *)TABLES;

	GOP.query_mode = gop_query_mode;
	GOP.set_mode = gop_set_mode;
	GOP.mode = &GOP_MODE;
	GOP_MODE.max_mode = 1;
	GOP_MODE.mode = 0;
	GOP_MODE.info = &GOP_INFO;
	GOP_MODE.info_size = sizeof(GOP_INFO);
}

/* ------------------------------------------------------------------------ */
/* The Tensor G5's own: its watchdogs, its framebuffer                       */

static int
is_lga(void)
{
	return fdt_compat_has(FI.compatible, FI.compatible_len, "google,lga");
}

/*
 * The boot loader may leave either watchdog counting (on the Pixel 6a, PixOS found both armed): 24 s and 30 s in
 * Google's tree, then a restart. Each is stopped as Google's driver stops it (google_wdt.c's stop: the unlock key,
 * the enable bit cleared, the lock key), after its ID register says it is one. scion.wdt=keep leaves them be.
 */
static void
lga_watchdogs(void)
{
	static const uint64_t wdt[2] = { LGA_WDT0, LGA_WDT1 };
	const char *keep = bootarg("scion.wdt=");

	for (int i = 0; i < 2; i++) {
		uint32_t id = rd32(wdt[i] + WDT_ID), ctl = rd32(wdt[i] + WDT_CONTROL);
		say("shim: watchdog %d at %x: id %x control %x", (uint64_t)i, wdt[i], (uint64_t)id, (uint64_t)ctl);
		if (id != 0x574454 || !(ctl & 1) || (keep && starts(keep, "keep"))) {
			say("%s\n", (ctl & 1) ? " (left running)" : "");
			continue;
		}
		wr32(wdt[i] + WDT_KEY, WDT_KEY_UNLOCK);
		wr32(wdt[i] + WDT_CONTROL, ctl & ~1u);
		wr32(wdt[i] + WDT_KEY, 0);
		say(" -> %x\n", (uint64_t)rd32(wdt[i] + WDT_CONTROL));
	}
}

static void
fb_set(uint64_t base, uint32_t width, uint32_t height, uint32_t stride, const char *from)
{
	if (base == 0 || width == 0 || height == 0 || stride < width * 4) {
		say("shim: framebuffer from %s refused (%x, %d x %d, stride %d)\n", from, base, (uint64_t)width,
		    (uint64_t)height, (uint64_t)stride);
		return;
	}
	GOP_INFO.version = 0;
	GOP_INFO.width = width;
	GOP_INFO.height = height;
	GOP_INFO.pixel_format = 1;              /* PixelBlueGreenRedReserved8BitPerColor: a8r8g8b8 in memory */
	GOP_INFO.pixels_per_scanline = stride / 4;
	GOP_MODE.fb_base = base;
	GOP_MODE.fb_size = (uint64_t)stride * height;
	have_fb = 1;
	say("shim: framebuffer %d x %d at %x, stride %d, from %s\n", (uint64_t)width, (uint64_t)height, base,
	    (uint64_t)stride, from);
}

/*
 * Where the boot loader's picture is: scion.fb=BASE,WIDTH,HEIGHT,STRIDE on the boot image's command line first (the
 * one sure way, once the address is known); a simple-framebuffer node, should the boot loader add one; on the Tensor
 * G5, the display controller's enabled layer in A8R8G8B8, if its SMMU is off (then what the layer reads is a physical
 * address). The panel is a command-mode DSI panel: whether a write to this memory reaches it without another frame
 * being started is what the first run shows (README.md).
 */
static void
find_framebuffer(void)
{
	const char *a = bootarg("scion.fb=");
	if (a != NULL) {
		uint64_t v[4] = { 0 };
		for (int i = 0; i < 4; i++) {
			v[i] = parse_num(&a);
			if (*a == ',') {
				a++;
			}
		}
		fb_set(v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], "scion.fb=");
		if (have_fb) {
			return;
		}
	}
	if (FI.fb.ok) {
		fb_set(FI.fb.base, FI.fb.width, FI.fb.height, FI.fb.stride, "the device tree's simple-framebuffer");
		if (have_fb) {
			return;
		}
	}
	const char *probe = bootarg("scion.fbprobe=");
	if (!is_lga() || (probe && *probe == '0')) {
		return;
	}
	uint32_t cr0 = rd32(LGA_DPU_SMMU + SMMU_CR0);
	say("shim: DPU SMMU CR0 %x\n", (uint64_t)cr0);
	for (int l = 0; l < 6; l++) {
		uint64_t r = LGA_DPU + (uint64_t)l * DPU_LAYER_STEP;
		uint32_t cfg = rd32(r + DPU_L0_CONFIG), size = rd32(r + DPU_L0_SIZE);
		uint64_t addr = rd32(r + DPU_L0_ADDRESS) | (uint64_t)(rd32(r + DPU_L0_HIGH_ADDRESS) & 0xff) << 32;
		uint32_t stride = rd32(r + DPU_L0_STRIDE) & 0x3ffff;
		say("shim: DPU layer %d: config %x address %x stride %d size %x\n", (uint64_t)l, (uint64_t)cfg, addr,
		    (uint64_t)stride, (uint64_t)size);
		if (!have_fb && (cfg & 1) && ((cfg >> 24) & 0x3f) == 0 && !(cr0 & 1) &&
		    memmap_in_ram(&MM, addr, (uint64_t)stride * (size >> 16))) {
			fb_set(addr, size & 0xffff, size >> 16, stride, "the DPU's layer");
		}
	}
}

/*
 * The console on the screen, where UEFI's would be: the shim's lines and the loader's (its banner, "platform frankel"),
 * 8 x 8 glyphs (font8x8.h) doubled, light grey on black, below the stage bands; at the bottom it starts again at the top.
 */
#define CON_SCALE       2
#define CON_TOP         96
static int con_on;
static uint32_t con_x, con_y;

static void
screen_glyph(uint32_t cx, uint32_t cy, char c)
{
	const uint8_t *g = font8x8[(c >= 0x20 && c < 0x7f ? c : '?') - 0x20];
	uint32_t ppl = GOP_INFO.pixels_per_scanline;
	for (uint32_t y = 0; y < 8 * CON_SCALE; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(GOP_MODE.fb_base + (uint64_t)(cy + y) * ppl * 4);
		uint8_t bits = g[y / CON_SCALE];
		for (uint32_t x = 0; x < 8 * CON_SCALE; x++) {
			row[cx + x] = (bits >> (x / CON_SCALE)) & 1 ? 0x00c0c0c0 : 0;
		}
	}
}

static void
screen_newline(void)
{
	con_x = 0;
	con_y += 8 * CON_SCALE;
	if (con_y + 8 * CON_SCALE > GOP_INFO.height) {
		con_y = CON_TOP;
	}
	for (uint32_t x = 0; x + 8 * CON_SCALE <= GOP_INFO.width; x += 8 * CON_SCALE) {
		screen_glyph(x, con_y, ' ');
	}
}

static void
screen_putc(char c)
{
	if (!con_on) {
		return;
	}
	if (c == '\n') {
		screen_newline();
		return;
	}
	if (c == '\r') {
		return;
	}
	if (con_x + 8 * CON_SCALE > GOP_INFO.width) {
		screen_newline();
	}
	screen_glyph(con_x, con_y, c == '\t' ? ' ' : c);
	con_x += 8 * CON_SCALE;
}

static void
screen_console_on(void)
{
	if (!have_fb || GOP_INFO.height < CON_TOP + 8 * CON_SCALE || bootarg("scion.con=0")) {
		return;
	}
	con_x = 0;
	con_y = CON_TOP - 8 * CON_SCALE;
	con_on = 1;
	screen_newline();
	for (uint32_t i = 0; i < loglen; i++) {          /* what was said before there was a screen */
		screen_putc(logbuf[i]);
	}
}

/* a band of colour at the top of the screen for each stage reached: a photograph says how far the shim got */
static void
stage(int n, uint32_t rgb)
{
	if (!have_fb) {
		return;
	}
	uint32_t w = GOP_INFO.width / 8, y0 = 16 + (uint32_t)n * 24;
	for (uint32_t y = y0; y < y0 + 16 && y < GOP_INFO.height; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(GOP_MODE.fb_base + (uint64_t)y * GOP_INFO.pixels_per_scanline * 4);
		for (uint32_t x = 16; x < 16 + w; x++) {
			row[x] = rgb;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* The payload and the loader in it                                          */

static const uint8_t *
payload_entry(const uint8_t *p, const char *name, uint32_t *size)
{
	uint32_t total = *(const uint32_t *)(p + 4);
	for (uint32_t at = 8; at + 24 <= total + 8;) {
		const char *n = (const char *)p + at;
		uint32_t len = *(const uint32_t *)(p + at + 16);
		if (n[0] == 0) {
			break;
		}
		if (memeq(n, name, slen_(name) + 1)) {
			*size = len;
			return p + at + 24;
		}
		at += 24 + ((len + 15) & ~15u);
	}
	return NULL;
}

typedef efi_status (EFIAPI *efi_entry_fn)(efi_handle, efi_system_table *);

static efi_entry_fn
load_pe(const uint8_t *pe, uint32_t len)
{
	if (len < 0x200 || pe[0] != 'M' || pe[1] != 'Z') {
		say("shim: loader.efi: not a PE image\n");
		return NULL;
	}
	uint32_t lfanew = *(const uint32_t *)(pe + 0x3c);
	const uint8_t *nt = pe + lfanew, *opt = nt + 24;
	if (lfanew + 24 + 112 > len || !memeq(nt, "PE\0\0", 4) || *(const uint16_t *)(nt + 4) != 0xAA64 ||
	    *(const uint16_t *)opt != 0x20B) {
		say("shim: loader.efi: not an AArch64 PE32+ image\n");
		return NULL;
	}
	uint16_t nsect = *(const uint16_t *)(nt + 6), optsize = *(const uint16_t *)(nt + 20);
	uint32_t entry = *(const uint32_t *)(opt + 16), size_image = *(const uint32_t *)(opt + 56);
	uint32_t size_headers = *(const uint32_t *)(opt + 60), ndirs = *(const uint32_t *)(opt + 108);
	uint64_t pref = *(const uint64_t *)(opt + 24);
	uint64_t pages = ((uint64_t)size_image + 4095) / 4096, base = pref;

	if (EFI_ERROR(efi_allocate_pages(AllocateAddress, EfiLoaderCode, pages, &base)) &&
	    EFI_ERROR(efi_allocate_pages(AllocateAnyPages, EfiLoaderCode, pages, &base))) {
		say("shim: loader.efi: no room for %x bytes\n", (uint64_t)size_image);
		return NULL;
	}
	memset((void *)(uintptr_t)base, 0, pages * 4096);
	memcpy((void *)(uintptr_t)base, pe, size_headers < len ? size_headers : len);
	const uint8_t *sec = opt + optsize;
	for (int i = 0; i < nsect; i++, sec += 40) {
		uint32_t vsize = *(const uint32_t *)(sec + 8), va = *(const uint32_t *)(sec + 12);
		uint32_t raw = *(const uint32_t *)(sec + 16), rawp = *(const uint32_t *)(sec + 20);
		uint32_t n = raw < vsize || vsize == 0 ? raw : vsize;
		if (va + (uint64_t)n > size_image || rawp + (uint64_t)n > len) {
			say("shim: loader.efi: section %d out of bounds\n", (uint64_t)i);
			return NULL;
		}
		memcpy((void *)(uintptr_t)(base + va), pe + rawp, n);
	}
	/* base relocations (data directory 5): blocks of {page RVA, size, u16 (type << 12 | offset)...}; DIR64 (10) only */
	uint64_t delta = base - pref;
	if (ndirs > 5 && delta != 0) {
		uint32_t rva = *(const uint32_t *)(opt + 112 + 5 * 8), rsize = *(const uint32_t *)(opt + 112 + 5 * 8 + 4);
		for (uint32_t at = 0; at + 8 <= rsize;) {
			const uint8_t *blk = (const uint8_t *)(uintptr_t)(base + rva + at);
			uint32_t page = *(const uint32_t *)blk, bsize = *(const uint32_t *)(blk + 4);
			if (bsize < 8) {
				break;
			}
			for (uint32_t e = 8; e + 2 <= bsize; e += 2) {
				uint16_t v = *(const uint16_t *)(blk + e);
				if ((v >> 12) == 10) {
					*(uint64_t *)(uintptr_t)(base + page + (v & 0xfff)) += delta;
				} else if ((v >> 12) != 0) {
					say("shim: loader.efi: relocation type %d not understood\n", (uint64_t)(v >> 12));
					return NULL;
				}
			}
			at += bsize;
		}
	}
	sync_icache(base, base + pages * 4096);
	LOADED_IMAGE.revision = 0x1000;
	LOADED_IMAGE.st = &ST;
	LOADED_IMAGE.image_base = (void *)(uintptr_t)base;
	LOADED_IMAGE.image_size = pages * 4096;
	LOADED_IMAGE.code_type = EfiLoaderCode;
	LOADED_IMAGE.data_type = EfiLoaderData;
	say("shim: loader.efi at %x (wanted %x), %x bytes, entry +%x\n", base, pref, (uint64_t)size_image, (uint64_t)entry);
	return (efi_entry_fn)(uintptr_t)(base + entry);
}

/* an exception at EL1 before the loader has its own vectors (start.S: el1_vectors) */
void
shim_el1_fault(uint64_t esr, uint64_t elr, uint64_t far, uint64_t kind)
{
	say("\nshim: exception %d at EL1: ESR %x ELR %x FAR %x\n", kind, esr, elr, far);
	halt("stopped");
}

/* ------------------------------------------------------------------------ */

void
shim_main(const void *fdt)
{
	FDT = fdt;
	int r = fdt_parse(fdt, &FI);
	say("\nshim: Scion's Tensor G5 boot shim, entered at EL%d, device tree at %x (%d)\n", shim_entry_el,
	    (uint64_t)(uintptr_t)fdt, (uint64_t)(r < 0 ? -r : 0));
	if (r < 0) {
		halt("no device tree I can read");
	}
	say("shim: model %s, compatible %s\n", FI.model, FI.compatible);
	if (shim_entry_el != 2 && FI.psci_smc) {
		/* nothing at EL2 is ours to pass PSCI on: the loader (psci_hvc) and the kernel would call into nothing */
		say("shim: entered at EL1 with PSCI by SMC: platform_frankel.psci_hvc must be 0 for this (README.md)\n");
	}
	if (FI.rsv_dropped) {
		halt("more reserved regions than I keep");
	}

	/* where we are: the shim and the payload behind it */
	image_base = (uint64_t)(uintptr_t)_head;
	uint64_t shim_bytes = (uint64_t)(uintptr_t)__image_end - image_base;
	const uint8_t *payload = (const uint8_t *)(uintptr_t)(image_base + shim_bytes);
	uint32_t payload_bytes = *(const uint32_t *)(payload) == PAYLOAD_MAGIC ? *(const uint32_t *)(payload + 4) + 8 : 0;
	image_bytes = shim_bytes + payload_bytes;

	struct fdt_range extra[3] = {
		{ image_base, image_bytes, 0 },
		{ (uint64_t)(uintptr_t)fdt, FI.total_size, 0 },
		{ PAYLOAD_BASE, PAYLOAD_MAX, 0 },
	};
	memmap_build(&MM, &FI, extra, 3);
	if (is_lga()) {
		lga_watchdogs();
	}
	find_framebuffer();
	if (have_fb) {
		/* reserved, and mapped as a GOP's is (normal, not cached): the loader's and XNU's writes reach the display */
		struct fdt_range more[4] = { extra[0], extra[1], extra[2], { GOP_MODE.fb_base, GOP_MODE.fb_size, RSV_FB } };
		memmap_build(&MM, &FI, more, 4);
	}
	for (int i = 0; i < MM.nram; i++) {
		say("shim: RAM %x + %x\n", MM.ram[i].base, MM.ram[i].size);
	}
	say("shim: %d reservations; free:", (uint64_t)MM.nrsv);
	for (int i = 0; i < MM.nfree; i++) {
		say(" %x+%x", MM.free[i].base, MM.free[i].size);
	}
	say("\n");
	if (MM.nram == 0 || MM.nfree == 0) {
		halt("no RAM in the device tree");
	}
	if (mmu_on() < 0) {
		halt("too much RAM for the shim's page tables");
	}
	say("shim: MMU on, %d tables of 2 MB blocks\n", (uint64_t)l2_used);

	/* the UEFI memory map: the free RAM, and what the loader must know is taken */
	for (int i = 0; i < MM.nfree; i++) {
		desc_insert(ndesc, EfiConventionalMemory, MM.free[i].base, MM.free[i].size / 4096);
	}
	if (!memmap_in_ram(&MM, PAYLOAD_BASE, PAYLOAD_MAX)) {
		halt("0xF0000000 + 64 MB, where the loader takes its payload from, is not RAM here");
	}
	if (image_base < PAYLOAD_BASE + PAYLOAD_MAX && PAYLOAD_BASE < image_base + image_bytes) {
		halt("the boot loader put the shim over 0xF0000000 + 64 MB");
	}
	/* a device tree the boot loader put where the payload goes is moved out of its way first */
	uint64_t fdt_pa = (uint64_t)(uintptr_t)FDT;
	if (fdt_pa < PAYLOAD_BASE + PAYLOAD_MAX && PAYLOAD_BASE < fdt_pa + FI.total_size) {
		uint64_t to = 0;
		if (EFI_ERROR(efi_allocate_pages(AllocateAnyPages, EfiRuntimeServicesData, (FI.total_size + 4095) / 4096, &to))) {
			halt("no room to move the device tree out of the payload's way");
		}
		memcpy((void *)(uintptr_t)to, FDT, FI.total_size);
		FDT = (const void *)(uintptr_t)to;
		fdt_parse(FDT, &FI);
		say("shim: device tree moved to %x, out of the payload's way\n", to);
	}

	screen_console_on();
	stage(0, 0x000000ff);   /* blue: the shim runs, the MMU is on */

	if (payload_bytes == 0 || payload_bytes > PAYLOAD_MAX) {
		halt("no XNUP payload behind the shim");
	}
	memcpy((void *)(uintptr_t)PAYLOAD_BASE, payload, payload_bytes);
	say("shim: payload, %d bytes, copied to %x\n", (uint64_t)payload_bytes, PAYLOAD_BASE);
	stage(1, 0x0000ff00);   /* green: the payload is in place */

	uint32_t lsize = 0;
	const uint8_t *loader = payload_entry((const uint8_t *)(uintptr_t)PAYLOAD_BASE, "loader.efi", &lsize);
	if (loader == NULL) {
		halt("no loader.efi in the payload");
	}
	tables_init();
	LOADED_IMAGE.load_options = NULL;
	efi_entry_fn entry = load_pe(loader, lsize);
	if (entry == NULL) {
		halt("loader.efi could not be loaded");
	}
	stage(2, 0x00ffff00);   /* yellow: the loader is about to run */
	efi_status s = entry((efi_handle)&IMAGE_HANDLE, &ST);
	say("shim: loader.efi returned %x\n", s);
	if (shim_el2_fault[0] != 0) {
		say("shim: EL2 fault: ESR %x ELR %x FAR %x\n", shim_el2_fault[0], shim_el2_fault[1], shim_el2_fault[2]);
	}
	halt("the loader is done");
}
