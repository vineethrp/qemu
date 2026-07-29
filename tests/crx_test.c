/*
 * crx_test.c — userspace test suite for the Cryptex (CRX) kernel driver
 *
 * Covers two paths:
 *
 *   PATH 1 — single-shot DMA ioctl (CRX_FLAG_USE_DMA):
 *     Driver programs CTRL_DMA_IN | CTRL_ENCRYPT | CTRL_DMA_OUT and fires
 *     CTRL_START once. Data goes: user buf → BAR1 → XOR → user buf. One
 *     ioctl, one interrupt, no intermediate driver state.
 *
 *   PATH 2 — decoupled write / encrypt / read:
 *     write()  → CTRL_DMA_IN only      (populate BAR1)
 *     ioctl()  → CTRL_ENCRYPT only     (XOR in place)
 *     read()   → CTRL_DMA_OUT only     (drain BAR1)
 *     Exercises the char-device read/write interface independently.
 *
 * Build:  gcc -O2 -Wall -o crx_test crx_test.c
 * Usage:  ./crx_test [/dev/crx0]
 *
 * The ioctl numbers and structs here must match the kernel driver header.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>

/* -------------------------------------------------------------------------
 * CRX ABI — keep in sync with the kernel driver's crx.h
 * ---------------------------------------------------------------------- */

struct crx_op {
    __u64 src;      /* userspace VA of input  */
    __u64 dst;      /* userspace VA of output */
    __u32 len;      /* bytes (max 65536)      */
    __u32 flags;
};

struct crx_stats {
    __u64 ops_completed;
    __u64 bytes_processed;
    __u64 irq_count;
};

#define CRX_FLAG_USE_DMA    (1U << 0)   /* single-shot DMA_IN+ENCRYPT+DMA_OUT */
#define CRX_FLAG_ASYNC      (1U << 1)   /* don't block; poll CRX_STATUS */

#define CRX_IOC_MAGIC       'C'
#define CRX_IOC_SET_KEY     _IOW (CRX_IOC_MAGIC, 0, __u32)
#define CRX_IOC_ENCRYPT     _IOWR(CRX_IOC_MAGIC, 1, struct crx_op)
#define CRX_IOC_GET_STATS   _IOR (CRX_IOC_MAGIC, 2, struct crx_stats)
#define CRX_IOC_RESET       _IO  (CRX_IOC_MAGIC, 3)

#define CRX_BAR1_SIZE       (64 * 1024)

/* -------------------------------------------------------------------------
 * Test framework (minimal)
 * ---------------------------------------------------------------------- */

static int g_passed, g_failed;

#define PASS(fmt, ...) do { \
    printf("  PASS  " fmt "\n", ##__VA_ARGS__); \
    g_passed++; \
} while (0)

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "  FAIL  " fmt "\n", ##__VA_ARGS__); \
    g_failed++; \
} while (0)

#define CHECK(cond, fmt, ...) \
    do { if (cond) PASS(fmt, ##__VA_ARGS__); else FAIL(fmt, ##__VA_ARGS__); } while (0)

/* Fatal: print and abort the whole run. */
#define REQUIRE(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FATAL  " fmt "\n", ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

static void section(const char *name)
{
    printf("\n[%s]\n", name);
}

/* -------------------------------------------------------------------------
 * Reference XOR: mirrors the device's in-place operation
 * ---------------------------------------------------------------------- */

/* Key bytes repeat LSB-first: byte i uses bits [(i%4)*8 +: 8] of key. */
static void xor_ref(uint8_t *dst, const uint8_t *src, size_t len, uint32_t key)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i] ^ ((key >> ((i % 4) * 8)) & 0xff);
}

/* -------------------------------------------------------------------------
 * PATH 1: single-shot DMA ioctl
 * ---------------------------------------------------------------------- */

static void test_dma_basic(int fd, const char *label,
                            const uint8_t *src, size_t len, uint32_t key)
{
    uint8_t *dst      = malloc(len);
    uint8_t *expected = malloc(len);
    REQUIRE(dst && expected, "malloc");

    xor_ref(expected, src, len, key);

    __u32 k = key;
    REQUIRE(ioctl(fd, CRX_IOC_SET_KEY, &k) == 0,
            "SET_KEY: %s", strerror(errno));

    struct crx_op op = {
        .src   = (uintptr_t)src,
        .dst   = (uintptr_t)dst,
        .len   = len,
        .flags = CRX_FLAG_USE_DMA,
    };
    int rc = ioctl(fd, CRX_IOC_ENCRYPT, &op);
    CHECK(rc == 0, "DMA encrypt %s (len=%zu key=0x%08x): rc=%d",
          label, len, key, rc);

    if (rc == 0)
        CHECK(memcmp(dst, expected, len) == 0,
              "DMA output correct %s", label);

    free(dst);
    free(expected);
}

/*
 * Encrypt twice with the same key — must recover the original data because
 * XOR is its own inverse.
 */
static void test_dma_roundtrip(int fd, size_t len, uint32_t key)
{
    uint8_t *src = malloc(len);
    uint8_t *mid = malloc(len);
    uint8_t *out = malloc(len);
    REQUIRE(src && mid && out, "malloc");

    for (size_t i = 0; i < len; i++)
        src[i] = (uint8_t)(i ^ (i >> 8));

    __u32 k = key;
    REQUIRE(ioctl(fd, CRX_IOC_SET_KEY, &k) == 0,
            "SET_KEY: %s", strerror(errno));

    struct crx_op enc = { (uintptr_t)src, (uintptr_t)mid, len, CRX_FLAG_USE_DMA };
    REQUIRE(ioctl(fd, CRX_IOC_ENCRYPT, &enc) == 0,
            "first encrypt failed: %s", strerror(errno));

    struct crx_op dec = { (uintptr_t)mid, (uintptr_t)out, len, CRX_FLAG_USE_DMA };
    REQUIRE(ioctl(fd, CRX_IOC_ENCRYPT, &dec) == 0,
            "second encrypt failed: %s", strerror(errno));

    CHECK(memcmp(src, out, len) == 0,
          "DMA round-trip len=%zu key=0x%08x: double-XOR recovers original",
          len, key);

    free(src); free(mid); free(out);
}

/* src == dst: in-place DMA encrypt (driver must handle overlapping pointers). */
static void test_dma_inplace(int fd, size_t len, uint32_t key)
{
    uint8_t *buf      = malloc(len);
    uint8_t *expected = malloc(len);
    REQUIRE(buf && expected, "malloc");

    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i + 0x33);

    xor_ref(expected, buf, len, key);

    __u32 k = key;
    REQUIRE(ioctl(fd, CRX_IOC_SET_KEY, &k) == 0,
            "SET_KEY: %s", strerror(errno));

    /* src and dst point to the same buffer */
    struct crx_op op = { (uintptr_t)buf, (uintptr_t)buf, len, CRX_FLAG_USE_DMA };
    int rc = ioctl(fd, CRX_IOC_ENCRYPT, &op);
    CHECK(rc == 0, "DMA in-place len=%zu: rc=%d", len, rc);
    if (rc == 0)
        CHECK(memcmp(buf, expected, len) == 0,
              "DMA in-place output correct len=%zu", len);

    free(buf); free(expected);
}

/* -------------------------------------------------------------------------
 * PATH 2: decoupled write / encrypt / read via char-device syscalls
 * ---------------------------------------------------------------------- */

/*
 * write() fills BAR1 (CTRL_DMA_IN only, no encrypt).
 * ioctl(ENCRYPT, no USE_DMA) XORs BAR1 in place (CTRL_ENCRYPT only).
 * read() drains BAR1 (CTRL_DMA_OUT only, no encrypt).
 */
static void test_pio_path(int fd, const uint8_t *src, size_t len, uint32_t key)
{
    uint8_t *out      = malloc(len);
    uint8_t *expected = malloc(len);
    REQUIRE(out && expected, "malloc");

    xor_ref(expected, src, len, key);

    /* Step 1: write plaintext into BAR1 */
    ssize_t wr = write(fd, src, len);
    CHECK((size_t)wr == len,
          "write %zu bytes into BAR1: wrote %zd", len, wr);

    /* Step 2: encrypt in place — ioctl WITHOUT CRX_FLAG_USE_DMA */
    __u32 k = key;
    REQUIRE(ioctl(fd, CRX_IOC_SET_KEY, &k) == 0,
            "SET_KEY: %s", strerror(errno));

    struct crx_op op = { .len = len, .flags = 0 /* no DMA */ };
    int rc = ioctl(fd, CRX_IOC_ENCRYPT, &op);
    CHECK(rc == 0, "in-place encrypt (no DMA) len=%zu key=0x%08x: rc=%d",
          len, key, rc);

    /* Step 3: read ciphertext back from BAR1 */
    if (rc == 0) {
        ssize_t rd = read(fd, out, len);
        CHECK((size_t)rd == len,
              "read %zu bytes from BAR1: got %zd", len, rd);
        if ((size_t)rd == len)
            CHECK(memcmp(out, expected, len) == 0,
                  "PIO output correct len=%zu key=0x%08x", len, key);
    }

    free(out); free(expected);
}

/* -------------------------------------------------------------------------
 * Stats: ops_completed and bytes_processed must grow after each operation
 * ---------------------------------------------------------------------- */

static void test_stats(int fd)
{
    struct crx_stats before, after;
    REQUIRE(ioctl(fd, CRX_IOC_GET_STATS, &before) == 0,
            "GET_STATS before: %s", strerror(errno));

    const size_t len = 256;
    uint8_t *buf = calloc(1, len);
    REQUIRE(buf, "calloc");

    __u32 k = 0xAABBCCDD;
    ioctl(fd, CRX_IOC_SET_KEY, &k);

    struct crx_op op = { (uintptr_t)buf, (uintptr_t)buf, len, CRX_FLAG_USE_DMA };
    REQUIRE(ioctl(fd, CRX_IOC_ENCRYPT, &op) == 0,
            "encrypt for stats test: %s", strerror(errno));

    REQUIRE(ioctl(fd, CRX_IOC_GET_STATS, &after) == 0,
            "GET_STATS after: %s", strerror(errno));

    CHECK(after.ops_completed == before.ops_completed + 1,
          "ops_completed: %llu → %llu (expected +1)",
          (unsigned long long)before.ops_completed,
          (unsigned long long)after.ops_completed);

    CHECK(after.bytes_processed == before.bytes_processed + len,
          "bytes_processed: %llu → %llu (expected +%zu)",
          (unsigned long long)before.bytes_processed,
          (unsigned long long)after.bytes_processed, len);

    CHECK(after.irq_count > before.irq_count,
          "irq_count: %llu → %llu (expected increase)",
          (unsigned long long)before.irq_count,
          (unsigned long long)after.irq_count);

    free(buf);
}

/* -------------------------------------------------------------------------
 * Error conditions
 * ---------------------------------------------------------------------- */

static void test_errors(int fd)
{
    uint8_t dummy[8] = {0};
    __u32 k = 0;
    ioctl(fd, CRX_IOC_SET_KEY, &k);

    /* len > 64KB must fail */
    struct crx_op op = {
        .src   = (uintptr_t)dummy,
        .dst   = (uintptr_t)dummy,
        .len   = CRX_BAR1_SIZE + 1,
        .flags = CRX_FLAG_USE_DMA,
    };
    int rc = ioctl(fd, CRX_IOC_ENCRYPT, &op);
    CHECK(rc != 0,
          "len > 64KB rejected (rc=%d errno=%s)", rc, strerror(errno));

    /* Reset must clear the error state */
    rc = ioctl(fd, CRX_IOC_RESET);
    CHECK(rc == 0, "reset after error: rc=%d", rc);
}

/* -------------------------------------------------------------------------
 * mmap: map BAR1 and verify it reflects what write() placed there
 * ---------------------------------------------------------------------- */

static void test_mmap(int fd)
{
    void *bar1 = mmap(NULL, CRX_BAR1_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (bar1 == MAP_FAILED) {
        printf("  SKIP  mmap: %s (kernel driver may not support it yet)\n",
               strerror(errno));
        return;
    }

    const size_t len = 64;
    uint8_t src[64], expected[64];
    uint32_t key = 0x12345678;

    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 7);
    xor_ref(expected, src, len, key);

    /* Write directly into BAR1 via mmap */
    memcpy(bar1, src, len);

    /* Encrypt in place */
    __u32 k = key;
    ioctl(fd, CRX_IOC_SET_KEY, &k);
    struct crx_op op = { .len = len, .flags = 0 };
    int rc = ioctl(fd, CRX_IOC_ENCRYPT, &op);
    CHECK(rc == 0, "mmap: in-place encrypt via ioctl rc=%d", rc);

    if (rc == 0)
        CHECK(memcmp(bar1, expected, len) == 0,
              "mmap: BAR1 contents correct after encrypt");

    munmap(bar1, CRX_BAR1_SIZE);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/crx0";

    int fd = open(dev, O_RDWR);
    REQUIRE(fd >= 0, "open(%s): %s", dev, strerror(errno));

    printf("CRX test suite  device=%s\n", dev);

    REQUIRE(ioctl(fd, CRX_IOC_RESET) == 0,
            "initial reset: %s", strerror(errno));

    /* ------------------------------------------------------------------
     * PATH 1: single-shot DMA ioctl (DMA_IN | ENCRYPT | DMA_OUT)
     * ------------------------------------------------------------------ */
    section("PATH 1: single-shot DMA ioctl");

    {
        uint8_t src[64];
        for (int i = 0; i < 64; i++) src[i] = i;
        test_dma_basic(fd, "small (64B)",       src, 64,            0xDEADBEEF);
        test_dma_basic(fd, "zero plaintext",    src, 64,            0xCAFEBABE);
    }
    {
        uint8_t zeros[256];
        memset(zeros, 0, sizeof(zeros));
        test_dma_basic(fd, "zero key (identity)", zeros, 256,       0x00000000);
    }
    {
        /* Non-multiple-of-4 length tests key rotation boundary */
        uint8_t src[13];
        for (int i = 0; i < 13; i++) src[i] = (uint8_t)(0xAA ^ i);
        test_dma_basic(fd, "odd length (13B)",  src, 13,            0x11223344);
    }
    {
        uint8_t *src = malloc(CRX_BAR1_SIZE);
        REQUIRE(src, "malloc");
        for (size_t i = 0; i < CRX_BAR1_SIZE; i++) src[i] = (uint8_t)i;
        test_dma_basic(fd, "max size (64KB)",   src, CRX_BAR1_SIZE, 0x01020304);
        free(src);
    }

    section("PATH 1: round-trip (encrypt twice = original)");
    test_dma_roundtrip(fd, 512,           0xDEADBEEF);
    test_dma_roundtrip(fd, 1024,          0x00000001);
    test_dma_roundtrip(fd, CRX_BAR1_SIZE, 0xBAADF00D);

    section("PATH 1: in-place (src == dst)");
    test_dma_inplace(fd, 128,  0xFEEDFACE);
    test_dma_inplace(fd, 1024, 0xABCDEF01);

    /* ------------------------------------------------------------------
     * PATH 2: decoupled write() / ioctl(no DMA) / read()
     * ------------------------------------------------------------------ */
    section("PATH 2: decoupled write / encrypt / read");
    {
        uint8_t src[256];
        for (int i = 0; i < 256; i++) src[i] = (uint8_t)(i ^ 0x55);
        test_pio_path(fd, src, 64,  0xDEADBEEF);
        test_pio_path(fd, src, 256, 0x12345678);
    }

    /* ------------------------------------------------------------------
     * Ancillary
     * ------------------------------------------------------------------ */
    section("Stats");
    test_stats(fd);

    section("Error conditions");
    test_errors(fd);

    section("mmap");
    test_mmap(fd);

    close(fd);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
