/*
 * pakread.c -- read Controller Pak blocks through the driver's SDK
 * command-block channel. Runs INSIDE the VM, as Administrator.
 *
 *     pakread <first-block> [count]
 *
 * A block is 32 bytes, so block N is byte address N*32 and the Pak is
 * blocks 0..1023. Output is a hex dump plus the entry's reply-length byte,
 * whose top bit is the driver's failure flag.
 *
 * THE CONFIGURATOR CANNOT DO THIS. wishd201.exe has no Controller Pak
 * support at all - its only accessory feature is rumble - so this channel
 * is the only way to reach Pak storage. See docs/command-block.txt.
 *
 * THE SEQUENCE IS WRITE THEN READ, over one handle. The driver keeps a
 * single 64-byte block in the control device extension; a write stores the
 * commands and runs pass 0, which is the one that actually goes to the
 * controller, and the following read hands the same buffer back with the
 * replies filled in. Reading without writing first returns whatever the
 * previous write left.
 *
 * Build on the host with the x86 toolchain, statically linked so the VM
 * needs no redistributable:
 *
 *     cl /nologo /MT /W3 /Fe:pakread.exe tools\pakread.c
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define BLOCK_BYTES     0x40    /* the command block is always 64 bytes  */
#define GO_FLAG_OFFSET  0x3F    /* byte, not the dword at 0x3C           */
#define END_OF_BLOCK    0xFE
#define PAK_READ_CMD    0x02    /* joybus read accessory                 */
#define PAK_REPLY_LEN   33      /* 32 data bytes plus the CRC8           */
#define REPLY_FAIL_BIT  0x80

#define PAK_ADDR_BITS   11
#define PAK_ADDR_MASK   0x07FF

static const char *DEVICE_PATH = "\\\\.\\Wish_NA1";

/*
 * CRC5, polynomial 0x15 - x^5 + x^4 + x^2 + 1 - over the 11-bit block
 * address. Same routine as core_pak_addr_crc5 in src_drv/core.c; kept
 * separate because this file must build as an ordinary Win32 console
 * program with no driver headers.
 */
static unsigned char pak_addr_crc5(unsigned short address)
{
    unsigned short msg = (unsigned short)((address >> 5) & PAK_ADDR_MASK);
    unsigned char  crc = 0;
    int i;

    for (i = 0; i < PAK_ADDR_BITS; i++) {
        if (msg & (1u << (PAK_ADDR_BITS - 1))) {
            crc ^= 0x10;
        }
        crc = (unsigned char)(crc << 1);
        if (crc & 0x20) {
            crc ^= 0x15;
        }
        crc &= 0x1f;
        msg = (unsigned short)((msg << 1) & PAK_ADDR_MASK);
    }
    return crc;
}

/* Block address in the top 11 bits, its CRC5 in the low 5. */
static unsigned short pak_addr_word(unsigned short address)
{
    unsigned short block = (unsigned short)((address >> 5) & PAK_ADDR_MASK);
    return (unsigned short)((block << 5) | pak_addr_crc5(address));
}

static void dump(unsigned long block, const unsigned char *p, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            printf("  %04lx:%02x  ", block, i);
        }
        printf("%02x ", p[i]);
        if ((i % 16) == 15) {
            printf("\n");
        }
    }
    if (len % 16) {
        printf("\n");
    }
}

static int read_block(HANDLE h, unsigned long block)
{
    unsigned char  buf[BLOCK_BYTES];
    unsigned short addr;
    unsigned short word;
    DWORD          done;
    unsigned char  reply_len;

    if (block > 1023) {
        fprintf(stderr, "block %lu is past the end of a 32KB Pak\n", block);
        return 0;
    }

    addr = (unsigned short)(block * 32);
    word = pak_addr_word(addr);

    ZeroMemory(buf, sizeof(buf));
    buf[0] = 3;                                     /* command length     */
    buf[1] = PAK_REPLY_LEN;                         /* reply length       */
    buf[2] = PAK_READ_CMD;
    buf[3] = (unsigned char)(word >> 8);            /* address, big end   */
    buf[4] = (unsigned char)(word & 0xFF);
    buf[2 + 3 + PAK_REPLY_LEN] = END_OF_BLOCK;      /* next entry: stop   */

    /*
     * Little-endian client: leave the dword at 0x3C zero and set the go
     * flag byte directly. Writing 1 to the dword would ask for byte
     * swapping instead, and after the swap that same bit LANDS on the go
     * flag - which is why a big-endian client cannot stage a block
     * without also running it.
     */
    buf[GO_FLAG_OFFSET] = 1;

    if (!WriteFile(h, buf, BLOCK_BYTES, &done, NULL) || done != BLOCK_BYTES) {
        fprintf(stderr, "block %lu: write failed, error %lu\n",
                block, GetLastError());
        return 0;
    }

    ZeroMemory(buf, sizeof(buf));
    if (!ReadFile(h, buf, BLOCK_BYTES, &done, NULL) || done != BLOCK_BYTES) {
        fprintf(stderr, "block %lu: read failed, error %lu\n",
                block, GetLastError());
        return 0;
    }

    reply_len = buf[1];
    if (reply_len & REPLY_FAIL_BIT) {
        printf("  %04lx: ENTRY FAILED (reply byte %02x, length %u)\n",
               block, reply_len, reply_len & 0x3F);
        return 0;
    }

    printf("  block %lu  addr %04x  word %04x  reply %u bytes\n",
           block, addr, word, reply_len);
    dump(block, buf + 5, PAK_REPLY_LEN);
    return 1;
}

int main(int argc, char **argv)
{
    HANDLE        h;
    unsigned long first;
    unsigned long count;
    unsigned long i;
    unsigned long ok = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: pakread <first-block> [count]\n");
        return 2;
    }
    first = strtoul(argv[1], NULL, 0);
    count = (argc > 2) ? strtoul(argv[2], NULL, 0) : 1;

    h = CreateFileA(DEVICE_PATH, GENERIC_READ | GENERIC_WRITE,
                    0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s, error %lu\n",
                DEVICE_PATH, GetLastError());
        fprintf(stderr, "run as Administrator, with the adapter plugged in\n");
        return 1;
    }

    printf("%s opened\n", DEVICE_PATH);
    for (i = 0; i < count; i++) {
        ok += (unsigned long)read_block(h, first + i);
    }
    printf("%lu of %lu block(s) read\n", ok, count);

    CloseHandle(h);
    return (ok == count) ? 0 : 1;
}
