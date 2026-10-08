# Plan: Transparent Compression for tmpfs

## Rationale

### Why Transparent Compression for tmpfs?

Memory is the most constrained resource in modern systems. While CPU and storage have seen exponential improvements, RAM capacity grows slowly and is expensive. Many workloads store large amounts of compressible data in tmpfs:

- **Build caches**: Object files, dependency graphs, intermediate build artifacts
- **Package managers**: Downloaded packages, extracted archives
- **Docker/containers**: Image layers, temporary container storage
- **Web servers**: Cached responses, session data, uploaded files
- **Databases**: Temporary tables, query results, write-ahead logs
- **CI/CD systems**: Test artifacts, build outputs, log files
- **Development**: Compiler temporaries, test outputs, debug symbols

In these scenarios, 40-70% of tmpfs data is compressible, but conventional tmpfs stores it uncompressed, wasting valuable RAM.

### The Problem with Current Solutions

**Without compression:**
- tmpfs consumes RAM proportional to file size
- Memory pressure triggers swap (if configured) or OOM
- Users must manually limit tmpfs size or clean up frequently

**With zram (current approach):**
- zram reserves RAM upfront for compressed storage
- Fixed allocation regardless of actual compression ratio
- Adds complexity: separate block device, swap setup, formatting
- Not transparent: requires explicit mount and configuration
- Only works for swapped-out pages, not actively used tmpfs files

**With zswap:**
- Compresses pages during swapout (cold pages only)
- Hot tmpfs data remains uncompressed
- Still requires swap to be configured
- Compression happens too late (after memory pressure)

### Why This Approach is Different

Unlike zram and zswap, this implementation provides **proactive, transparent compression** that:

1. **Compresses actively used tmpfs files** - not just cold/swapped pages
2. **No upfront RAM reservation** - uses zsmalloc pools that grow on demand
3. **Transparent to applications** - no API changes, no code modifications
4. **Integrated into tmpfs** - not a separate block device or swap mechanism
5. **Configurable at runtime** - sysfs interface for tuning
6. **Preserves mmap support** - page-level compression maintains random access

## Comparison: This vs zram

### Architecture Differences

```
┌─────────────────────────────────────────────────────────────┐
│                    zram Approach                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   Application                                               │
│       │                                                     │
│       ▼                                                     │
│   tmpfs (uncompressed)                                      │
│       │                                                     │
│       │  Memory pressure / swap                             │
│       ▼                                                     │
│   Swap device (zram) ──► Block device layer                 │
│                               │                             │
│                               ▼                             │
│                          Compressed RAM pool                │
│                          (reserved upfront)                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│              tmpfs Compression Approach                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   Application                                               │
│       │                                                     │
│       ▼                                                     │
│   tmpfs (transparently compressed)                          │
│       │                                                     │
│       ├─► Page cache ──► xarray ──► Compressed entry        │
│       │                              │                      │
│       │                              ▼                      │
│       │                         zsmalloc pool               │
│       │                         (grows on demand)           │
│       │                                                     │
│       └─► mmap() ──► Page fault ──► Decompress on access   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Detailed Comparison

| Aspect | zram | tmpfs Compression |
|--------|------|-------------------|
| **When compression happens** | During swapout (cold pages) | On write or memory pressure (all pages) |
| **RAM reservation** | Yes - fixed allocation upfront | No - zsmalloc grows on demand |
| **Transparency** | Low - separate device, swap config | High - transparent to applications |
| **mmap support** | Indirect via swap | Native - decompress on page fault |
| **Hot data compression** | No - only cold/swapped pages | Yes - actively used files compressed |
| **Compression ratio tracking** | Limited | Full statistics via sysfs |
| **Configuration complexity** | High - mkswap, swapon, fstab | Low - mount option or sysfs |
| **Overhead** | Block device layer, I/O scheduling | Minimal - direct page cache access |
| **Failure mode** | Swap fails, system slows | Falls back to uncompressed |
| **Use case** | System-wide swap compression | Application-specific tmpfs |

### Memory Efficiency Comparison

**Scenario: 100MB tmpfs with 60% compressible data**

```
┌─────────────────────────────────────────────────────────────┐
│ zram (reserved 100MB for swap):                             │
├─────────────────────────────────────────────────────────────┤
│ Reserved RAM: 100MB (fixed)                                │
│ Actual usage: 40MB uncompressed + 24MB compressed = 64MB   │
│ Wasted reservation: 36MB (36% waste)                       │
│                                                             │
│ Total RAM consumed: 100MB (reserved)                        │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ tmpfs compression (on-demand):                              │
├─────────────────────────────────────────────────────────────┤
│ Reserved RAM: 0MB (no reservation)                         │
│ Actual usage: 40MB uncompressed + 24MB compressed = 64MB   │
│ Wasted reservation: 0MB                                    │
│                                                             │
│ Total RAM consumed: 64MB (actual)                           │
│ Savings vs zram: 36MB (36% less RAM used)                  │
└─────────────────────────────────────────────────────────────┘
```

### Latency Comparison

**Read access to compressed page:**

```
zram path:
  Application read() → Page fault → Swap in from zram → Block I/O → Decompress
  Latency: ~10-50μs (block device overhead)

This approach:
  Application read() → Page fault → Decompress from zsmalloc → Direct access
  Latency: ~2-5μs (no block device overhead)
```

**Write access to compressed page:**

```
zram path:
  Application write() → Write to tmpfs (uncompressed) → Later: swapout → Compress
  Compression happens: After memory pressure (too late)

This approach:
  Application write() → Write to tmpfs → Compress on writeback
  Compression happens: Proactively, during normal operation
```

### When to Use Which

**Use zram when:**
- You need system-wide swap compression
- You're compressing anonymous memory (process heaps)
- You want simple setup (just swap on zram device)
- You don't need hot data compression
- You're okay with fixed RAM reservation

**Use tmpfs compression when:**
- You're compressing tmpfs files specifically
- You want no RAM reservation
- You need hot data compression
- You need mmap support on compressed files
- You want transparent, application-agnostic compression
- You need runtime configuration and statistics

### Hybrid Approach

For maximum benefit, use both:

```bash
# zram for system swap (anonymous memory)
modprobe zram num_devices=1
echo 1G > /sys/block/zram0/disksize
mkswap /dev/zram0
swapon /dev/zram0

# tmpfs compression for file-backed storage
mount -t tmpfs -o compress=lz4,size=4G tmpfs /var/cache
mount -t tmpfs -o compress=lzo tmpfs /tmp/build
```

This gives you:
- zram: Compressed swap for anonymous memory (processes)
- tmpfs compression: Compressed file cache (build artifacts, packages)
- No RAM reservation for either
- Transparent to all applications

## Testing via QEMU with Static Initrd

### Test Environment Overview

QEMU provides an ideal environment for testing kernel features:

- **Isolated**: No impact on host system
- **Reproducible**: Same kernel + initrd = same environment
- **Fast iteration**: Boot in seconds, not minutes
- **Debuggable**: Serial console, gdb support, kernel logs
- **Configurable**: Easy to change kernel config, memory size, etc.

```
┌─────────────────────────────────────────────────────────────┐
│ QEMU Virtual Machine                                        │
├─────────────────────────────────────────────────────────────┤
│  Kernel: bzImage (with CONFIG_TMPFS_COMPRESSION=y)          │
│  Initrd: rootfs.cpio (static test programs)                 │
│  Memory: 512MB-2GB (configurable)                          │
│  Console: Serial (ttyS0) → host terminal                   │
│                                                             │
│  Test programs:                                             │
│  ├── /bin/test_compress      - Basic compression tests      │
│  ├── /bin/test_mmap          - mmap() tests                 │
│  ├── /bin/test_performance   - Benchmarks                   │
│  ├── /bin/test_concurrent    - Multi-threaded tests         │
│  └── /bin/run_all_tests      - Test suite runner            │
└─────────────────────────────────────────────────────────────┘
        │
        ▼ (serial console)
┌─────────────────────────────────────────────────────────────┐
│ Host                                                        │
│  - QEMU process                                             │
│  - Serial output capture                                    │
│  - Optional: 9p filesystem for file transfer               │
└─────────────────────────────────────────────────────────────┘
```

### Phase 1: Build Environment Setup

#### 1.1 Create Test Directory Structure

```bash
#!/bin/bash
# setup_test_env.sh - Create test directory structure

TEST_ROOT="/home/snide/tmpfs-compress-test"
KERNEL_SRC="/home/snide/linux"

mkdir -p ${TEST_ROOT}/{kernel,initrd,tests,results,scripts}
cd ${TEST_ROOT}

echo "Test environment created at ${TEST_ROOT}"
echo "Kernel source: ${KERNEL_SRC}"
```

#### 1.2 Build Kernel with Compression Support

```bash
#!/bin/bash
# build_kernel.sh - Build kernel with tmpfs compression

KERNEL_SRC="/home/snide/linux"
BUILD_DIR="/home/snide/tmpfs-compress-test/kernel"
CROSS_COMPILE=""  # Use native x86_64

cd ${KERNEL_SRC}

# Start with existing config or defconfig
cp .config ${BUILD_DIR}/.config 2>/dev/null || make defconfig

# Enable required options
./scripts/config --file ${BUILD_DIR}/.config \
    --enable CONFIG_TMPFS \
    --enable CONFIG_TMPFS_XATTR \
    --enable CONFIG_TMPFS_QUOTA \
    --enable CONFIG_TMPFS_COMPRESSION \
    --enable CONFIG_TMPFS_COMPRESSOR_DEFAULT \
    --set-str CONFIG_TMPFS_COMPRESSOR_DEFAULT "lz4" \
    --enable CONFIG_CRYPTO \
    --enable CONFIG_CRYPTO_LZ4 \
    --enable CONFIG_CRYPTO_LZO \
    --enable CONFIG_CRYPTO_LZO_RLE \
    --enable CONFIG_CRYPTO_ZSTD \
    --enable CONFIG_ZSMALLOC \
    --enable CONFIG_ZSMALLOC_PIN \
    --enable CONFIG_SWAP \
    --enable CONFIG_ZSWAP \
    --enable CONFIG_SERIAL_8250 \
    --enable CONFIG_SERIAL_8250_CONSOLE \
    --enable CONFIG_SERIAL_8250_NR_UARTS \
    --enable CONFIG_SERIAL_8250_RUNTIME_UARTS \
    --enable CONFIG_DEVTMPFS \
    --enable CONFIG_DEVTMPFS_MOUNT \
    --enable CONFIG_BLK_DEV_INITRD \
    --enable CONFIG_RD_GZIP \
    --enable CONFIG_RD_LZ4 \
    --enable CONFIG_BINFMT_ELF \
    --enable CONFIG_BINFMT_SCRIPT \
    --enable CONFIG_PROC_FS \
    --enable CONFIG_SYSFS \
    --enable CONFIG_TMPFS_POSIX_ACL \
    --enable CONFIG_DEBUG_FS \
    --enable CONFIG_KALLSYMS \
    --enable CONFIG_KALLSYMS_ALL \
    --enable CONFIG_PRINTK \
    --enable CONFIG_SERIAL_8250_CONSOLE

# Build kernel
cd ${KERNEL_SRC}
make -j$(nproc) ARCH=x86_64 LOCALVERSION="-tmpfs-compress"

cp arch/x86/boot/bzImage ${BUILD_DIR}/bzImage
cp .config ${BUILD_DIR}/.config

echo "Kernel built successfully: ${BUILD_DIR}/bzImage"
```

#### 1.3 Build Static Test Programs

```bash
#!/bin/bash
# build_tests.sh - Build static test programs for initrd

TEST_SRC="/home/snide/tmpfs-compress-test/tests"
INITRD_ROOT="/home/snide/tmpfs-compress-test/initrd"

mkdir -p ${INITRD_ROOT}/{bin,dev,proc,sys,tmp,mnt,testdata}

# Compile test programs statically
cd ${TEST_SRC}

for test in test_compress test_mmap test_performance test_concurrent run_all_tests; do
    gcc -static -O2 -o ${INITRD_ROOT}/bin/${test} ${test}.c \
        -lpthread -lm
    echo "Built: ${INITRD_ROOT}/bin/${test}"
done

# Copy busybox for shell and basic utilities
cp /bin/busybox ${INITRD_ROOT}/bin/busybox
for cmd in sh mount umount ls cat echo grep mkdir rm dmesg; do
    ln -sf busybox ${INITRD_ROOT}/bin/${cmd}
done

echo "Test programs built successfully"
```

### Phase 2: Static Initrd Creation

#### 2.1 Minimal Root Filesystem Structure

```
initrd/
├── bin/                    # Static test programs
│   ├── busybox            # Multi-call binary
│   ├── test_compress      # Compression tests
│   ├── test_mmap          # mmap() tests
│   ├── test_performance   # Benchmarks
│   ├── test_concurrent    # Multi-threaded tests
│   └── run_all_tests      # Test suite runner
├── dev/                    # Device nodes
│   ├── console
│   ├── null
│   ├── zero
│   └── ttyS0
├── proc/                   # Proc filesystem mount point
├── sys/                    # Sysfs mount point
├── tmp/                    # Default tmpfs mount point
├── mnt/                    # Test mount points
│   ├── tmpfs_compress
│   ├── tmpfs_no_compress
│   └── tmpfs_lzo
├── testdata/              # Test data files
│   ├── random.bin        # Incompressible data
│   ├── zeros.bin         # Highly compressible
│   ├── text.txt          # Text data
│   └── binary.bin        # Mixed binary
└── init                    # Init script (PID 1)
```

#### 2.2 Init Script

```bash
#!/bin/sh
# init - PID 1 script for test initrd

# Mount essential filesystems
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || {
    mkdir -p /dev
    mknod /dev/console c 5 1
    mknod /dev/null c 1 3
    mknod /dev/zero c 1 5
    mknod /dev/ttyS0 c 4 64
}

# Mount debugfs for kernel debugging
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null

# Clear dmesg buffer for clean test output
dmesg -c > /dev/null 2>&1

echo "=========================================="
echo "  tmpfs Compression Test Environment"
echo "=========================================="
echo ""

# Display kernel info
echo "Kernel: $(uname -r)"
echo "Memory: $(free -m | grep Mem | awk '{print $2}')MB"
echo ""

# Check if tmpfs compression is available
echo "Checking tmpfs compression support..."
if [ -f /sys/fs/tmpfs/compressor ]; then
    echo "  ✓ Compression sysfs interface found"
    echo "  Current compressor: $(cat /sys/fs/tmpfs/compressor)"
    echo "  Compression enabled: $(cat /sys/fs/tmpfs/compression)"
else
    echo "  ✗ Compression sysfs interface not found"
    echo "  Kernel may not have CONFIG_TMPFS_COMPRESSION=y"
fi
echo ""

# Check for compression modules/features
echo "Kernel compression features:"
grep -E "CONFIG_TMPFS_COMPRESSION|CONFIG_CRYPTO_LZ4|CONFIG_CRYPTO_LZO|CONFIG_ZSMALLOC" /proc/config.gz 2>/dev/null | gunzip | grep -E "=y|=m" || \
    zcat /proc/config.gz 2>/dev/null | grep -E "CONFIG_TMPFS_COMPRESSION|CONFIG_CRYPTO_LZ4|CONFIG_CRYPTO_LZO|CONFIG_ZSMALLOC" | grep -E "=y|=m" || \
    echo "  (config.gz not available)"
echo ""

# Run test suite
echo "Starting test suite..."
echo ""

/bin/run_all_tests
TEST_EXIT=$?

echo ""
echo "=========================================="
echo "  Test suite completed with exit code: ${TEST_EXIT}"
echo "=========================================="

# If TESTS_AUTOMATED=1, shutdown after tests
if [ "${TESTS_AUTOMATED}" = "1" ]; then
    echo ""
    echo "Automated mode: shutting down..."
    sleep 1
    poweroff -f
else
    echo ""
    echo "Dropping to shell for interactive testing..."
    echo "Available commands: test_compress, test_mmap, test_performance"
    echo ""
    exec /bin/sh
fi
```

#### 2.3 Create Initrd Image

```bash
#!/bin/bash
# build_initrd.sh - Create compressed initrd image

INITRD_ROOT="/home/snide/tmpfs-compress-test/initrd"
INITRD_IMAGE="/home/snide/tmpfs-compress-test/rootfs.cpio.gz"

# Create device nodes if not present
mkdir -p ${INITRD_ROOT}/dev
mknod -m 622 ${INITRD_ROOT}/dev/console c 5 1 2>/dev/null
mknod -m 666 ${INITRD_ROOT}/dev/null c 1 3 2>/dev/null
mknod -m 666 ${INITRD_ROOT}/dev/zero c 1 5 2>/dev/null
mknod -m 666 ${INITRD_ROOT}/dev/ttyS0 c 4 64 2>/dev/null
mknod -m 666 ${INITRD_ROOT}/dev/random c 1 8 2>/dev/null
mknod -m 666 ${INITRD_ROOT}/dev/urandom c 1 9 2>/dev/null

# Create test data files
echo "Creating test data..."
dd if=/dev/urandom of=${INITRD_ROOT}/testdata/random.bin bs=1M count=10 2>/dev/null
dd if=/dev/zero of=${INITRD_ROOT}/testdata/zeros.bin bs=1M count=10 2>/dev/null
cat > ${INITRD_ROOT}/testdata/text.txt << 'EOF'
This is a test file with repetitive text content.
This is a test file with repetitive text content.
This is a test file with repetitive text content.
This is a test file with repetitive text content.
This is a test file with repetitive text content.
EOF
dd if=/dev/urandom bs=1K count=100 2>/dev/null | cat - ${INITRD_ROOT}/testdata/text.txt > ${INITRD_ROOT}/testdata/binary.bin

# Create initrd image
cd ${INITRD_ROOT}
find . | cpio -o -H newc 2>/dev/null | gzip -9 > ${INITRD_IMAGE}

echo "Initrd created: ${INITRD_IMAGE}"
echo "Size: $(du -h ${INITRD_IMAGE} | cut -f1)"
```

### Phase 3: Test Programs

#### 3.1 Basic Compression Test

```c
// test_compress.c - Basic compression tests
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>

#define TEST_DIR "/mnt/tmpfs_compress"
#define TEST_FILE TEST_DIR "/test_file.dat"
#define TEST_SIZE (10 * 1024 * 1024)  // 10MB

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  ✓ %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  ✗ %s (failed)\n", msg); \
        tests_failed++; \
    } \
} while(0)

void create_test_file(const char *path, int type, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }
    
    char *buf = malloc(4096);
    size_t written = 0;
    
    while (written < size) {
        size_t to_write = size - written;
        if (to_write > 4096) to_write = 4096;
        
        switch (type) {
            case 0:  // Zeros (highly compressible)
                memset(buf, 0, to_write);
                break;
            case 1:  // Random (incompressible)
                for (size_t i = 0; i < to_write; i++)
                    buf[i] = rand() & 0xFF;
                break;
            case 2:  // Text (compressible)
                for (size_t i = 0; i < to_write; i++)
                    buf[i] = 'A' + (rand() % 26);
                break;
            case 3:  // Repetitive pattern (very compressible)
                for (size_t i = 0; i < to_write; i++)
                    buf[i] = (i / 100) & 0xFF;
                break;
        }
        
        if (write(fd, buf, to_write) != (ssize_t)to_write) {
            perror("write");
            close(fd);
            exit(1);
        }
        written += to_write;
    }
    
    close(fd);
    free(buf);
}

long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return st.st_size;
}

long get_compressed_pages(void) {
    FILE *f = fopen("/sys/fs/tmpfs/comp_stats", "r");
    if (!f) return -1;
    
    char line[256];
    long pages = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "compressed_pages: %ld", &pages) == 1)
            break;
    }
    fclose(f);
    return pages;
}

void test_basic_compression(void) {
    printf("\n[TEST] Basic compression functionality\n");
    printf("----------------------------------------\n");
    
    // Mount tmpfs with compression
    mkdir(TEST_DIR, 0755);
    int ret = mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
    ASSERT(ret == 0, "Mount tmpfs with compression");
    if (ret < 0) return;
    
    // Check compression is enabled
    FILE *f = fopen("/sys/fs/tmpfs/compression", "r");
    if (f) {
        int enabled = 0;
        fscanf(f, "%d", &enabled);
        fclose(f);
        ASSERT(enabled == 1, "Compression is enabled");
    }
    
    // Test 1: Write compressible data
    printf("\n  Writing compressible data (zeros)...\n");
    create_test_file(TEST_FILE, 0, TEST_SIZE);
    ASSERT(get_file_size(TEST_FILE) == TEST_SIZE, "File size correct");
    
    long pages_before = get_compressed_pages();
    printf("  Compressed pages before read: %ld\n", pages_before);
    
    // Read file to trigger decompression
    int fd = open(TEST_FILE, O_RDONLY);
    ASSERT(fd >= 0, "Open file for reading");
    if (fd >= 0) {
        char buf[4096];
        size_t total = 0;
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            total += n;
        close(fd);
        ASSERT(total == TEST_SIZE, "Read all data back");
        
        long pages_after = get_compressed_pages();
        printf("  Compressed pages after read: %ld\n", pages_after);
    }
    
    // Test 2: Write incompressible data
    printf("\n  Writing incompressible data (random)...\n");
    create_test_file(TEST_FILE, 1, TEST_SIZE);
    ASSERT(get_file_size(TEST_FILE) == TEST_SIZE, "File size correct (random)");
    
    // Test 3: Write compressible text
    printf("\n  Writing compressible text...\n");
    create_test_file(TEST_FILE, 2, TEST_SIZE);
    ASSERT(get_file_size(TEST_FILE) == TEST_SIZE, "File size correct (text)");
    
    umount(TEST_DIR);
}

void test_mmap_compressed(void) {
    printf("\n[TEST] mmap() with compressed files\n");
    printf("------------------------------------\n");
    
    mkdir(TEST_DIR, 0755);
    mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
    
    // Create compressible file
    create_test_file(TEST_FILE, 3, TEST_SIZE);
    
    // mmap the file
    int fd = open(TEST_FILE, O_RDONLY);
    ASSERT(fd >= 0, "Open file for mmap");
    if (fd >= 0) {
        void *mapped = mmap(NULL, TEST_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
        ASSERT(mapped != MAP_FAILED, "mmap() succeeds");
        
        if (mapped != MAP_FAILED) {
            // Access memory to trigger decompression
            volatile char *p = mapped;
            char sum = 0;
            for (size_t i = 0; i < TEST_SIZE; i += 4096)
                sum += p[i];
            
            ASSERT(1, "mmap access triggers decompression");
            printf("  Accessed %zu pages, checksum: %d\n", 
                   TEST_SIZE / 4096, sum);
            
            munmap(mapped, TEST_SIZE);
        }
        close(fd);
    }
    
    umount(TEST_DIR);
}

void test_compression_ratio(void) {
    printf("\n[TEST] Compression ratio measurement\n");
    printf("-------------------------------------\n");
    
    mkdir(TEST_DIR, 0755);
    mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
    
    // Test different data types
    const char *data_types[] = {"zeros", "random", "text", "pattern"};
    
    for (int i = 0; i < 4; i++) {
        printf("\n  Data type: %s\n", data_types[i]);
        create_test_file(TEST_FILE, i, TEST_SIZE);
        
        long size = get_file_size(TEST_FILE);
        printf("  File size: %ld bytes\n", size);
        
        // Trigger compression by reading and writing
        int fd = open(TEST_FILE, O_RDWR);
        if (fd >= 0) {
            char buf[4096];
            lseek(fd, 0, SEEK_SET);
            read(fd, buf, sizeof(buf));
            lseek(fd, 0, SEEK_SET);
            write(fd, buf, sizeof(buf));
            close(fd);
        }
        
        // Check stats
        FILE *f = fopen("/sys/fs/tmpfs/comp_stats", "r");
        if (f) {
            char line[256];
            long comp_pages = 0, uncomp_pages = 0;
            long comp_bytes = 0, uncomp_bytes = 0;
            
            while (fgets(line, sizeof(line), f)) {
                sscanf(line, "compressed_pages: %ld", &comp_pages);
                sscanf(line, "uncompressed_pages: %ld", &uncomp_pages);
                sscanf(line, "total_compressed_bytes: %ld", &comp_bytes);
                sscanf(line, "total_uncompressed_bytes: %ld", &uncomp_bytes);
            }
            fclose(f);
            
            if (uncomp_bytes > 0) {
                double ratio = (double)comp_bytes / uncomp_bytes * 100;
                printf("  Compression ratio: %.2f%%\n", ratio);
                printf("  Compressed: %ld bytes, Original: %ld bytes\n",
                       comp_bytes, uncomp_bytes);
            }
        }
    }
    
    umount(TEST_DIR);
}

int main(int argc, char *argv[]) {
    printf("tmpfs Compression Test Suite\n");
    printf("============================\n");
    
    srand(time(NULL));
    
    test_basic_compression();
    test_mmap_compressed();
    test_compression_ratio();
    
    printf("\n============================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("============================\n");
    
    return tests_failed > 0 ? 1 : 0;
}
```

#### 3.2 Performance Benchmark Test

```c
// test_performance.c - Performance benchmarks
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>

#define TEST_DIR "/mnt/perf_test"
#define ITERATIONS 100

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void benchmark_write(int size, const char *data_type, int compress) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_%d_%s.dat", 
             TEST_DIR, size, compress ? "comp" : "nocomp");
    
    char *buf = malloc(size);
    
    // Fill buffer based on type
    if (strcmp(data_type, "zeros") == 0)
        memset(buf, 0, size);
    else if (strcmp(data_type, "random") == 0) {
        for (int i = 0; i < size; i++)
            buf[i] = rand() & 0xFF;
    } else {
        for (int i = 0; i < size; i++)
            buf[i] = 'A' + (rand() % 26);
    }
    
    double start = get_time_ms();
    
    for (int i = 0; i < ITERATIONS; i++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, buf, size);
            close(fd);
        }
        unlink(path);
    }
    
    double end = get_time_ms();
    double avg_ms = (end - start) / ITERATIONS;
    
    printf("  Write %dKB %s (%s): %.2f ms avg\n", 
           size / 1024, data_type, compress ? "compressed" : "plain",
           avg_ms);
    
    free(buf);
}

void benchmark_read(int size, const char *data_type, int compress) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_read_%d_%s.dat",
             TEST_DIR, size, compress ? "comp" : "nocomp");
    
    // Create test file
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *buf = malloc(size);
    if (strcmp(data_type, "zeros") == 0)
        memset(buf, 0, size);
    else if (strcmp(data_type, "random") == 0) {
        for (int i = 0; i < size; i++)
            buf[i] = rand() & 0xFF;
    } else {
        for (int i = 0; i < size; i++)
            buf[i] = 'A' + (rand() % 26);
    }
    write(fd, buf, size);
    close(fd);
    
    // Benchmark read
    double start = get_time_ms();
    
    for (int i = 0; i < ITERATIONS; i++) {
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, size);
            close(fd);
        }
    }
    
    double end = get_time_ms();
    double avg_ms = (end - start) / ITERATIONS;
    
    printf("  Read  %dKB %s (%s): %.2f ms avg\n",
           size / 1024, data_type, compress ? "compressed" : "plain",
           avg_ms);
    
    unlink(path);
    free(buf);
}

void benchmark_mmap(int size, const char *data_type, int compress) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_mmap_%d_%s.dat",
             TEST_DIR, size, compress ? "comp" : "nocomp");
    
    // Create test file
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *buf = malloc(size);
    if (strcmp(data_type, "zeros") == 0)
        memset(buf, 0, size);
    else if (strcmp(data_type, "random") == 0) {
        for (int i = 0; i < size; i++)
            buf[i] = rand() & 0xFF;
    } else {
        for (int i = 0; i < size; i++)
            buf[i] = 'A' + (rand() % 26);
    }
    write(fd, buf, size);
    close(fd);
    
    // Benchmark mmap
    double start = get_time_ms();
    
    for (int i = 0; i < ITERATIONS; i++) {
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                // Touch every page
                volatile char *p = mapped;
                char sum = 0;
                for (int j = 0; j < size; j += 4096)
                    sum += p[j];
                munmap(mapped, size);
            }
            close(fd);
        }
    }
    
    double end = get_time_ms();
    double avg_ms = (end - start) / ITERATIONS;
    
    printf("  mmap  %dKB %s (%s): %.2f ms avg\n",
           size / 1024, data_type, compress ? "compressed" : "plain",
           avg_ms);
    
    unlink(path);
    free(buf);
}

int main(void) {
    printf("tmpfs Compression Performance Benchmarks\n");
    printf("==========================================\n");
    
    // Mount tmpfs for testing
    mkdir(TEST_DIR, 0755);
    
    int sizes[] = {4096, 16384, 65536, 262144, 1048576};
    const char *types[] = {"zeros", "random", "text"};
    
    printf("\n--- Write Benchmarks ---\n");
    for (int s = 0; s < 5; s++) {
        for (int t = 0; t < 3; t++) {
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
            benchmark_write(sizes[s], types[t], 1);
            umount(TEST_DIR);
            
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "");
            benchmark_write(sizes[s], types[t], 0);
            umount(TEST_DIR);
        }
    }
    
    printf("\n--- Read Benchmarks ---\n");
    for (int s = 0; s < 5; s++) {
        for (int t = 0; t < 3; t++) {
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
            benchmark_read(sizes[s], types[t], 1);
            umount(TEST_DIR);
            
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "");
            benchmark_read(sizes[s], types[t], 0);
            umount(TEST_DIR);
        }
    }
    
    printf("\n--- mmap Benchmarks ---\n");
    for (int s = 0; s < 5; s++) {
        for (int t = 0; t < 3; t++) {
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "compress");
            benchmark_mmap(sizes[s], types[t], 1);
            umount(TEST_DIR);
            
            mount("tmpfs", TEST_DIR, "tmpfs", 0, "");
            benchmark_mmap(sizes[s], types[t], 0);
            umount(TEST_DIR);
        }
    }
    
    return 0;
}
```

### Phase 4: QEMU Launch Scripts

#### 4.1 Basic QEMU Launcher

```bash
#!/bin/bash
# run_qemu.sh - Launch QEMU with test initrd

QEMU="/usr/bin/qemu-system-x86_64"
KERNEL="/home/snide/tmpfs-compress-test/kernel/bzImage"
INITRD="/home/snide/tmpfs-compress-test/rootfs.cpio.gz"
RESULTS="/home/snide/tmpfs-compress-test/results"
MEMORY="1G"
CPUS="2"
SERIAL_LOG="${RESULTS}/serial_$(date +%Y%m%d_%H%M%S).log"

mkdir -p ${RESULTS}

echo "Starting QEMU test environment..."
echo "  Kernel: ${KERNEL}"
echo "  Initrd: ${INITRD}"
echo "  Memory: ${MEMORY}"
echo "  CPUs: ${CPUS}"
echo "  Serial log: ${SERIAL_LOG}"
echo ""

# Run QEMU
${QEMU} \
    -kernel ${KERNEL} \
    -initrd ${INITRD} \
    -m ${MEMORY} \
    -smp ${CPUS} \
    -nographic \
    -append "console=ttyS0 earlyprintk=serial,ttyS0,115200 ignore_loglevel" \
    -serial file:${SERIAL_LOG} \
    -display none \
    -no-reboot \
    -monitor none \
    -append "rdinit=/init"

EXIT_CODE=$?

echo ""
echo "QEMU exited with code: ${EXIT_CODE}"
echo "Serial log saved to: ${SERIAL_LOG}"

# Parse results from log
echo ""
echo "Extracting test results..."
grep -E "Results:.*passed.*failed|✓|✗" ${SERIAL_LOG} | tail -20
```

#### 4.2 Automated Test Runner

```bash
#!/bin/bash
# run_tests.sh - Automated test suite runner

RESULTS="/home/snide/tmpfs-compress-test/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TEST_RESULTS="${RESULTS}/test_results_${TIMESTAMP}.txt"

mkdir -p ${RESULTS}

echo "========================================" | tee ${TEST_RESULTS}
echo "tmpfs Compression Automated Test Suite" | tee -a ${TEST_RESULTS}
echo "Timestamp: ${TIMESTAMP}" | tee -a ${TEST_RESULTS}
echo "========================================" | tee -a ${TEST_RESULTS}
echo "" | tee -a ${TEST_RESULTS}

# Test 1: Basic functionality
echo "[TEST 1] Basic Compression Functionality" | tee -a ${TEST_RESULTS}
./run_qemu.sh 2>&1 | tee -a ${TEST_RESULTS}
echo "" | tee -a ${TEST_RESULTS}

# Extract summary
echo "[SUMMARY]" | tee -a ${TEST_RESULTS}
grep "Results:.*passed.*failed" ${RESULTS}/serial_*.log | tail -1 | tee -a ${TEST_RESULTS}
echo "" | tee -a ${TEST_RESULTS}

echo "Full serial logs available in: ${RESULTS}/" | tee -a ${TEST_RESULTS}
echo "Test results saved to: ${TEST_RESULTS}" | tee -a ${TEST_RESULTS}
```

#### 4.3 Interactive Debugging Session

```bash
#!/bin/bash
# debug_session.sh - Interactive QEMU session for debugging

QEMU="/usr/bin/qemu-system-x86_64"
KERNEL="/home/snide/tmpfs-compress-test/kernel/bzImage"
INITRD="/home/snide/tmpfs-compress-test/rootfs.cpio.gz"
MEMORY="2G"

# Check if kernel has debug symbols
if [ -f "${KERNEL}.debug" ]; then
    echo "Kernel debug symbols found"
    echo "GDB remote debugging: gdb -ex 'target remote :1234'"
    GDB_PORT="-s -S"
else
    echo "No debug symbols (run 'make debuginfo' for GDB support)"
    GDB_PORT=""
fi

echo ""
echo "Starting interactive QEMU session..."
echo "Commands will be available in the guest shell"
echo "Press Ctrl-A X to exit QEMU"
echo ""

${QEMU} \
    -kernel ${KERNEL} \
    -initrd ${INITRD} \
    -m ${MEMORY} \
    -smp 2 \
    -nographic \
    -append "console=ttyS0 ignore_loglevel" \
    -serial stdio \
    -display none \
    ${GDB_PORT}
```

### Phase 5: Test Data Generation

```bash
#!/bin/bash
# generate_test_data.sh - Create various test data files

TEST_DATA="/home/snide/tmpfs-compress-test/initrd/testdata"
mkdir -p ${TEST_DATA}

# Zeros (highly compressible)
dd if=/dev/zero of=${TEST_DATA}/zeros_10MB.bin bs=1M count=10 2>/dev/null

# Random (incompressible)
dd if=/dev/urandom of=${TEST_DATA}/random_10MB.bin bs=1M count=10 2>/dev/null

# Text (compressible)
for i in $(seq 1 100000); do
    echo "Line $i: The quick brown fox jumps over the lazy dog."
done > ${TEST_DATA}/text_10MB.txt

# Repetitive pattern (very compressible)
python3 -c "
import struct
data = b'\x00\x01\x02\x03' * (2.5 * 1024 * 1024)
open('${TEST_DATA}/pattern_10MB.bin', 'wb').write(data)
" 2>/dev/null || \
    dd if=/dev/zero bs=1M count=10 2>/dev/null | \
    tr '\0' '\x01\x02\x03\x04' > ${TEST_DATA}/pattern_10MB.bin

# Binary with structure (moderately compressible)
for i in $(seq 1 50000); do
    printf "HEADER:%08x DATA:%s\n" $i "$(head -c 100 /dev/urandom | xxd -p)"
done > ${TEST_DATA}/binary_10MB.dat

# Sparse file (mostly zeros with occasional data)
python3 -c "
import os
with open('${TEST_DATA}/sparse_10MB.bin', 'wb') as f:
    for i in range(100):
        f.seek(i * 100000)
        f.write(os.urandom(1000))
" 2>/dev/null || \
    echo "Python3 not available, skipping sparse file"

echo "Test data files created:"
ls -lh ${TEST_DATA}/
```

### Phase 6: Debugging and Troubleshooting

#### 6.1 Kernel Debug Output

Add debug prints to compression code:

```c
// In mm/shmem_compress.c

#ifdef CONFIG_TMPFS_COMPRESSION_DEBUG
#define comp_dbg(fmt, ...) \
    pr_debug("shmem_comp: " fmt "\n", ##__VA_ARGS__)
#else
#define comp_dbg(fmt, ...) \
    pr_debug("shmem_comp: " fmt "\n", ##__VA_ARGS__)
#endif

// Usage:
comp_dbg("compressing page %lu: src=%p dst=%p", index, src, dst);
comp_dbg("compressed %u -> %u bytes", src_len, *dst_len);
comp_dbg("decompressing handle=%lu len=%u", handle, len);
```

Enable debug in kernel command line:
```
console=ttyS0 ignore_loglevel dyndbg="file mm/shmem_compress.c +p"
```

#### 6.2 Sysfs Debug Interface

```c
// Add to mm/shmem.c

static ssize_t comp_debug_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    struct shmem_comp_debug_info info;
    
    shmem_comp_get_debug_info(&info);
    
    return sysfs_emit(buf,
        "pools: %d\n"
        "active_pools: %d\n"
        "total_allocated_bytes: %lu\n"
        "total_compressed_pages: %lu\n"
        "pending_compressions: %lu\n"
        "worker_threads: %d\n"
        "last_error: %d (%s)\n",
        info.nr_pools,
        info.active_pools,
        info.total_allocated,
        info.compressed_pages,
        info.pending,
        info.workers,
        info.last_error,
        info.last_error_str);
}
TMPFS_ATTR_RO(comp_debug);
```

#### 6.3 Performance Tracing

```c
// Add tracepoints for performance analysis

TRACE_EVENT(shmem_comp_compress,
    TP_PROTO(unsigned long ino, pgoff_t index, \
             unsigned int src_len, unsigned int dst_len, \
             int ret, u64 duration_ns),
    
    TP_ARGS(ino, index, src_len, dst_len, ret, duration_ns),
    
    TP_STRUCT__entry(
        __field(unsigned long, ino)
        __field(pgoff_t, index)
        __field(unsigned int, src_len)
        __field(unsigned int, dst_len)
        __field(int, ret)
        __field(u64, duration_ns)
    ),
    
    TP_fast_assign(
        __entry->ino = ino;
        __entry->index = index;
        __entry->src_len = src_len;
        __entry->dst_len = dst_len;
        __entry->ret = ret;
        __entry->duration_ns = duration_ns;
    ),
    
    TP_printk("ino=%lu index=%lu %u->%u ret=%d time=%llums",
              __entry->ino, __entry->index,
              __entry->src_len, __entry->dst_len,
              __entry->ret, __entry->duration_ns / 1000000)
);
```

Use with trace-cmd or ftrace:
```bash
# In guest shell
echo 1 > /sys/kernel/debug/tracing/events/shmem/enable
cat /sys/kernel/debug/tracing/trace_pipe
```

### Phase 7: Continuous Integration

#### 7.1 CI Pipeline Script

```yaml
# .github/workflows/tmpfs-compression.yml
name: tmpfs-compression-tests

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  build-and-test:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Setup build environment
      run: |
        sudo apt-get update
        sudo apt-get install -y \
          build-essential libncurses-dev bison flex \
          libssl-dev libelf-dev qemu-system-x86
    
    - name: Build kernel
      run: |
        ./scripts/build_kernel.sh
    
    - name: Build test programs
      run: |
        ./scripts/build_tests.sh
    
    - name: Create initrd
      run: |
        ./scripts/build_initrd.sh
    
    - name: Run tests
      run: |
        ./scripts/run_tests.sh
    
    - name: Upload results
      if: always()
      uses: actions/upload-artifact@v2
      with:
        name: test-results
        path: results/
```

#### 7.2 Performance Regression Detection

```bash
#!/bin/bash
# check_performance.sh - Detect performance regressions

BASELINE="/home/snide/tmpfs-compress-test/results/baseline.txt"
CURRENT="/home/snide/tmpfs-compress-test/results/current.txt"
THRESHOLD=20  # 20% tolerance

if [ ! -f "${BASELINE}" ]; then
    echo "No baseline found, creating one..."
    cp ${CURRENT} ${BASELINE}
    exit 0
fi

echo "Comparing performance..."
echo "Baseline: ${BASELINE}"
echo "Current:  ${CURRENT}"
echo ""

REGRESSIONS=0

while IFS= read -r line; do
    # Parse benchmark results
    if [[ "${line}" =~ Write.*ms.*avg ]]; then
        baseline_time=$(grep -oP '\d+\.\d+' <<< "${line}" | head -1)
        current_line=$(grep -F "${line%%:*}" ${CURRENT} | head -1)
        current_time=$(grep -oP '\d+\.\d+' <<< "${current_line}" | head -1)
        
        if [ -n "${baseline_time}" ] && [ -n "${current_time}" ]; then
            diff=$(echo "${current_time} ${baseline_time}" | awk '{print ($1/$2 - 1) * 100}')
            if (( $(echo "${diff} > ${THRESHOLD}" | bc -l) )); then
                echo "REGRESSION: ${line}"
                echo "  Baseline: ${baseline_time}ms, Current: ${current_time}ms"
                echo "  Regression: ${diff}%"
                REGRESSIONS=$((REGRESSIONS + 1))
            fi
        fi
    fi
done < "${CURRENT}"

if [ ${REGRESSIONS} -gt 0 ]; then
    echo ""
    echo "Found ${REGRESSIONS} performance regressions"
    exit 1
else
    echo "No performance regressions detected"
    exit 0
fi
```

## Implementation Plan

## Overview

Add transparent page-level compression to tmpfs, allowing files to be compressed in-memory without reserving RAM. Uses LZ4/LZO via kernel crypto API, stored in zsmalloc pools.

## Goals

1. Transparent compression for tmpfs files
2. No RAM reservation (unlike zram)
3. Page-level compression (4KB granularity)
4. mmap support (decompress on page fault)
5. Compatible with existing shmem infrastructure
6. Runtime configuration via sysfs
7. Per-mount and per-file control

## Non-Goals

- Whole-file compression (breaks mmap)
- Compression of kernel-internal shmem (only tmpfs files)
- Swap integration (separate feature)

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Userspace                              │
├─────────────────────────────────────────────────────────────┤
│  read() / write() / mmap() / fallocate()                    │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    VFS / Page Cache                         │
│  filemap_get_entry() → xarray lookup                        │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Entry Type:                                          │   │
│  │  - Folio pointer (normal uncompressed page)          │   │
│  │  - Swap entry (xa_mk_value) → existing swap          │   │
│  │  - Compressed entry (xa_mk_value) → NEW              │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            │
              ┌─────────────┴─────────────┐
              ▼                           ▼
┌───────────────────────┐     ┌───────────────────────┐
│    Write Path         │     │     Read Path         │
│                       │     │                       │
│ shmem_write_end()     │     │ shmem_get_folio()     │
│   ↓                   │     │   ↓                   │
│ [Optional compress]   │     │ xa_is_shmem_comp()?   │
│   ↓                   │     │   ↓ Yes               │
│ shmem_compress_folio()│     │ shmem_decompress()    │
│   ↓                   │     │   ↓                   │
│ zs_malloc()           │     │ zs_obj_read()         │
│   ↓                   │     │   ↓                   │
│ Replace xarray entry  │     │ crypto_acomp_decompress│
└───────────────────────┘     └───────────────────────┘
              │                           │
              └─────────────┬─────────────┘
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   Compression Pool                          │
│                                                             │
│  struct shmem_comp_pool {                                   │
│      struct crypto_acomp *acomp;    // LZ4/LZO instance     │
│      struct zs_pool *storage;       // zsmalloc pool        │
│      struct percpu_struct *pcpu;    // Per-CPU contexts     │
│  };                                                         │
│                                                             │
│  Per-CPU context (16KB for LZ4):                            │
│  struct shmem_comp_pcpu {                                   │
│      struct crypto_acomp *acomp;                            │
│      struct acomp_req *req;                                 │
│      struct mutex lock;                                     │
│      char *buffer;              // PAGE_SIZE output         │
│  };                                                         │
└─────────────────────────────────────────────────────────────┘
```

## Implementation Phases

### Phase 1: Infrastructure (Foundation)

**Files to create:**
- `mm/shmem_compress.c` - Compression pool management
- `include/linux/shmem_compress.h` - Internal structures

**Files to modify:**
- `include/linux/shmem_fs.h` - Add compression fields to shmem_sb_info
- `mm/Kconfig` - Add CONFIG_TMPFS_COMPRESSION
- `mm/Makefile` - Add shmem_compress.o

**Tasks:**
1. [ ] Define `struct shmem_comp_pool` and per-CPU context
2. [ ] Implement pool create/destroy functions
3. [ ] Implement compression/decompression using crypto_acomp
4. [ ] Add zsmalloc integration for storage
5. [ ] Define compressed entry encoding for xarray

### Phase 2: Page Cache Integration

**Files to modify:**
- `mm/shmem.c` - Main integration point
- `include/linux/swapops.h` - Add compressed entry type helpers

**Tasks:**
1. [ ] Add `xa_is_shmem_comp()` helper
2. [ ] Add `shmem_comp_to_radix_entry()` encoding
3. [ ] Add `shmem_radix_to_comp_entry()` decoding
4. [ ] Modify `shmem_get_folio_gfp()` to handle compressed entries
5. [ ] Implement `shmem_decompress_folio()` for read path
6. [ ] Modify `shmem_write_end()` to optionally compress
7. [ ] Implement `shmem_compress_folio()` for write path

### Phase 3: Sysfs and Mount Options

**Files to modify:**
- `mm/shmem.c` - Sysfs attributes and mount option parsing

**Tasks:**
1. [ ] Add global config variables (enabled, compressor, threshold)
2. [ ] Add sysfs attributes under `/sys/fs/tmpfs/`
3. [ ] Add mount options: `compress`, `nocompress`, `compress=lz4`, `compress_threshold=N`
4. [ ] Implement mount-time pool initialization
5. [ ] Add statistics tracking and reporting

### Phase 4: Advanced Features

**Tasks:**
1. [ ] Add per-file compression flag (FS_COMPR_FL)
2. [ ] Implement ioctl for per-file control
3. [ ] Add shrinker for memory pressure compression
4. [ ] Handle THP interaction (split before compress)
5. [ ] Add swap interaction (compress before swapout)
6. [ ] Implement deferred compression worker

### Phase 5: Testing and Optimization

**Tasks:**
1. [ ] Write unit tests for compression/decompression
2. [ ] Write integration tests for read/write/mmap paths
3. [ ] Benchmark compression ratio and latency
4. [ ] Optimize hot paths
5. [ ] Add debug statistics
6. [ ] Write documentation

## Detailed Design

### Compressed Entry Encoding

```c
// include/linux/shmem_compress.h

/*
 * Compressed entry stored in xarray as xa_value
 *
 * Bit layout (64-bit):
 * ┌─────────────────────────────────────────────────────────┐
 * │ 63      40 │ 39                  12 │ 11       0      │
 * │   unused   │     zs_handle (28)    │  length (12)     │
 * └─────────────────────────────────────────────────────────┘
 *
 * - zs_handle: zsmalloc handle (max 256MB pool = 28 bits)
 * - length: compressed size in bytes (max 4095, fits 12 bits)
 */

struct shmem_comp_entry {
    unsigned long handle;    // zsmalloc handle
    unsigned int length;     // Compressed size (bytes)
    bool referenced;         // For LRU eviction
};

static inline void *shmem_comp_to_radix_entry(struct shmem_comp_entry *entry)
{
    unsigned long val = (entry->handle << 12) | (entry->length & 0xFFF);
    return xa_mk_value(val);
}

static inline struct shmem_comp_entry shmem_radix_to_comp_entry(void *arg)
{
    unsigned long val = xa_to_value(arg);
    struct shmem_comp_entry entry = {
        .handle = val >> 12,
        .length = val & 0xFFF,
        .referenced = true,
    };
    return entry;
}

static inline bool xa_is_shmem_comp(const void *entry)
{
    if (!xa_is_value(entry))
        return false;
    
    // Use a reserved swap type range for shmem compression
    // Need to ensure this doesn't conflict with existing types
    return shmem_comp_type_check(entry);
}
```

### Compression Pool

```c
// mm/shmem_compress.c

struct shmem_comp_pool {
    const char *algo_name;           // "lz4", "lzo", etc.
    struct crypto_acomp *acomp;      // Shared acomp instance
    struct zs_pool *storage;         // zsmalloc pool
    struct percpu_struct *pcpu;      // Per-CPU contexts
    atomic_t refcount;               // Reference counting
    struct list_head list;           // Global pool list
};

struct shmem_comp_pcpu {
    struct crypto_acomp *acomp;      // Per-CPU acomp
    struct acomp_req *req;           // Per-CPU request
    struct mutex lock;               // Serialize per-CPU
    char *buffer;                    // PAGE_SIZE scratch buffer
    struct crypto_wait wait;
};

// Pool lifecycle
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo);
void shmem_comp_pool_destroy(struct shmem_comp_pool *pool);
struct shmem_comp_pool *shmem_comp_pool_get(const char *algo);
void shmem_comp_pool_put(struct shmem_comp_pool *pool);

// Compression/decompression
int shmem_comp_compress(struct shmem_comp_pool *pool,
                        const void *src, void *dst,
                        unsigned int *dst_len);
int shmem_comp_decompress(struct shmem_comp_pool *pool,
                          const void *src, unsigned int src_len,
                          void *dst);

// Storage
unsigned long shmem_comp_store(struct shmem_comp_pool *pool,
                               const void *data, unsigned int len);
int shmem_comp_load(struct shmem_comp_pool *pool,
                    unsigned long handle, void *dst,
                    unsigned int *len);
void shmem_comp_free(struct shmem_comp_pool *pool,
                     unsigned long handle);
```

### Integration with shmem_get_folio_gfp()

```c
// mm/shmem.c - modified shmem_get_folio_gfp()

static int shmem_get_folio_gfp(struct inode *inode, pgoff_t index,
                                loff_t write_end, struct folio **foliop,
                                enum sgp_type sgp, gfp_t gfp,
                                struct vm_fault *vmf, 
                                vm_fault_t *fault_type)
{
    // ... existing code ...
    
    folio = filemap_get_entry(inode->i_mapping, index);
    
    // ... existing userfaultfd handling ...
    
    // Check for swap entry (existing code)
    if (xa_is_value(folio)) {
        error = shmem_swapin_folio(inode, index, &folio,
                                   sgp, gfp, vmf, fault_type);
        // ...
    }
    
    // NEW: Check for compressed entry
    if (xa_is_shmem_comp(folio)) {
        struct shmem_comp_entry comp_entry = 
            shmem_radix_to_comp_entry(folio);
        
        error = shmem_decompress_folio(inode, index, &comp_entry,
                                        sgp, gfp, foliop);
        if (error == -EEXIST)
            goto repeat;
        
        if (!error) {
            // Mark as referenced for LRU
            shmem_comp_mark_referenced(inode, index, 
                                        comp_entry.handle);
        }
        
        *foliop = folio;
        return error;
    }
    
    // ... rest of existing code ...
}
```

### Decompression Path

```c
// mm/shmem.c

static int shmem_decompress_folio(struct inode *inode, pgoff_t index,
                                   struct shmem_comp_entry *comp_entry,
                                   enum sgp_type sgp, gfp_t gfp,
                                   struct folio **foliop)
{
    struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
    struct folio *folio;
    void *src, *dst;
    int ret;
    
    if (!sbinfo->comp_pool)
        return -ENOENT;
    
    // Allocate new folio
    folio = shmem_alloc_and_add_folio(NULL, gfp, inode, index,
                                       current->mm, 0);
    if (IS_ERR(folio))
        return PTR_ERR(folio);
    
    folio_lock(folio);
    
    // Verify folio is still valid
    if (folio->mapping != inode->i_mapping) {
        folio_unlock(folio);
        folio_put(folio);
        return -EEXIST;  // Retry
    }
    
    // Read compressed data from storage
    dst = kmap_local_folio(folio, 0);
    ret = shmem_comp_load(sbinfo->comp_pool, comp_entry->handle,
                          dst, &comp_entry->length);
    kunmap_local(dst);
    
    if (ret) {
        folio_unlock(folio);
        folio_put(folio);
        return ret;
    }
    
    // Decompress
    dst = kmap_local_folio(folio, 0);
    ret = shmem_comp_decompress(sbinfo->comp_pool, 
                                 /* src from somewhere */, 
                                 comp_entry->length,
                                 dst);
    kunmap_local(dst);
    
    if (ret) {
        folio_unlock(folio);
        folio_put(folio);
        return ret;
    }
    
    // Mark folio as uptodate
    folio_mark_uptodate(folio);
    
    // Free compressed entry
    shmem_comp_free(sbinfo->comp_pool, comp_entry->handle);
    
    // Update accounting
    atomic_long_dec(&sbinfo->comp_pages);
    atomic_long_sub(comp_entry->length, &sbinfo->comp_bytes);
    
    *foliop = folio;
    return 0;
}
```

### Compression Path

```c
// mm/shmem.c

static int shmem_compress_folio(struct inode *inode, pgoff_t index,
                                 struct folio *folio)
{
    struct shmem_inode_info *info = SHMEM_I(inode);
    struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
    struct shmem_comp_entry comp_entry;
    void *src, *comp_buf;
    unsigned int comp_len;
    unsigned long handle;
    int ret;
    
    if (!sbinfo->comp_pool)
        return -ENOTSUPP;
    
    // Lock folio
    folio_lock(folio);
    
    // Verify folio is valid
    if (folio->mapping != inode->i_mapping ||
        !folio_test_uptodate(folio)) {
        folio_unlock(folio);
        return -ENOENT;
    }
    
    // Skip if already compressed (shouldn't happen)
    if (folio_test_compressed(folio)) {
        folio_unlock(folio);
        return 0;
    }
    
    // Get source data
    src = kmap_local_folio(folio, 0);
    
    // Compress
    comp_buf = kmalloc(PAGE_SIZE, GFP_NOWAIT);
    if (!comp_buf) {
        kunmap_local(src);
        folio_unlock(folio);
        return -ENOMEM;
    }
    
    comp_len = PAGE_SIZE;
    ret = shmem_comp_compress(sbinfo->comp_pool, src, comp_buf,
                               &comp_len);
    kunmap_local(src);
    
    if (ret || comp_len >= PAGE_SIZE) {
        // Incompressible - skip
        kfree(comp_buf);
        folio_unlock(folio);
        return -ENOSPC;
    }
    
    // Store in zsmalloc pool
    handle = shmem_comp_store(sbinfo->comp_pool, comp_buf, comp_len);
    kfree(comp_buf);
    
    if (!handle) {
        folio_unlock(folio);
        return -ENOMEM;
    }
    
    // Build compressed entry
    comp_entry.handle = handle;
    comp_entry.length = comp_len;
    comp_entry.referenced = true;
    
    // Replace page cache entry
    // Note: Need careful locking here
    folio_clear_dirty(folio);
    folio_clear_uptodate(folio);
    folio_set_compressed(folio);  // New flag
    
    // Update xarray: replace folio with compressed entry
    ret = xa_cmpxchg(&inode->i_mapping->i_pages, index,
                      folio, shmem_comp_to_radix_entry(&comp_entry),
                      0);
    if (ret) {
        // Failed to replace - cleanup
        shmem_comp_free(sbinfo->comp_pool, handle);
        folio_clear_compressed(folio);
        folio_unlock(folio);
        folio_put(folio);
        return ret;
    }
    
    // Update accounting
    atomic_long_inc(&sbinfo->comp_pages);
    atomic_long_add(comp_len, &sbinfo->comp_bytes);
    
    // Remove folio from page cache
    filemap_remove_folio(folio);
    folio_unlock(folio);
    folio_put(folio);
    
    return 0;
}
```

### Write Path Integration

```c
// mm/shmem.c - shmem_write_end() modification

static int shmem_write_end(const struct kiocb *iocb, 
                           struct address_space *mapping,
                           struct folio *folio, pos, len, copied,
                           struct page **pagep)
{
    // ... existing write handling ...
    
    // After write completes successfully
    if (sbinfo->comp_pool && sbinfo->comp_enabled) {
        struct inode *inode = mapping->host;
        
        // Check if file meets compression threshold
        if (i_size_read(inode) >= sbinfo->comp_threshold) {
            // Queue for deferred compression
            // Don't compress inline - too slow for write path
            shmem_comp_queue_folio(inode, folio);
        }
    }
    
    // ... rest of existing code ...
}
```

## Kconfig Options

```kconfig
# mm/Kconfig

config TMPFS_COMPRESSION
    bool "Transparent compression for tmpfs"
    depends on TMPFS && CRYPTO && ZSMALLOC
    select CRYPTO_LZO
    select CRYPTO_LZ4
    select CRYPTO_LZO_RLE
    default n
    help
      Enable transparent page-level compression for tmpfs files.
      Compressed pages are stored in zsmalloc pools and transparently
      decompressed on access. This allows tmpfs to use less memory
      for compressible data without reserving RAM.

      If unsure, say N.

config TMPFS_COMPRESSOR_DEFAULT
    string "Default tmpfs compressor"
    depends on TMPFS_COMPRESSION
    default "lz4"
    help
      Default compression algorithm for tmpfs. Available options:
        lz4     - Fastest, good compression ratio
        lzo     - Good balance of speed and ratio
        lzo-rle - Better for incompressible data

      This can be changed at runtime via /sys/fs/tmpfs/compressor.
```

## Testing Strategy

### Unit Tests
- Compression/decompression roundtrip
- Edge cases: incompressible data, all zeros, random data
- Concurrent compression from multiple CPUs
- Memory pressure handling

### Integration Tests
- Basic read/write to compressed tmpfs
- mmap() of compressed files
- fallocate() on compressed files
- Truncate and hole punch
- Swap interaction
- Mount/unmount with compression

### Benchmarks
- Compression ratio for various data types
- Read/write latency
- mmap fault latency
- CPU usage during compression
- Memory savings

## Risk Assessment

### High Risk
- **Page cache locking**: Compressed entries change locking semantics
- **THP interaction**: Huge pages need special handling
- **Race conditions**: Concurrent compress/decompress

### Medium Risk
- **Memory accounting**: Compressed pages use different amounts
- **Swap interaction**: Compressed pages also need swap support
- **Error handling**: Corruption recovery

### Low Risk
- **Sysfs interface**: Straightforward to add
- **Mount options**: Standard parsing
- **Statistics**: Simple counters

## Dependencies

1. **zsmalloc** - For compressed data storage
2. **crypto API** - For LZ4/LZO compression
3. **shmem** - Existing tmpfs infrastructure
4. **page cache** - xarray for entry storage

## Success Criteria

1. ✅ Files can be compressed transparently
2. ✅ mmap() works on compressed files
3. ✅ No RAM reserved (uses free memory opportunistically)
4. ✅ Compression ratio > 40% for compressible data
5. ✅ Read latency overhead < 20% for compressed pages
6. ✅ Runtime configuration via sysfs
7. ✅ Per-mount and per-file control
8. ✅ Statistics available for monitoring

## Timeline Estimate

- **Phase 1 (Infrastructure)**: 3-5 days
- **Phase 2 (Page Cache)**: 5-7 days
- **Phase 3 (Sysfs/Options)**: 2-3 days
- **Phase 4 (Advanced)**: 5-7 days
- **Phase 5 (Testing)**: 3-5 days

**Total**: 18-27 days for a production-ready implementation

## References

- zswap implementation: `mm/zswap.c`
- shmem implementation: `mm/shmem.c`
- zsmalloc API: `include/linux/zsmalloc.h`
- crypto acomp API: `include/crypto/acompress.h`
- Existing compression: `crypto/lz4.c`, `crypto/lzo.c`

---

# Cell-Based Storage (FTL-inspired) — Transient Plan

## Motivation

zsmalloc works but pays ~24-40B per object in metadata (8B handle header
inside every slot + size-class rounding slack + zspage struct + handle from
handle_cachep). For our workload (144 pages, ~2.8KB compressed each) that
measured as 479KB pool usage for 410KB of data — 14% overhead.

The cell allocator stores compressed blocks at **exact sizes** in
bump-allocated cells. Zero per-object overhead. Whole cells return to the
buddy allocator the instant they empty.

## Design

### Data structures

```c
#define CELL_ORDER_MIN   2       /* 16KB */
#define CELL_ORDER_MAX   10      /* 4MB  */
#define CELL_ORDER_DEF   4       /* 64KB */

struct cell {
    struct page *pages[1 << CELL_ORDER_MAX]; /* actual count = 1<<order */
    void         *base;          /* page_address(pages[0]) */
    u32          next_free;      /* bump pointer, monotonically increasing */
    u32          live_bytes;     /* decremented on free */
    u16          live_objects;
    u16          cellid;
    u8           order;          /* alloc_pages order */
    bool         active;         /* current allocation target */
    struct rw_semaphore lock;    /* readers vs compaction */
    struct list_head node;       /* in pool's sparse list */
};

struct cell_pool {
    spinlock_t        lock;      /* protects active + directory */
    struct cell       *active;
    struct cell       *cells;    /* array, indexed by cellid */
    u16               nr_cells;
    u16               max_cells;
    u8                cell_order;
    bool              use_thp;   /* GFP_TRANSHUGE | __GFP_COMP */
    struct list_head  sparse;    /* cells eligible for compaction */
    atomic_long_t     pages_used; /* for stats */
};
```

### Location encoding

Each compressed block is addressed by `{cellid, offset, length}`.
Packed into the entry struct alongside the existing zsmalloc fields:

```c
struct shmem_comp_entry {
    /* zsmalloc path */
    unsigned long handle;
    unsigned long *handles;
    unsigned short nchunks;
    /* cell path */
    u16 cellid;
    u32 offset;
    /* common */
    unsigned int length;
    unsigned short nr_pages;
    atomic_t refs;
    atomic_t slots;
    struct shmem_comp_pool *pool;
    struct rcu_head rcu;
};
```

The pool struct gains a `struct cell_pool *cells` field and a
`bool use_cells` flag. Mount option `cell=N` sets use_cells=true.

### Allocation (bump)

```
cell_store(pool, data, len) -> {cellid, offset}
  spin_lock(&cell_pool->lock)
  if (active->next_free + len > CELL_SIZE)
      active = cell_pool_new_cell()     /* alloc_pages or THP */
  off = active->next_free
  active->next_free += len
  active->live_bytes += len
  active->live_objects++
  spin_unlock
  memcpy(active->base + off, data, len)
  return {active->cellid, off}
```

### Free

```
cell_free(pool, cellid, offset, length)
  cell = &cells[cellid]
  spin_lock(&pool->lock)
  cell->live_bytes -= length
  cell->live_objects--
  if (cell->live_bytes == 0)
      cell_pool_return(cell)            /* __free_pages, 1<<order pages */
  else if (cell->live_objects < SPARSE_THRESHOLD)
      list_add(&cell->node, &pool->sparse)
  spin_unlock
```

### Load

```
cell_load(pool, cellid, offset, dst, len)
  cell = &cells[cellid]
  memcpy(dst, cell->base + offset, len)
```

No bounce buffer. Cells are virtually contiguous (direct map on x86_64),
so any offset is plain pointer arithmetic.

### Compaction (phase 2, not in initial prototype)

Triggered when active cell is full and no empty cell exists:
1. Pick sparsest cell from pool->sparse
2. down_write(&victim->lock) — block readers
3. For each entry pointing into victim: read, write to active, update entry
4. Return victim to buddy
5. up_write(&victim->lock)

## Mount options

| Option | Values | Default | Notes |
|--------|--------|---------|-------|
| `compress=` | lz4,zstd,lzo,none | sysfs default | existing |
| `merge_pages=` | 1..64 | 1 | existing |
| `compress_threshold=` | bytes | 65536 | existing |
| `cell=` | 64K,256K,1M,2M,4M | derived from merge | **new** |
| `thp=` | yes,no | yes (if cell>=1M) | **new** |

Cell size defaults when `cell=` not given:
- merge=1 → 64K, merge=2 → 128K, merge=4 → 256K
- merge=8 → 512K, merge=16 → 1M

## Implementation phases

Each phase is a separate git commit, buildable and testable.

### P1: Cell data structures + pool lifecycle
- Add `struct cell`, `struct cell_pool` to shmem_compress.c
- `cell_pool_create(order, use_thp)` / `cell_pool_destroy()`
- No wiring yet — compiles, existing tests pass

### P2: Bump allocation + free
- `cell_store(pool, data, len)` — bump alloc, memcpy
- `cell_free(pool, cellid, offset, length)` — dec live_bytes, return empty cells
- `cell_load(pool, cellid, offset, dst, len)` — memcpy
- No wiring yet

### P3: Wire into shmem_comp_store/load/free
- Add `use_cells` flag to `shmem_comp_pool`
- When true: cell_store/cell_load/cell_free instead of zs_malloc/zs_obj_read/zs_free
- Entry gets cellid/offset instead of handle/handles
- Mount option `cell=` enables this path

### P4: Mount options cell= and thp=
- Parse in shmem_parse_one
- Store in shmem_options and shmem_sb_info
- Pass to cell_pool_create

### P5: Sweep verification
- Run sweep_merge.sh with cell-based storage
- Compare RAM numbers against zsmalloc baseline
- Document in results/

### P6: Compaction (if P5 numbers justify it)
- Sparse cell detection
- Compaction algorithm
- Shrinker integration

## Baseline (zsmalloc, current tree)

From sweep_merge_20261006_183047.summary:

| merge | ram_pool | ram_merged | ram_entry | ram_total | compressed_bytes |
|-------|----------|------------|-----------|-----------|------------------|
| 1     | 479,232  | 0          | 9,216     | 488,448   | 409,915          |
| 2     | 606,208  | 1,152      | 4,608     | 611,968   | 407,987          |
| 4     | 552,960  | 1,152      | 2,304     | 556,416   | 405,974          |
| 8     | 565,248  | 1,152      | 1,152     | 567,552   | 405,468          |
| 16    | 454,656  | 896        | 576       | 456,128   | 405,128          |

Target: match or beat ram_total for every merge value, with the biggest
gains expected at merge=2/4 where zsmalloc chunk overhead is worst.

---

# Spec: cellzram - compressed RAM block device on the cell allocator

## Overview

cellzram is a compressed memory block device (`/dev/cellzramN`) that
stores 4 KiB blocks in the cell allocator instead of zsmalloc. It is
zram's contract (logical capacity set via `disksize`, physical usage
grows on demand) with the cell allocator's properties: zero
per-object storage metadata, direct-map reads with no bounce buffer,
and synchronous whole-cell return to the buddy allocator on discard
and free.

## Motivation

zram is the standard compressed-RAM block device (swap, scratch
backing), but its storage layer is zsmalloc, which we measured this
project paying a 14% metadata tax on (8-byte handle in every slot,
size-class rounding, zspage structs, handle-slab objects) and which
retains freed pages until its shrinker runs - precisely when memory
is tight. Our cell allocator already fixes both problems for tmpfs
(24% less RAM at merge=2, 21.9 vs 14.7 MB/s read throughput,
synchronous cell release). A block device front-end makes those
properties available to workloads that require a `struct
block_device`: swap, ext4/xfs, O_DIRECT databases, journal devices.
The discard path is the headline synergy: ext4 issues discards on
truncate/delete, and each discard frees entries whose cells return
to the buddy inline - the filesystem sees MemFree jump before the
deleting syscall returns.

The device must not exist as an uncompressed ramdisk: `brd` already
does that, and uncompressed memory-backed storage is what tmpfs is.
Compression is the point. `compress=none` exists only as a debug and
latency escape hatch.

## Goals

- `/dev/cellzramN` gendisk with 4 KiB logical and physical sectors,
  capacity set at runtime via `disksize` sysfs (zram-style).
- Compression ON by default (lz4); `comp_algorithm` accepts
  `lz4|zstd|none`; `none` stores raw blocks (debug/latency opt-out).
- Physical allocation on demand: a 1 GiB disksize backed by a 300 MB
  working set physically commits ~300 MB.
- Discard frees blocks synchronously: empty cells return to the buddy
  before the discard completes.
- Swap works unmodified (mkswap/swapon on the device), with
  discard-driven reclamation of freed swap slots.
- Rig-verified: checksum-verified write/read across all modes,
  discard-then-verify-cells-drain, reset-returns-all-cells, swap
  trial. Zero KASAN/Oops.
- Measurably better than zram's architecture on RAM overhead and
  read throughput under the rig's existing methodology.

## Non-Goals

- Raw/uncompressed mode as a first-class feature. `brd` exists;
  uncompressed memory storage is what tmpfs is. `compress=none` is a
  debug knob, not a product mode.
- writeback-to-backing-device (zram's `backing_dev` feature).
- Replacing zram upstream; this is a project experiment first.
- Runtime switching between compressed and raw. `comp_algorithm` is
  writable only when `disksize` is 0 (before first use) or after
  `reset`.
- Hot-added devices via configfs or netlink. `nr_devices` module
  parameter, fixed at load.
- Per-cell batched entry tables (one entry covering N blocks).
  Deferred; see Open Questions - it is the main metadata lever.
- Read-only device support, bad-block handling, encryption.

## Design

### Device model

One `struct cellzram` per device, created at module init:

```c
struct cellzram {
	struct gendisk		*disk;
	struct request_queue	*q;
	/*
	 * Private compression pool: gives us the per-CPU acomp
	 * contexts, the cell pool (use_cells=true), and the entry
	 * table membership.  All hot paths (compress, store, load,
	 * entry pin/retry) are shmem_comp's, unchanged.
	 */
	struct shmem_comp_pool	*pool;
	/*
	 * block_nr -> entry id.  Sparse by nature: unwritten blocks
	 * have no slot and read as zeros.  Values only (entry ids
	 * via xa_mk_value), so no pointer/value mixing.
	 */
	struct xarray		table;
	u64			logical_size;	/* bytes; 0 until disksize set */
	bool			compressed;	/* false when algo == none */
	/* stats for mm_stat / stat */
	atomic64_t		orig_data_size;
	atomic64_t		compr_data_size;
	atomic64_t		objects;
	atomic64_t		io_reads;
	atomic64_t		io_writes;
	struct mutex		io_lock;	/* serialises reset only */
};
```

Module parameters: `nr_devices` (default 1). Devices appear as
`/dev/cellzram0..N-1` after `nr_devices` disks are allocated at
init. All I/O paths run lock-free except `io_lock`, which serialises
`reset` against itself (I/O is serialised against reset by freezing
the request queue).

### Addressing and the block table

Logical sector size is 4096; block_nr = sector >> 3. The table maps
block_nr to a `shmem_comp_entry` id - the same ID table tmpfs
compression uses, so pinning, relocation-safe loads, and free
dispatch come for free. Each block is one entry (`nr_pages = 1`).

Entry `length` doubles as the raw/compressed discriminator:

| Entry length | Meaning | Read path |
|--------------|---------|-----------|
| 4096 | raw block (algo=none, or compression did not shrink) | `cell_load` memcpy |
| < 4096 | lz4/zstd compressed block | `shmem_comp_load_entry` + `crypto_acomp_decompress` |

`shmem_comp_compress()` returns `-ENOSPC` when output >= input, so a
stored block with `length == 4096` is unambiguously raw - no flag
needed in the entry.

### I/O paths

```mermaid
graph TD
    A[submit_bio] --> B{REQ_OP}
    B -->|WRITE| C[per-page loop]
    B -->|READ| D[per-page loop]
    B -->|DISCARD| E[block range free]
    C --> C1[compressed? compress : skip]
    C1 --> C2["shmem_comp_store() -> packed handle"]
    C2 --> C3["entry_alloc(pool, handle, len, 1, NULL, 0)"]
    C3 --> C4["xa_exchange(table, nr, id): COW swap"]
    C4 --> C5["old id? entry_put: tier-1 cell return"]
    D --> D1["xa_load(table, nr)"]
    D1 --> D2{slot exists?}
    D2 -->|no| D3[zero-fill page]
    D2 -->|yes| D4["shmem_comp_load_entry(id, ...)"]
    D4 --> D5{length == 4096?}
    D5 -->|raw| D6[done]
    D5 -->|compressed| D7[acomp decompress]
    E --> E1["xa_erase + entry_put per block"]
    E1 --> E5[cells return to buddy inline]
```

Write path (per 4 KiB page of the bio):

```c
/* compressed mode: one crypto call per page, same as tmpfs */
comp_len = PAGE_SIZE;
ret = shmem_comp_compress(pool, page_addr, PAGE_SIZE, cbuf, &comp_len);
if (ret) {                 /* -ENOSPC: store raw, length marks it */
	comp_len = PAGE_SIZE;
	memcpy(cbuf, page_addr, PAGE_SIZE);
}
handle = shmem_comp_store(pool, cbuf, comp_len);
if (!handle) {
	bio->bi_status = BLK_STS_IOERR;	/* cell ENOMEM; zram pattern */
	return;
}
id = shmem_comp_entry_alloc(pool, handle, comp_len, 1, NULL, 0);
if (!id) {
	u16 cellid;
	u32 offset;

	/* pack_loc biases cellid +1: unpack, never raw shifts */
	shmem_comp_unpack_loc(handle, &cellid, &offset);
	cell_free(shmem_comp_pool_cells(pool), cellid, offset, comp_len);
	bio->bi_status = BLK_STS_IOERR;
	return;
}
old = xa_exchange(&dev->table, block_nr, xa_mk_value(id), 0);
if (old)
	shmem_comp_entry_put(xa_to_value(old));   /* COW: free previous */
```

The COW xarray swap is the entire write-atomicity story: a reader
pinning the old entry keeps its data alive until the read completes;
there is no torn-block state to lock around.

Read path (per 4 KiB page): `xa_load` -> miss = zero-fill (brd
semantics for unwritten blocks); hit = `shmem_comp_load_entry(id,
buf, &len)` (pins internally, retries if GC relocated the block
mid-read), then decompress iff `len < PAGE_SIZE`.

Discard path: `xa_erase` + `entry_put` per block in range. The
entry's table reference is its slot reference; dropping it runs the
normal free dispatch -> `cell_free` -> tier-1 cell return when the
last block of a cell goes away.

Failure handling: `cell_store` returning `-ENOMEM` (genuine memory
pressure) sets `bio->bi_status = BLK_STS_IOERR` — zram's pattern for
a failed write. A bio-based driver has no blk-mq requeue path; the
status reaches the caller as -ENOMEM via `blk_status_to_errno()`.
Swap survives this: a failed swap write leaves the page in the swap
cache and the next reclaim pass retries it. `failed_writes` counts
every occurrence, as zram's stat does. This is the one path where the
block device is stricter than tmpfs (which silently keeps pages
uncompressed): a block device cannot decline to store data. Bounded
internal retry is later hardening; v1 matches zram.

### Compression policy

- Default algorithm: lz4 (throughput). `zstd` opt-in for ratio.
- `none`: device is raw-on-cells - `shmem_comp_store` still stores
  through the cell pool (entry length always 4096), but no crypto
  runs. Debug and latency-sensitive use.
- The pool is created at `disksize`-set time.
  `shmem_comp_pool_create()` validates the algorithm with
  `crypto_has_acomp()`, which "none" fails — so the pool is always
  created with a real algorithm (lz4 in `none` mode) and the device's
  `compressed` flag alone gates crypto. The contexts idle in `none`
  mode (noted inefficiency; removing them requires a shmem_comp_pool
  change deferred to Open Questions).
- No merge groups at the block layer in v1: each 4 KiB block is an
  independent I/O unit, one entry each. Merged-group storage (one
  entry, N block slots) is the v2 ratio lever for sequential-write
  filesystems on top; partial-group invalidation under random writes
  is the reason it is deferred.

### Sysfs interface

Per device, zram-shaped, under `/sys/block/cellzramN/`:

| Attribute | Access | Semantics |
|-----------|--------|-----------|
| `disksize` | RW | Logical size in bytes. Writable only when 0 (or after reset). Creates the pool on first set. |
| `comp_algorithm` | RW | Shows current; writable only before disksize is set or after reset. Values: `lz4`, `zstd`, `none`. |
| `mm_stat` | RO | `orig_data_size compr_data_size phys_used objects entry_bytes ratio_per_10000` (6 fields, space-separated). `phys_used` = pool `pages_used << PAGE_SHIFT` (cell storage only); `entry_bytes` = live block-table entry RAM (~64 B per written block), reported separately so the zram comparison is like-for-like — zram's `mem_used_total` includes its per-page table. |
| `stat` | RO | `backing_dev_discard io_reads io_writes` (device-level I/O counters). |
| `reset` | WO | Freeze queue, erase all table slots (entry_put each), unfreeze. All cells return to the buddy. Stats zeroed. |
| `discard` granularity | - | 4096 via `queue_limits` at `blk_alloc_disk()` time: `.max_hw_discard_sectors = UINT_MAX`, `.discard_granularity = PAGE_SIZE`. This tree has no `QUEUE_FLAG_DISCARD`; zram sets the same limits in `zram_add()`. |

GC interplay: device pools join the global `shmem_comp_pools` list,
so `/sys/fs/tmpfs/compression/gc_now` already triggers a GC pass on
cellzram's sparse cells, and the shrinker registered at pool creation
reclaims under memory pressure. The sysfs group is tmpfs-named for a
block-device pool - cosmetic confusion, accepted for the prototype
(Open Questions).

### Concurrency and locking

| Resource | Protection |
|----------|------------|
| block table | xarray internal lock (xa_exchange/xa_erase/xa_load) |
| entry lifetime | refcount (refs/slots); readers pin via load_entry |
| cell memory | per-cell rwsem (GC eviction vs readers) |
| cell pool bump | cp->lock |
| reset vs I/O | `blk_freeze_queue` (bio-based submit holds a q_usage_counter ref; the drain completes) |

No device-wide lock on the read or write path. Concurrent writers to
the same block_nr serialise only inside `xa_exchange`; last writer
wins, the loser's entry is freed by the winner's COW put.

### Swap

The device is a normal gendisk: `mkswap` + `swapon` work unchanged.
Two independent reclaim paths return freed swap slots to the buddy:

1. `.swap_slot_free_notify` in `block_device_operations` — zram's
   primary path. The swap code calls it on every slot free regardless
   of swapon flags; each notify maps `{si, offset}` -> block_nr ->
   `xa_erase` + `entry_put`, and the cell returns to the buddy inline
   when its last object goes.
2. Discard bios — `sys_swapon()` issues these only when the caller
   passed `SWAP_FLAG_DISCARD` AND `bdev_max_discard_sectors()` is set
   (`mm/swapfile.c`). The queue limits satisfy the second condition;
   the first depends on the invocation, so the rig test uses a tiny
   static `swapon(2)` wrapper (busybox's swapon may not expose the
   flag).

v1 implements (1); (2) falls out of the existing discard path for
free. The synchronous cell return — before the freeing syscall or
reclaim pass completes — is the property zram lacks.

REQ_PREFLUSH/FUA complete immediately after the write lands (RAM;
no cache-flush semantics needed), matching zram.

## Build Changes

| Target | Change |
|--------|--------|
| `mm/Kconfig` | New `CONFIG_TMPFS_COMP_BLKDEV` (bool, default n, depends on TMPFS_COMPRESSION, help: cell-backed compressed RAM block device) |
| `mm/Makefile` | `obj-$(CONFIG_TMPFS_COMP_BLKDEV) += cellzram.o` |
| rig `kernel/.config` | Enable `CONFIG_TMPFS_COMP_BLKDEV=y` |
| rig `initrd/init` | Run cellzram test when `test.cellzram=1` on cmdline |
| rig `scripts/run_qemu.sh` | `TEST_CELLZRAM` -> `test.cellzram=` cmdline passthrough |

## File Manifest

| File | Action | Purpose |
|------|--------|---------|
| `TMPFS_COMPRESSION_PLAN.md` | Modify | This spec |
| `mm/cellzram.c` | Create | gendisk (`blk_alloc_disk`, bio-based `.submit_bio`), block table, I/O paths, sysfs, module init |
| `mm/Kconfig` | Modify | `CONFIG_TMPFS_COMP_BLKDEV` |
| `mm/Makefile` | Modify | Build `cellzram.o` |
| `include/linux/shmem_compress.h` | Modify | Only if pool-create tweaks are needed for algo=none (see Open Questions); otherwise untouched |
| rig `tests/test_cellzram.c` | Create | dd round-trips + checksums, discard/drain, reset, comp_algorithm matrix, swap trial |
| rig `initrd/init` | Modify | Gate test behind `test.cellzram=1` |
| rig `scripts/run_qemu.sh` | Modify | `TEST_CELLZRAM` passthrough |

Reuse (no changes expected): `mm/cell_store.c` (bump allocator,
tier-1 return, GC), `shmem_comp_{compress,store,load_entry,entry_*}`
(hot paths), cell GC + shrinker + `gc_now`.

## Migration

### Phase 1: raw skeleton on cells (4-6 h)

1. `mm/cellzram.c`: module init, `nr_devices`, `blk_alloc_disk(&lim,
   NUMA_NO_NODE)` with `.submit_bio` in `block_device_operations`
   (zram's shape in this tree — bio-based, no tag set), 4K
   `logical_block_size`, queue-limits discard fields, `disksize`
   sysfs, pool creation with `use_cells` (algo lz4 but compression
   bypassed: every block stored raw, length 4096).
2. Block table (`xarray`), write/read/discard/reset paths, zero-fill
   for unwritten blocks, `BLK_STS_IOERR` on ENOMEM (zram pattern).
3. `mm/Kconfig` + `mm/Makefile`; rig config enable.
4. Rig test: 256 MiB dd round-trip with checksums, unwritten-block
   zero-read, discard half the device and verify `cell_stats` drains
   and MemFree rises, reset returns every cell.
5. Commit. Full suite green.

### Phase 2: compression on by default (4-6 h)

1. `comp_algorithm` sysfs (`lz4|zstd|none`), writable pre-disksize.
2. Wire `shmem_comp_compress` into the write path; entry length
   discriminator on read; `-ENOSPC` -> raw store.
3. `mm_stat` with orig/compr/phys/objects/ratio.
4. Rig test: compressible vs incompressible datasets, ratio reported,
   checksums in lz4/zstd/none, physical usage < logical for
   compressible data.
5. Commit. Full suite green.

### Phase 3: swap trial + hardening (3-4 h)

1. `stat` sysfs, `.swap_slot_free_notify` wiring, and any queue_limits
   discard gaps left from phase 1.
2. Rig: `mkswap` + `swapon` on cellzram0, force page cache pressure,
   verify swap I/O and that freed slots return cells (MemFree
   movement); `swapoff` releases everything.
3. KASAN boot: full test battery including concurrent dd writers +
   gc_now + discard storm.
4. Commit. Mission log entry.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Per-block entry metadata: ~64 B slab per written 4 KiB block (16 MiB per GiB written) | Certain | Med | Same class as zram's per-page table; acceptable for v1. Batched per-cell entries are the v2 lever (Open Questions). |
| Entry table is global (shared with tmpfs compression); xa_alloc contention between workloads | Low | Low | xarray lock is per-node; measured contention negligible at prototype scale. Split tables if it shows up. |
| ENOMEM under true memory pressure fails writes (tmpfs degrades gracefully; block device cannot) | Med | Med | Bio-based: `BLK_STS_IOERR` (zram pattern); swap retries because the failed page stays in the swap cache. Bounded internal retry is later hardening, not v1. |
| THP cell allocation (`GFP_TRANSHUGE`) may fail under fragmentation; same requeue path applies | Low | Low | Requeue semantics; optionally create device pools with `use_thp=false` if requeue storms appear. |
| Reset races in-flight I/O | Low | High | `blk_freeze_queue` before erasing the table; covered by KASAN concurrent test in phase 3. |
| Global `shmem_comp_global_stats` mixes tmpfs and cellzram numbers | Certain | Low | Accepted for prototype; per-device stats are authoritative (`mm_stat`). |
| Swap writes bypass nothing (normal bdev path) but swap slot frees depend on discard being honoured | Med | High | Phase 3 explicitly tests swap-free -> MemFree; if the swap code path needs `SWP_DISCARDABLE` hints, wire them. |

## Open Questions

- **Per-cell batched entries.** One `shmem_comp_entry` per cell
  region covering N contiguous blocks would cut entry metadata ~32x
  (one 64 B entry per 128 KiB instead of per 4 KiB). Cost: partial
  discard inside a batched region requires decompress-split, and the
  read path becomes offset-within-entry. This is the single biggest
  optimization lever left; needs a design pass before v2.
- **Device naming** (`cellzram`) is a placeholder; upstream review
  would pick the final name.
- **Sysfs placement**: cellzram pools appear under
  `/sys/fs/tmpfs/compression/` via the global pool list. Either
  rename the group compression-agnostically or give cellzram its own
  group.
- **`comp_algorithm=none` pool weight**: idle per-CPU acomp contexts
  are allocated even in raw mode. Removing them needs a
  `shmem_comp_pool_create` variant; low priority.
- **zstd default?** lz4 wins on throughput (21.7 MB/s untar vs
  lz4 numbers in the sweep); zstd wins ~10% ratio. Keep lz4 default,
  revisit with real dataset measurements.

## References

- `TMPFS_COMPRESSION_PLAN.md` sections: "Cell-Based Storage
  (FTL-inspired)" (allocator design, invariants), "Mount options"
  (cell= derivation), "Baseline" (zsmalloc sweep numbers).
- `mission_log/2026-10-05_tmpfs-transparent-compression.md` session
  3: cell allocator implementation, async batched GC, the 256M untar
  numbers (cell 21.9 vs zsmalloc 14.7 MB/s verify; merge=2 RAM 463K
  vs 612K), and the bugs that shaped the invariants.
- `mm/cell_store.c` header comment: the four allocator invariants
  this device inherits.
- zram (`drivers/block/zram/`): the sysfs interface shape, the
  bio-based `blk_alloc_disk` + `fops.submit_bio` driver model, and
  the `swap_slot_free_notify` + queue-limits discard reclaim pattern
  being mirrored.

---

# Upstream Shape — blast zone, carve plan, comment policy

## Why this section exists

The feature's acceptance depends on the diff to existing files, not on
the size of the new ones. Measured against base `a90ee4305c4a` (51
commits): +1,387 lines across 6 existing files, +2,663 lines in new
files (zero blast). Every existing-file touch already sits inside
`#ifdef CONFIG_TMPFS_COMPRESSION` (20 guard sites in shmem.c;
shmem_fs.h fields guarded; `SWP_SHMEM_COMPRESS` compiles to 0 when
disabled) and the config is `default n`: with the config off, the
behavioral diff is zero. What remains is the source diff a reviewer
must read. This section plans how to shrink it to ~+210 lines without
moving logic across a locking boundary, and fixes the comment policy
for each tier of file.

Config shape: `CONFIG_EXPERIMENTAL` no longer exists (removed in
v5.19). The modern acceptance pattern is the zswap precedent: `bool`,
`default n`, help text says "experimental". `TMPFS_COMPRESSION`
already is `default n`; the help text gains the experimental wording
at posting time. The global sysfs defaults are the acceptance-hostile
surface (they change what plain `mount -t tmpfs` does for NEW mounts):
gate them behind `TMPFS_COMPRESSION_GLOBAL_DEFAULTS` (default n) or
leave them out of v1. Per-mount options are additive and change
nobody's behavior unless requested.

## Measured blast zone

New files, zero blast: `mm/shmem_compress.c` (1412), `mm/cell_store.c`
(515), `include/linux/shmem_compress.h` (551),
`include/linux/cell_store.h` (185).

Existing files (+1,387 total):

| File | Added | Hunks |
|------|------:|------:|
| mm/shmem.c | 1,283 | 18 |
| include/linux/swapops.h | 40 | 1 |
| include/linux/swap.h | 18 | 2 |
| include/linux/shmem_fs.h | 14 | 2 |
| fs/Kconfig | 31 | 1 |
| mm/Makefile | 1 | 1 |

shmem.c hunk breakdown:

| Lines | Site | Nature |
|------:|------|--------|
| 767 | after `shmem_split_large_entry` | compress/decompress implementation (folio/group/chunk paths) |
| 209 | `tmpfs_sysfs_init` area | sysfs attributes |
| 103 | `shmem_fill_super` | pool creation, cell-order derivation |
| 63 | `shmem_parse_one` | mount options |
| 41 | `shmem_free_swap` | eviction: release compression entries |
| 27 | `shmem_get_folio_gfp` | read path: decompress branch |
| 23 | `shmem_write_end` | write path: compress trigger |
| ~56 | 11 small hunks | options struct, reconfigure guard, put_super, free_fc, init_fs_context, decls |

## Carve plan

Each carve is a separate, behavior-neutral commit; the rig suite must
be green before and after each one (bisectable). None moves logic
across a locking boundary — they relocate whole functions whose
dependencies are already clean.

### C1 — the 767-line block -> mm/shmem_compress.c

The implementation block (`shmem_comp_get_folio_noalloc`,
`shmem_decompress_folio`/`_group`, `shmem_compress_folio`/`_group`,
`shmem_comp_compress_chunk`, `shmem_comp_chunk_nr`) lives in shmem.c
only because it calls shmem statics:

| Primitive | Uses in block | Action |
|-----------|--------------:|--------|
| `shmem_recalc_inode` | 6 | unstatic + `EXPORT_SYMBOL_GPL` (inside the config ifdef) |
| `shmem_delete_from_page_cache` | 3 | unstatic + `EXPORT_SYMBOL_GPL` (inside the config ifdef) |
| `shmem_undo_range` | 2 | try the already-exported `shmem_truncate_range` (it also updates timestamps/iversion — behavior delta gets its own commit); else third export |

`shmem_get_folio` is already exported. Exports go inside
`#ifdef CONFIG_TMPFS_COMPRESSION`: a disabled build exports nothing
new — the API surface is gated, not just the code. Precedent for
exporting shmem internals: `shmem_write_folio` (i915/dramfs),
`shmem_get_folio`, `shmem_truncate_range`.

### C2 — sysfs + defaults globals -> mm/shmem_compress.c

All attribute bodies (`compression`, `compressor`, `merge_pages`,
`comp_stats`, `cell_stats`, `gc_now`) touch only `shmem_comp_*`
functions and the three defaults globals
(`shmem_comp_default_{enabled,compressor,merge}` + their lock); the
sole shmem dependency is the parent kobject. New API:

    int  shmem_comp_sysfs_register(struct kobject *parent);
    void shmem_comp_sysfs_unregister(void);

shmem.c keeps ~4 lines in `tmpfs_sysfs_init()`. The defaults globals
move with their only consumers; shmem.c holds none. Side benefit:
gating the global-defaults knobs behind
`TMPFS_COMPRESSION_GLOBAL_DEFAULTS` becomes a 3-line ifdef inside
shmem_compress.c — zero shmem.c cost.

### C3 — fill_super pool creation -> shmem_comp_fill_sb()

The 103-line block is pure compression policy: algo resolution (mount
option -> sysfs default -> Kconfig default), "none" handling,
cell-order derivation from tmpfs size, pool create, fallback to
uncompressed on pool failure. New types/functions:

    struct shmem_comp_opts { ... };   /* parsed mount options */
    struct shmem_comp_sb   { ... };   /* per-mount state */

    int  shmem_comp_fill_sb(struct shmem_sb_info *sbinfo,
                            struct shmem_comp_opts *opts,
                            unsigned long tmpfs_bytes);
    void shmem_comp_put_super(struct shmem_sb_info *sbinfo);

`tmpfs_bytes` is passed in so `shmem_default_max_blocks()` needs no
export. shmem.c keeps a 4-line call.

### C4 — sbinfo: 8 fields -> 1 opaque pointer

After C1-C3 shmem.c has zero reads of `comp_*` sbinfo fields.
`include/linux/shmem_fs.h` drops from +14 to +3:

    #ifdef CONFIG_TMPFS_COMPRESSION
    struct shmem_comp_sb;             /* mm/shmem_compress.c */
    ... struct shmem_comp_sb *comp;   /* NULL = uncompressed mount */
    #endif

The eight fields become private to shmem_compress.c. Largest
structural readability win: shmem.c stops knowing about compression,
not just textually mentioning it.

### C5 — parse_one thinning

The enum + `fs_parameter_spec` entries (~16 lines) stay in shmem.c:
the mount-option ABI must be greppable in one table alongside every
other tmpfs option. Case bodies shrink to stores into
`ctx->comp_opts`; `init_fs_context`/`free_fc`/`reconfigure` become
one-line calls to `shmem_comp_opts_init/free`. parse_one: 57 -> ~30
lines. The reconfigure seen-bit guard (~5 lines) stays in shmem.c —
remount policy is shmem's contract.

## Residual floor

| File | Before | After | Contents after carve |
|------|-------:|------:|----------------------|
| mm/shmem.c | +1,283 | ~110 | 3 dispatch hooks (~20), mount ABI + parse (~46), opts plumbing (~30), ifdef wrappers + decls (~30) |
| shmem.c exports | 0 | +2-3 | gated by CONFIG_TMPFS_COMPRESSION |
| include/linux/shmem_fs.h | +14 | +3 | forward decl + one pointer |
| include/linux/swap.h | +18 | +18 | irreducible: swap-type namespace |
| include/linux/swapops.h | +40 | ~40 | irreducible: entry encode/decode contract |
| fs/Kconfig + mm/Makefile | +32 | +32 | additive |
| **Total existing files** | **+1,387** | **~210** | |

~210 is the design floor. Anything smaller requires either hiding the
entry-type dispatch from shmem's page-cache paths (unreviewable — a
reader of eviction/fault must see that the xarray holds a second
value-entry type) or an ops-struct indirection (strictly more code,
single in-tree user, historically rejected).

## Readability floor — what stays in shmem.c, and why

- The three dispatch sites keep check + 1-2 line invariant comment:
  `is_shmem_comp_entry` in `shmem_free_swap`, `xa_is_shmem_comp` in
  `shmem_get_folio_gfp`, the compress call in `shmem_write_end`.
- The reconfigure seen-bit guard: remount semantics belong to shmem.
- The #ifdef wrappers (~20 lines): visible gating is what reviewers
  audit; do not minimize them.
- The fs_parameter_spec table: mount-option ABI lives with its peers.

## Comment policy

Two tiers, matching file ownership. Everything added to an
upstream-visible file sits inside an existing
`#ifdef CONFIG_TMPFS_COMPRESSION` region — additions (code and
comments) are gated together, so a disabled build's source diff is
the ifdef skeleton plus Kconfig. Project files
(`mm/shmem_compress.c`, `mm/cell_store.c`, `mm/cellzram.c`,
`include/linux/shmem_compress.h`, `include/linux/cell_store.h`) are
only compiled when the config is on (`obj-$(CONFIG_...)`), so their
comments are gated by the TU and can be verbose: these files are the
project's internal documentation, and no separate design docs ship
with them.

Rule of thumb for both tiers: a comment must be true and useful to a
maintainer who has never seen this project. Invariants and physical
justifications survive; what-happened narratives belong in commit
messages.

### Upstream-visible files — invariant comments only, <=3 lines each

| Site | Comment (intent) |
|------|------------------|
| shmem_write_end hook | compression runs after folio_unlock; the path takes the lock itself and it is not recursive; the caller's reference keeps the folio alive |
| shmem_get_folio_gfp hook | compressed entry -> decompress + install, replacing the entry; NULL only when the entry vanished, caller retries |
| shmem_free_swap hook | compression entries are not swap entries — release via the entry ID table; routing through the swap code WARNs and leaks the block |
| shmem_fs.h pointer | compression state for this mount; private to mm/shmem_compress.c |
| swap.h type slot | extend the existing block: entries live only in the page cache xarray, never in PTEs — no PTE swap-type space is consumed |
| export sites | exported for the tmpfs compression backend (mm/shmem_compress.c); gated so no API exists when CONFIG_TMPFS_COMPRESSION=n |

Existing shmem.c comments at these sites get a diet pass: narratives
collapse to invariants; consequence clauses ("WARNs and leaks") stay —
they document the failure mode a maintainer must not re-create.

swapops.h: wrap `make_shmem_comp_entry` / `shmem_comp_entry_id` /
`is_shmem_comp_entry` in `#ifdef CONFIG_TMPFS_COMPRESSION` — both call
sites are already gated, so this is safe and makes the disabled-build
diff to swapops.h the ifdef skeleton alone.

### Project files — verbose, deliberately

- `mm/shmem_compress.c` header block: extends the existing design
  overview with the carve contract — this TU is the sole in-tree
  consumer of the exported shmem primitives; adding a second consumer
  is a design change, not a convenience. Plus fill_sb ownership rules
  (who frees opts strings on each failure path, mount-failure vs
  graceful-uncompressed semantics), sysfs registration lifetime,
  defaults storage ownership.
- `mm/shmem_compress.h`: kerneldoc per new type/function —
  `shmem_comp_opts` field-by-field (units, what "unset" means, the
  threshold-0-equals-default quirk), `shmem_comp_sb` lifecycle,
  fill_sb/put_super contracts, sysfs_register parent-kobject rules.
- `mm/cell_store.h`: already carries the allocator invariants; keep
  them authoritative. The 8 GiB sizing fixes (derived max_cells,
  struct cell -> single page pointer, u16 -> u32 cellid/live_objects)
  land with the allocator-capacity work, not with the carve.
- `mm/cellzram.c` (when built): file header carries the reuse contract
  with shmem_comp — which entry semantics the device relies on (ref
  pin, relocation-safe load_entry, free dispatch via entry_put), and
  that its pools appear under the tmpfs-named sysfs group
  provisionally.

## Commit sequence

1. C2 (sysfs + defaults) — no exports, pure move.
2. C3 + C4 + C5 together (fill_sb + opts struct + sbinfo collapse are
   one data-structure change; splitting leaves intermediate states
   where shmem.c reads fields C4 removed).
3. C1 (767-block move) — the export commit; rig-green before/after.
4. shmem_undo_range -> shmem_truncate_range substitution — own commit,
   behavior delta (timestamps, iversion) reviewed separately.
5. Comment diet + swapops.h ifdef wrap — documentation-only commit.

Each step is behavior-neutral and independently testable on the rig;
the sweep and full suite are the acceptance gate per commit.

## Review pre-answers

- Swap-type slot: `SWP_SHMEM_COMPRESS` consumes one entry type from
  the shared namespace (pattern: SWP_DEVICE_*, SWP_PTE_MARKER).
  Compression entries never appear in PTEs — page-cache xarray only —
  so PTE type-space width is unaffected.
- Exports: 2-3, each consumed by exactly one in-tree TU, precedent
  `shmem_write_folio`. Fallback if the maintainer objects: keep the
  767-block in shmem.c (status quo) — correctness-neutral.
- Documentation: mount options + sysfs go in
  Documentation/filesystems/tmpfs.rst; TMPFS_COMPRESSION_PLAN.md and
  mission_log/ are project artifacts and never enter the series.
- Device metadata: cellzram reports entry-table RAM separately in
  mm_stat (`entry_bytes`) so the vs-zram comparison is like-for-like;
  per-block entries are ~64 B and remain the v2 batched-entry lever
  at 8 GiB scale.

---

# Option Surface v2 -- coherent model (2026-10-07)

The option surface grew organically across sessions and accumulated
incoherences: two spellings for the algorithm and merge knobs
(compress=/compressor=, compress_merge=/merge_pages=), three different
"off" spellings with subtly different meanings, unit soup (cell= takes
memparse syntax, compress_threshold= raw bytes), a threshold default
(64K) that silently disables compression for small-file farm
workloads, and a /proc/mounts that shows nothing about compression
(the cell256k torture bug -- a mount benchmarking as plain tmpfs --
was invisible exactly there).

## Three layers, one concept per knob, no aliases

**Layer 1 -- WHAT (participation + algorithm):**

| knob | syntax | meaning |
|------|--------|---------|
| `compress=<algo>` | string; `none` = off | algorithm; presence = participate |
| `nocompress` | flag | explicit off |

All "off" spellings normalize to one resolved state: no pool, no
memory overhead.

**Layer 2 -- WHEN (policy dial):**

| knob | syntax | sentinels |
|------|--------|-----------|
| `compress_watermark=LOW:HIGH` | percents, LOW <= HIGH | `0:0` = always (eager); `100:100` = never (identical to nocompress, no pool); `50:100` = pure deferral (maintenance-only aging); `50:70` = default target |

HIGH: new writes compress inline at usage% >= HIGH.
LOW: return to chill at usage% <= LOW (hysteresis band in between).
usage = used_blocks / max_blocks (relative to size=, default half of
RAM). Mode flips happen in the maintenance pass, never per write --
the write path cost is one READ_ONCE(comp->hot). Name chosen over
"compress_ratio": ratio already means compression ratio in comp_stats.

**Layer 3 -- GEOMETRY (storage shape):**

| knob | syntax | meaning |
|------|--------|---------|
| `merge_pages=N` | pages per group | large-file knob; leave at 1 for small/mixed workloads (torture-proven: groups that never fill stay uncompressed, collapsing ratio on small-file farms) |
| `cell=SIZE\|0` | memparse; 0 = zsmalloc backend | absent = auto-derived from tmpfs size (+1 order when merge>=8); derivation printed at mount |
| `compress_threshold=SIZE` | memparse suffixes now (`64K`) | applies to WRITE-TIME compression only (eager/hot path); the aging pass compresses any eligible survivor >= 1 page -- small survivors are worth their RAM |

**Global sysfs mirrors the defaults** (/sys/fs/tmpfs/compression/):
`compressor` (algo or none), `compression` (bool: do new mounts
participate at all), `merge_pages`, `compress_watermark` (new).
Precedence: mount option > sysfs default > built-in. The resolved
state prints as one complete line at mount (including WHY a mount is
uncompressed: nocompress / compress=none / watermark=100:100) and is
exposed via show_options, so /proc/mounts tells the truth.

**Deliberately internal (no knobs):** cell_order derivation, GC and
maintenance batch sizes, per-CPU contexts, entry table. Observable
(printed at mount / in cell_stats) but free to change.

## Compression policy v2 -- death-before-pop aging

Transient files are the point of tmpfs; compressing them at write
time spends CPU on data that dies unread (the torture workload's .d
depfiles and punch-hole temps). New policy, in stages:

1. **Stage 1 (this commit):** the watermark knob lands end-to-end
   (parse -> opts -> fill_sb -> sb -> show_options -> sysfs default)
   with the built-in default `0:0` (eager = today's behavior) so the
   surface is real before the machinery changes behavior. The never
   sentinel (100:100) already works: no pool created.

2. **Stage 2 (next):** deferred compression with death-before-pop.
   Chill-mode writes land uncompressed and drop a cheap candidate
   marker (list_add under a spinlock); unlink/truncate purges the
   marker (death-before-pop = free win, quantified as skipped_dead).
   Hot mode (usage >= HIGH) compresses inline as today. The
   maintenance pass -- one shmem_comp_maintain() per mount, triggered
   by the shrinker, gc_now, and a periodic delayed_work when
   candidates are pending -- MANDATORILY compresses eligible
   survivors (order-0, uptodate, unmapped, unpinned; clean pages
   included; threshold ignored: survivors of any size are worth it),
   then evacuates sparse cells, alternating budgets ~4:1 so crypto
   and GC memcpy do not starve each other. Compression raises cell
   density -> fewer sparse cells -> cheaper GC. Mode flips (with
   hysteresis) happen in the same pass.

3. **Why the 70% mark behaves like a soft limit:** compression
   releases size= accounting -- compressed entries are accounted like
   swap pages and shmem_recalc_inode() unaccounts blocks as they
   become value entries. Hot-mode compression therefore lowers
   used_blocks, dropping usage back below the mark; effective
   capacity for compressible data is size= / ratio.

Validation: the torture sweep becomes the policy matrix -- configs
for never / 0:0 / 50:70 / 50:100 against today's eager numbers,
predicting T=1 single-job throughput toward plain (26 vs 17.1 MB/s),
skipped_dead absorbing transient churn, survivor ratio unchanged
(~48% stored), and bounded Shmem overshoot (max sampled in-window).

## Stage 2 machinery -- walk-based aging (implementation design, 2026-10-07)

The policy above (death-before-pop, hysteresis flips, 4:1 aging:GC) is
unchanged. What changed is the candidate-finding MECHANISM: the policy
section sketched "a cheap candidate marker (list_add under a spinlock)";
code analysis against the real tree rejected markers. Decision:
**walk-based aging** -- the maintenance pass walks the mount's page
caches. Recorded here because it amends the policy section before any
Stage 2 code exists (plan-first protocol, session 3).

### Why not the marker list

- A marker holding no folio reference dangles when shmem_writeout()
  swaps an uncompressed chilled folio out -- writeout never touches
  undo_range, so no truncate-side purge would run. Fixing that needs a
  third purge hook in the swapout path, widening the shmem.c blast
  radius the session-4 carve just shrank.
- A marker pinning the inode (igrab) breaks tmpfs delete semantics:
  tmpfs truncates at EVICT (shmem_evict_inode -> shmem_truncate_range),
  and evict runs only at i_count == 0. A pinned inode cannot evict, so
  a chilled file deleted under pressure keeps its pages until the pass
  runs -- and unmount with pending pins is the busy-inodes hazard.
- Walk-based aging has no new lifetime rules at all: a truncated page
  is simply absent from the xarray when the pass arrives. There is
  nothing to purge, so there is nothing to purge wrong.

### Mechanism

Per-mount state additions to struct shmem_comp_sb:

- `bool hot` -- the mode flag. Write path: one READ_ONCE. Pass:
  WRITE_ONCE at flips. Initial value at fill_sb: (wm_high == 0), i.e.
  eager mounts start hot, 50:70 / 50:100 start chill.
- `struct super_block *sb` -- back-pointer for the inode walk and
  usage computation (fill_sb gains an sb parameter).
- `struct delayed_work age_work` + `struct mutex maintain_lock` --
  one pass per mount, serialized (work + gc_now + shrinker can race).
- `struct list_head list` on a global shmem_comp_mounts list -- gc_now
  iterates mounts; put_super removes before freeing comp and runs
  cancel_delayed_work_sync() first.
- `atomic_long_t aged_pages, skipped_dead` -- per-mount tallies
  (global stats for comp_stats; per-mount readers remain the standing
  carry-over).

Write path (shmem_write_end): `should_compress` gains `READ_ONCE(hot)`;
the else-branch calls `shmem_comp_arm_aging(inode)` -- a no-op unless
the mount participates AND the write left an uncompressed candidate
(chill write, or sub-threshold write in hot mode). Arming is
schedule_delayed_work, cheap when already pending. No allocation, no
new state per write.

`shmem_comp_maintain(comp)`, one pass:

1. usage = max_blocks ? used_blocks * 100 / max_blocks : 0 (percpu
   sum, sbinfo via comp->sb). Flip rules, hysteresis band (LOW, HIGH)
   keeps current mode:
   - usage >= wm_high -> hot
   - usage <= wm_low  -> chill
   For 0:0 the first rule always holds -> permanently hot, never
   flips to chill (the empty-fs + integer-division trap: 1 page on a
   large tmpfs reads usage 0; the sentinel must not flinch).
   pr_info once per flip (rare; greppable in serial logs).
2. Aging: walk sb->s_inodes (prune_icache pinning pattern: i_lock,
   skip I_FREEING/I_WILL_FREE, __iget). For each shmem inode, walk
   the page cache with find_get_entries() (the undo_range pattern --
   folios come back referenced, values as exceptionals). Per folio:
   xa_is_value -> skip (compressed entry or swap; NEVER swapin from
   the pass); lock; revalidate folio->mapping == mapping && uptodate;
   skip mapped/pinned/order>0; compress via the single-page core with
   allow_clean=TRUE. Threshold is ignored: any eligible survivor >=
   1 page is aged (this is what dissolves grate #2 -- the 64K
   write-time threshold no longer condemns small survivors to stay
   uncompressed forever).
   merge_pages interaction (v1): the pass always compresses SINGLE
   pages, bypassing the group dispatch. Compatible by construction:
   entries carry their own nr_pages and the decompress path dispatches
   per entry, so single-page entries coexist with write-time groups on
   the same mount. Group-aware aging is a follow-up if the policy
   matrix shows the ratio cost matters.
3. Budget alternation ~4:1: each round ages up to
   SHMEM_COMP_AGE_BUDGET (128) pages, then -- cell-backed pools only
   -- runs one shmem_comp_cell_gc() batch; up to SHMEM_COMP_AGE_MAX
   ROUNDS (4) rounds per pass (~2 MB aged max). Budget bounds the
   per-pass cost regardless of data compressibility (see gaps).
   zsmalloc pools: rounds age only, GC is the pool's own shrinker's
   job.
4. Re-arm: if a pass hit its round budget (candidates likely
   remain), schedule_delayed_work again; otherwise idle until the
   next write arms it.

`skipped_dead` accounting: shmem_comp_note_unmap(inode) called from
shmem_undo_range() adjacent to each FULL truncate_inode_folio() (both
loops; partial large-folio truncates are NOT deaths -- the folio
survives). Counts only when the mount is chilled (READ_ONCE(!hot)):
eager-mode deaths of uncompressed pages are not policy wins (those
pages were attempted at write time -- incompressible/mapped/below
threshold). Covers unlink, truncate, fallocate punch, and evict (evict
routes through shmem_truncate_range). The metric: pages that died
without paying a compression cycle -- the CPU death-before-pop saved.

Triggers:

- Armed delayed_work (above): writes arm it; delay
  SHMEM_COMP_AGE_DELAY_MS (1000).
- gc_now (sysfs): extended from cell-GC-all to also run one maintain
  pass per compressed mount (global mounts list) before the pool GC.
  Deterministic test trigger for the policy matrix.
- Shrinker: the existing cell-pool shrinker scan ARMS the work for
  mounts on that pool instead of compressing inline -- allocation
  under fs_reclaim (crypto contexts, kmalloc buffers, cell alloc_pages)
  is the reclaim-recursion hazard session 3 already hit once
  (alloc_pages(GFP_KERNEL) under cp->lock). Aging from the workqueue
  keeps GFP_KERNEL honest. Deviation from the plan text, recorded:
  "triggered by the shrinker" is satisfied asynchronously.
  zsmalloc-backed mounts have no such shrinker and rely on the armed
  work + gc_now in v1.

Core refactor: shmem_compress_folio()'s single-page body moves to
shmem_comp_compress_one(inode, index, folio, bool allow_clean). The
write path calls with allow_clean=false (unchanged: "write_end just
dirtied this folio; clean means the state changed under us"); the pass
calls with allow_clean=true. Existing race hardening (folio lock,
mapping revalidate, expected-value slot swap) is shared, not copied.

### Known v1 gaps (deliberate, measured-then-fixed)

1. Incompressible retry: the walk's candidate set is "uncompressed
   pages", which includes pages that failed compression at write time
   (hot mode). Each pass retries them within its budget. The budget
   bounds the CPU cost per pass; the waste is budget consumed by noise
   (quality, not stability). The proper fix is the session-1 entropy
   probe -- lands if the policy matrix's noise rows demand it.
   (The rejected marker design skipped these for free: markers existed
   only for never-attempted chilled pages.)
2. Chilled pages can still be written to swap uncompressed (writeout
   bypasses compression). v1 accepts it; compress-at-swapout is a
   separate lever.
3. Read-decompressed pages get recompressed by the next pass (clean
   pages are eligible by design). Thrash is bounded by the armed delay
   + budget; a referenced-bit eligibility check is the follow-up if
   read-heavy mounts show churn in the matrix.
4. Per-mount stats readers (comp_pages/comp_bytes/aged/skipped) remain
   open; the matrix uses global deltas with one mount active per
   config, plus pr_info flip lines.

### Landing sequence (bisectable, each rig-gated)

1. This amendment (docs only).
2. Machinery commit: hot flag + write-path consult + arm hook +
   maintain pass + skipped_dead hooks + delayed_work/mounts list +
   gc_now extension + stats + fill_sb(sb)/put_super teardown. Built-in
   default STAYS 0:0 -- eager mounts keep today's inline behavior; the
   only behavior change is sub-threshold pages now aging (grate #2)
   and explicit non-eager watermarks finally doing something. Config
   off-n build gate (session 4 rule: shmem.c touched).
3. Rig: new test_watermark_policy (chill mount: writes land
   uncompressed, gc_now ages them, unlink counts skipped_dead; eager
   mount: sub-threshold pages age; hysteresis flip via size= pressure).
4. Policy matrix on the torture sweep (never / 0:0 / 50:70 / 50:100
   vs session-7 eager numbers; watch stale_value_retries).
5. Default flip 0:0 -> 50:70 + suite + matrix re-run (final step,
   after the matrix validates 50:70).

## Status (2026-10-07)

Stage 1 landed as four commits (`a222524`..`275a232`): aliases
dropped, compress_threshold memparse units, compress_watermark=LOW:HIGH
end-to-end (mount parse -> watermark_seen -> fill_sb sentinels -> sb ->
show_options -> sysfs default knob, built-in default 0:0),
resolved-state mount lines with why-off, /proc/mounts truth via
shmem_comp_show_options(), and sysfs compressor=none normalization
(stored as-is; no Kconfig fallback).

Entry-ID-reuse race: CLOSED (session 10). Root causes were a phantom
put in the cell-backed group decompress path and ABA via id-only
xarray values; fixed by generation-encoded entries (32-bit id +
23-bit generation in the 55-bit swp offset, validated at pin and
release; `stale_value_retries` in comp_stats) and a single balanced
group pin. Corruption policy landed with it: refs/slots underflow,
double teardown (VM_BUG_ON), and decompressed-length mismatches
PANIC (user rule: any data corruption stops the machine); staleness
is a counted, retried race outcome. Gate recipe:
TEST_ENTRYRACE=1 QEMU_CPUS=4 -- 0 underflows, quiescent verify
12/12 PASS, full suite exit 0. Mid-race zero pages in that test are
POSIX truncate-vs-read TOCTOU, not corruption; the verdict is the
quiescent verify (mission log session 10 has the full story).
Stage 2 (machinery + default flip to 50:70) is next.
