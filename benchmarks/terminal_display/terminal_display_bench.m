//
//  terminal_display_bench.m
//
//  Standalone microbenchmarks for iSH terminal output display paths.
//

#import <Foundation/Foundation.h>
#import <mach/mach_time.h>

#define GHOSTTY_STATIC 1
#import <ghostty/vt.h>

typedef struct {
    const char *name;
    NSMutableData *payload;
} BenchPayload;

typedef struct {
    double total_ms;
    double mbps;
    double p50_us;
    double p95_us;
    NSUInteger checksum;
} BenchResult;

static double now_ns(void) {
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);
    uint64_t t = mach_absolute_time();
    return (double) t * (double) timebase.numer / (double) timebase.denom;
}

static int compare_double(const void *a, const void *b) {
    double lhs = *(const double *) a;
    double rhs = *(const double *) b;
    return (lhs > rhs) - (lhs < rhs);
}

static NSMutableData *repeat_line(NSUInteger target_bytes, NSString *line) {
    NSMutableData *payload = [NSMutableData dataWithCapacity:target_bytes + line.length];
    NSData *bytes = [line dataUsingEncoding:NSISOLatin1StringEncoding];
    while (payload.length < target_bytes)
        [payload appendData:bytes];
    return payload;
}

static BenchResult bench_hterm_bridge(NSData *payload, NSUInteger chunk_size) {
    NSMutableArray<NSNumber *> *samples = [NSMutableArray array];
    NSUInteger checksum = 0;
    double start = now_ns();
    for (NSUInteger offset = 0; offset < payload.length; offset += chunk_size) {
        NSUInteger len = MIN(chunk_size, payload.length - offset);
        double chunk_start = now_ns();
        NSString *dataString = [[NSString alloc] initWithBytes:(const char *) payload.bytes + offset
                                                        length:len
                                                      encoding:NSISOLatin1StringEncoding];
        dataString = [dataString stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
        dataString = [dataString stringByReplacingOccurrencesOfString:@"\r" withString:@"\\r"];
        dataString = [dataString stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
        dataString = [dataString stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
        NSString *js = [NSString stringWithFormat:@"exports.write(\"%@\")", dataString];
        checksum += js.length;
        [samples addObject:@((now_ns() - chunk_start) / 1000.0)];
    }
    double elapsed_ms = (now_ns() - start) / 1000000.0;

    NSUInteger n = samples.count;
    double *sorted = calloc(n, sizeof(double));
    for (NSUInteger i = 0; i < n; i++)
        sorted[i] = samples[i].doubleValue;
    qsort(sorted, n, sizeof(double), compare_double);
    BenchResult result = {
        .total_ms = elapsed_ms,
        .mbps = ((double) payload.length / 1024.0 / 1024.0) / (elapsed_ms / 1000.0),
        .p50_us = sorted[n / 2],
        .p95_us = sorted[(NSUInteger) floor((double) (n - 1) * 0.95)],
        .checksum = checksum,
    };
    free(sorted);
    return result;
}

static BenchResult bench_ghostty(NSData *payload, NSUInteger chunk_size) {
    GhosttyTerminal terminal = NULL;
    GhosttyTerminalOptions options = {
        .cols = 100,
        .rows = 30,
        .max_scrollback = 10 * 1000 * 1000,
    };
    if (ghostty_terminal_new(NULL, &terminal, options) != GHOSTTY_SUCCESS) {
        fprintf(stderr, "failed to create Ghostty terminal\n");
        exit(2);
    }

    GhosttyRenderState render_state = NULL;
    if (ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        fprintf(stderr, "failed to create Ghostty render state\n");
        exit(2);
    }

    NSMutableArray<NSNumber *> *samples = [NSMutableArray array];
    NSUInteger checksum = 0;
    double start = now_ns();
    for (NSUInteger offset = 0; offset < payload.length; offset += chunk_size) {
        NSUInteger len = MIN(chunk_size, payload.length - offset);
        double chunk_start = now_ns();
        ghostty_terminal_vt_write(terminal, (const uint8_t *) payload.bytes + offset, len);
        ghostty_render_state_update(render_state, terminal);
        uint16_t cols = 0;
        uint16_t rows = 0;
        ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
        ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);
        checksum += cols + rows;
        [samples addObject:@((now_ns() - chunk_start) / 1000.0)];
    }
    double elapsed_ms = (now_ns() - start) / 1000000.0;

    NSUInteger n = samples.count;
    double *sorted = calloc(n, sizeof(double));
    for (NSUInteger i = 0; i < n; i++)
        sorted[i] = samples[i].doubleValue;
    qsort(sorted, n, sizeof(double), compare_double);
    BenchResult result = {
        .total_ms = elapsed_ms,
        .mbps = ((double) payload.length / 1024.0 / 1024.0) / (elapsed_ms / 1000.0),
        .p50_us = sorted[n / 2],
        .p95_us = sorted[(NSUInteger) floor((double) (n - 1) * 0.95)],
        .checksum = checksum,
    };
    free(sorted);

    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
    return result;
}

static void print_result(NSString *payload_name, NSString *path_name, BenchResult result) {
    printf("%-14s %-16s %9.2f ms %9.2f MiB/s p50 %8.2f us p95 %8.2f us checksum %lu\n",
           payload_name.UTF8String,
           path_name.UTF8String,
           result.total_ms,
           result.mbps,
           result.p50_us,
           result.p95_us,
           (unsigned long) result.checksum);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSUInteger target_bytes = 16 * 1024 * 1024;
        NSUInteger chunk_size = 4096;
        if (argc > 1)
            target_bytes = (NSUInteger) strtoull(argv[1], NULL, 10);
        if (argc > 2)
            chunk_size = (NSUInteger) strtoull(argv[2], NULL, 10);

        BenchPayload payloads[] = {
            {"plain", repeat_line(target_bytes, @"0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n")},
            {"sgr", repeat_line(target_bytes, @"\033[31mred\033[0m \033[32mgreen\033[0m \033[1;34mbold-blue\033[0m plain text\r\n")},
            {"cursor", repeat_line(target_bytes, @"\033[2J\033[Htop-left\033[10;20Hmiddle\033[30;1Hbottom row wraps wraps wraps wraps\r\n")},
        };
        NSUInteger payload_count = sizeof(payloads) / sizeof(payloads[0]);

        printf("payload_bytes=%lu chunk_bytes=%lu\n",
               (unsigned long) target_bytes,
               (unsigned long) chunk_size);
        for (NSUInteger i = 0; i < payload_count; i++) {
            NSData *payload = payloads[i].payload;
            BenchResult before = bench_hterm_bridge(payload, chunk_size);
            BenchResult after = bench_ghostty(payload, chunk_size);
            print_result(@(payloads[i].name), @"hterm-bridge", before);
            print_result(@(payloads[i].name), @"ghostty-vt", after);
        }
    }
    return 0;
}
