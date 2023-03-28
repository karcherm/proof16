#include <sys/syscall.h>  /* for syscall */
#include <string.h>       /* for memcpy */
#include <unistd.h>       /* for SYS_modify_ldt */
#include <stdio.h>        /* for perror */
#include <sys/mman.h>     /* for mmap */
#include <asm/ldt.h>      /* for struct user_desc */
#include <stdint.h>       /* for uint32_t & uint16_t */
#include <stdlib.h>       /* for exit */

const int CODE_INDEX = 3;
const int DATA_INDEX = 4;
const int SCREEN_INDEX = 5;
const int TRAMPOLINE_CODE_INDEX = 6;
const int TRAMPOLINE_DATA_INDEX = 7;

const int IS_LDT = 4;
const int INDEX_MULTIPLIER = 8;

// Creates an level-3 LDT selector given an LDT index
#define LDTSEL_L3(idx) ((idx) * INDEX_MULTIPLIER + IS_LDT + 3)

/* normal unix convention: 0=good, -1=bad (see errno) */
int write_ldt(struct user_desc *new_desc)
{
	return syscall(SYS_modify_ldt, 0x11, new_desc, sizeof(*new_desc));
}

size_t roundup_page(size_t size)
{
	return (size + 0xFFF) & ~(size_t)0xFFF;
}

void* map32(size_t size, int executable)
{
	int prot = PROT_READ | PROT_WRITE;
	if (executable)
		prot |= PROT_EXEC;
	return mmap(NULL, roundup_page(size), prot, MAP_ANON | MAP_32BIT | MAP_PRIVATE, -1, 0);
}

void* initmap32(const void* src, size_t size, int executable)
{
	void* addr = map32(size, executable);
	memcpy(addr, src, size);
	return addr;
}

/* regparm(1): EAX on i386; RDI on x64 */
#ifdef __LP64__
#define REGPARM1 "rdi"
#else
#define REGPARM1 "eax"
#endif

struct fword {
	uint32_t ofs;
	uint16_t seg;
};

typedef int (*__attribute((regparm(1))) trampoline_t)(const struct fword* target);
trampoline_t call_trampoline;
void init_call_trampoline()
{
#ifdef __LP64__
	static const unsigned char trampoline_code[3] = {0xFF, 0x1F, 0xC3};  // lcalll *%rdi ; retq
#else
	static const unsigned char trampoline_code[3] = {0xFF, 0x18, 0xC3};  // lcalll *%eax ; ret
#endif
	call_trampoline = (trampoline_t)initmap32(trampoline_code, sizeof trampoline_code, 1);
}

#ifdef __LP64__
__attribute__((naked, regparm(1))) void switch_stack_(void* dummy)
{
	__asm__ __volatile__(
        "mov %%" REGPARM1 ", %%rdi\n"
	    "mov %%rsp, %%rsi\n"
	    "mov %%rsp, %%rcx\n"
	    "and $0xFFF, %%rcx\n"
	    "add %%rcx, %%rdi\n"
	    "add $0xF000, %%rdi\n"
	    "xor $0xFFF, %%rcx\n"
	    "inc %%rcx\n"
	    "sub %%rcx, %%rdi\n" 
	    "mov %%rdi, %%rax\n"
	    "rep ; movsb\n"
	    "mov %%rax, %%rsp\n"
	    "ret" : : : "rdi","rax","rsi","rcx","memory");

}
#define switch_stack() do { switch_stack_(map32(0x10000, 0)); } while(0)

#else
#define switch_stack()
#endif

void load(void* target, size_t maxsize, const char* filename)
{
	FILE* f;
	size_t got;

	f = fopen(filename, "rb");
	if (!f)
	{
		perror(filename);
		exit(1);
	}
	got = fread(target, 1, maxsize, f);
	if (got < 1 || got == maxsize)
	{
		puts("loading issue...");
		exit(1);
	}
	fclose(f);
}

void clear_screen(unsigned char *screenbase)
{
	int i;
	for (i = 0;i < 80*25; i++)
	{
		screenbase[2*i] = ' ';
		screenbase[2*i+1] = 7;  // gray on black
	}
}

void dump_screen(unsigned char *screenbase)
{
	int row, col;
	for (row = 0; row < 25; row++)
	{
		for (col = 0; col < 80; col++)
		{
			if (screenbase[2*(80*row + col) + 1] == 0x70)
				fwrite("\x1b[7m", 4, 1, stdout);
            else
                fwrite("\x1b[0m", 4, 1, stdout);
            putchar(screenbase[2*(80*row + col)]);
        }
        putchar('\n');
    }
}

struct trampoline_data {
	uint16_t r_di, r_si, r_bp, r_dummy_sp, r_bx, r_dx, r_cx, r_ax;
	uint16_t r_es, r_ss, r_sp, r_flags;
	uint16_t r_cs, r_ip, r_ds;
	uint16_t reserved[16];
};

// Tampoline data:
// 00..0F: general purpose registers in POPA layout (SP is ignored)
// 10:     ES
// 12:     SS
// 14:     SP
// 16:     FLAGS
// 18:     CS
// 1A:     IP
// 1C:     DS

// 20..25: temporary use (caller stack)


// set up a trampoline to call into VC code (the trampoline code already is 16 bit,
// as we want to load segment registers and want to be independent of 32 or 64 bit host.

static const unsigned char to16_trampoline[] = {
	// save registers
	0x66, 0x60,                   //   pushad
	0x06,                         //   push %es
	0x1e,                         //   push %ds
	0x68, LDTSEL_L3(TRAMPOLINE_DATA_INDEX), 0, //   push $TRAMPOLINE_DS
	0x1f,                         //   pop %ds
	0x66, 0x89, 0x26, 0x20, 0x00, //   mov %esp,(0x20)
	0x8c, 0x16, 0x24, 0x00,       //   mov %ss,(0x24)

	// switch to trampoline stack
	0x1e, 0x17,                   //   push ds; pop ss
	0x33, 0xE4,                   //   xor %sp,%sp

	// load target context
	0x61,                         //   popa
	0x07,                         //   pop %es
	0x17,                         //   pop %ss
	0x8b, 0x26, 0x14, 0x00,       //   mov (0x14), %sp (load SP just after setting SS for atomic stack switch)
	                              //   further access to data has to be performed via DS, as SS no longer
				      //   points to TRAMPOLINE_DATA
	0xff, 0x36, 0x16, 0x00,       //   pushw (0x16)  [FLAGS]
	0xff, 0x36, 0x18, 0x00,       //   pushw (0x18)  [CS]
	0xff, 0x36, 0x1A, 0x00,       //   pushw (0x1A)  [IP]
	0x8e, 0x1e, 0x1C, 0x00,       //   mov (0x1C), %ds
	0xcf,                         //   iret
};

static const unsigned char from16_trampoline[] = {
	// store target context
	0x9C,                         //   pushf
	0x1e,                         //   push %ds
	0x68, LDTSEL_L3(TRAMPOLINE_DATA_INDEX), 0, //   push $TRAMPOLINE_DS
	0x1f,                         //   pop %ds
	0x8f, 0x06, 0x1C, 0x00,       //   pop (0x1C)  [DS]
	0x8f, 0x06, 0x16, 0x00,       //   pop (0x16)  [FLAGS]
	0x8f, 0x06, 0x1A, 0x00,       //   pop (0x1A)  [IP]
	0x8f, 0x06, 0x18, 0x00,       //   pop (0x18)  [CS]
	0x89, 0x26, 0x14, 0x00,       //   mov %sp,(0x14)
	0x8C, 0x16, 0x12, 0x00,       //   mov %ss,(0x12)
	0x1e, 0x17,                   //   push %ds; pop %ss
	0xBC, 0x12, 0x00,             //   mov %sp,0x12
	0x06,                         //   push %es
	0x60,                         //   pusha

	// restore registers
	0x66, 0x0f, 0xb2, 0x26, 0x20, 0x00, //   lss %esp, (0x20)
	0x1f,                         //   pop %ds
	0x07,                         //   pop %es
	0x66, 0x61,                   //   popad
	0x66, 0xcb                    //   retfd
};

#define FROM16_OFS 0x00
#define TO16_OFS 0x80
#define DATA_OFS 0x100

struct trampoline_data* context16;

void make_trampoline(void)
{
	struct user_desc my_desc;
	unsigned char* trampoline_area = map32(0x200, 1);
	memcpy(trampoline_area + FROM16_OFS, from16_trampoline, sizeof from16_trampoline);
	memcpy(trampoline_area + TO16_OFS, to16_trampoline, sizeof to16_trampoline);

	my_desc.entry_number = TRAMPOLINE_CODE_INDEX;
	my_desc.base_addr = (uintptr_t)trampoline_area & 0xFFFFFFFF;
	my_desc.limit = 0x100;
	my_desc.seg_32bit = 0;	  // 16 bit!
	my_desc.contents = 2;     // non-conforming code
	my_desc.read_exec_only = 0;
	my_desc.limit_in_pages = 0;
	my_desc.seg_not_present = 0;
	my_desc.useable = 1;
	if(write_ldt(&my_desc) < 0)
		perror("modify_ldt(trampoline code)");

	context16 = (void*)(trampoline_area + DATA_OFS);
	my_desc.entry_number = TRAMPOLINE_DATA_INDEX;
	my_desc.limit = sizeof(struct trampoline_data);
	my_desc.base_addr += DATA_OFS;
	my_desc.contents = 0;     // expand-up data
	if(write_ldt(&my_desc) < 0)
		perror("modify_ldt(trampoline data)");
}

int enter16()
{
	const struct fword target = {TO16_OFS, LDTSEL_L3(TRAMPOLINE_CODE_INDEX)};
	call_trampoline(&target);
}

int main(void)
{
	unsigned char* codebase;
	unsigned char* screenbase;
	struct user_desc my_desc;
	uint16_t exit_ofs;

	const int DATA_SELECTOR = LDTSEL_L3(DATA_INDEX);
	const int CODE_SELECTOR = LDTSEL_L3(CODE_INDEX);
	const int SCREEN_SELECTOR = SCREEN_INDEX * INDEX_MULTIPLIER + IS_LDT + 3;

	switch_stack();
	init_call_trampoline();
	make_trampoline();
	codebase = (unsigned char*)map32(0x10000, 1);
	screenbase = (unsigned char*)map32(0x1000, 0);  // 4KB of video RAM (like MDA) is enough

	load(codebase + 0x100, 0xFF00, "VC.COM");
	clear_screen(screenbase);

	my_desc.entry_number = CODE_INDEX;
	my_desc.base_addr = (uintptr_t)codebase & 0xFFFFFFFF;
	my_desc.limit = 0x10000;  // full 64KB
	my_desc.seg_32bit = 0;	  // 16 bit!
	my_desc.contents = 2;     // non-conforming code
	my_desc.read_exec_only = 0;
	my_desc.limit_in_pages = 0;
	my_desc.seg_not_present = 0;
	my_desc.useable = 1;
	if(write_ldt(&my_desc) < 0)
		perror("modify_ldt(code)");
	my_desc.entry_number = DATA_INDEX;
	my_desc.contents = 0;     // expand-up data
	if(write_ldt(&my_desc) < 0)
		perror("modify_ldt(data)");
	my_desc.entry_number = SCREEN_INDEX;
	my_desc.base_addr = (uintptr_t)screenbase & 0xFFFFFFFF;
	my_desc.limit = 0x1000;   // MDA just has 4K video RAM
	if(write_ldt(&my_desc) < 0)
		perror("modify_ldt(screen)");

	/* install patches to run in this primitive "virtualized" environment */
	
	/* replace "MOV AX,CS" by "MOV AX,DS" in inital segment register setup */
	memcpy(codebase+0x10C, "\x8C\xD8", 2);

	/* Video interface related stuff */
	/* ----------------------------- */
	/* MOV AL,30 instead of INT 11 to find out the current video card (30 = MDA)
	   we want MDA mode to disable CGA snow checking */
	memcpy(codebase+0x6933, "\xB0\x30", 2);
	/* patch segment B000 to our fake screen selector */
	*(uint16_t*)(codebase+0x6944) = SCREEN_SELECTOR;

	// Kill INT 10 to set video mode 2 (which the IBM BIOS will treat as video mode 7 in "MDA mode")
	memcpy(codebase+0x692E, "\x90\x90", 2);
	// replace setting video mode and moving the cursor off screen by clearing the video
	// buffer manually. The replacement code is too large, so put it in the PSP
	memcpy(codebase+0x30,   "\x06"                   // push es
	                        "\x57"                   // push di
				"\x8E\x06\x45\x75"       // mov es,[ScreenSegment]
				"\x33\xFF"               // xor di,di
				"\xB8\x20\x07"           // mov ax,720h
				"\xB1\x00\x08"           // mov cx,800h
				"\xF3\xAB"               // rep stosw
				"\x5F"                   // pop di
				"\x07"                   // pop es
				"\xC3",                  // retn
				19);
	memcpy(codebase+0x6A81, "\xE8\xAC\x95", 3);      // call 0030
	memset(codebase+0x6A84, 0x90, 12);

    /* remove STI (executed in MDA path too, intended to revert CGA CLI) */
    *(uint8_t*)(codebase+0x6A4B) = 0x90;

	/* Keyboard interface related stuff */
	/* -------------------------------- */
	/* nop out INT21 call to install Ctrl-Brk handler */
	memcpy(codebase+0x5F87, "\x90\x90", 2);
	/* nop out instruction to clear break flag */
	memcpy(codebase+0x5FA1, "\x90\x90\x90\x90\x90", 5);
	memcpy(codebase+0x6128, "\x90\x90\x90\x90\x90", 5);
	/* report no shift keys and modifiers pressed (used for ScrlLock -> Break emulation) */
	memcpy(codebase+0x5F8B, "\xB0\x00", 2);
	memcpy(codebase+0x5FD7, "\xB0\x00", 2);
	memcpy(codebase+0x5FE9, "\xB0\x00", 2);
	/* for now: do not implement virtual keyboard input; return Z from INT 16, ah=1 */
	/* by overwriting INT 16 with XOR AX,AX */
	memcpy(codebase+0x5FE4, "\x33\xC0", 2);
	memcpy(codebase+0x5FFE, "\x33\xC0", 2);
	/* for now: As we always report an empty queue, no need to patch INT 16 (AH=0) at 6007 */

	/* System initialization stuff */
	/* --------------------------- */
	/* nop out INT21 call to set DTA address */
	memcpy(codebase+0x611E, "\x90\x90", 2);
	/* nop out INT21 call to set Critical Error handler */
	memcpy(codebase+0x6135, "\x90\x90", 2);
	/* return "1 floppy, no serial, no parallel" instead of INT 11 */
	memcpy(codebase+0x613F, "\x33\xC0", 2);  // xor ax,ax
	
	/* Set memory in a way that there is no space for cell data. This prevents
	   most segment register loads to access cell data space */
	/* The statically allocated data ends at 755C. This is rounded up to paragraphs,
	   so the dynamic memory for spreadsheet data starts at 7560 and is managed in paragraphs.
	   VC starts up with a single cell, which need a single row pointer to the cell array of that
	   row and a single cell pointer that points to the contents of that cell. This data is stored
	   in one "block", so these two words occupy a single paragraph.
	   Cell content data thus starts at 7570. Thus we want relative segment 757 to be used as
	   the end of allocatable memory. The clearing loop exits before accessing data in that segment.
	   VC decrements the first non-available segment to obtain its internal end, so we need relative
	   segment 758 in that location in the PSP */
	*(uint16_t*)(codebase+2) = DATA_SELECTOR + 0x758;
	*(uint16_t*)(codebase+0x404d) = 0x9090; // skip loading ES

	exit_ofs = 0x2A5A;
	*(uint8_t*)(codebase+exit_ofs) = 0x9A;               // CALL FAR
	*(uint16_t*)(codebase+exit_ofs+1) = FROM16_OFS;
	*(uint16_t*)(codebase+exit_ofs+3) = LDTSEL_L3(TRAMPOLINE_CODE_INDEX);
	//printf("mapped to: %8p\n", codebase);

	context16->r_cs = CODE_SELECTOR;
	context16->r_ds = DATA_SELECTOR;
	context16->r_ss = DATA_SELECTOR;
	context16->r_ax = 0x1234;
	context16->r_sp = 0xFFFC;
	context16->r_ip = 0x100;

	enter16();

/*	printf("output CS:IP = %04x:%04x\n", context16->r_cs, context16->r_ip);
	printf("output SS:SP = %04x:%04x\n", context16->r_ss, context16->r_sp);

	printf("AX=%04x BX=%04x CX=%04x DX=%04x  BP=%04x SI=%04x DI=%04x\n"
	       "DS=%04x ES=%04x\n",
		context16->r_ax, context16->r_bx, context16->r_cx, context16->r_dx,
		context16->r_bp, context16->r_si, context16->r_di,
		context16->r_ds, context16->r_es); */

    dump_screen(screenbase);

	return 0;
}
