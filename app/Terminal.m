//
//  Terminal.m
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import "Terminal.h"
#import "DelayedUITask.h"
#import "UserPreferences.h"
#include "LinuxInterop.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/devices.h"

extern struct tty_driver ios_pty_driver;

#if !ISH_LINUX
typedef struct tty *tty_t;
#else
typedef struct linux_tty *tty_t;
#endif

@interface Terminal () {
#if !ISH_LINUX
    lock_t _dataLock;
    cond_t _dataConsumed;
#endif
}

@property BOOL loaded;
@property (nonatomic) tty_t tty;
// lock with dataLock for !linux and @synchronized(self) for linux
@property (nonatomic) NSMutableData *pendingData;
// sending output is an asynchronous thing due to javascript, this is used to ensure it doesn't happen twice at once
@property (nonatomic) BOOL outputInProgress;

@property DelayedUITask *refreshTask;
@property DelayedUITask *scrollToBottomTask;

@property BOOL applicationCursor;

@property NSNumber *terminalsKey;
@property NSUUID *uuid;

@end

@implementation Terminal
@synthesize displayView = _displayView;

static const int BUF_SIZE = 1<<14;

static NSMapTable<NSNumber *, Terminal *> *terminals;
static NSMapTable<NSUUID *, Terminal *> *terminalsByUUID;

- (instancetype)initWithType:(int)type number:(int)num {
    @synchronized (Terminal.class) {
        self.terminalsKey = @(dev_make(type, num));
        Terminal *terminal = [terminals objectForKey:self.terminalsKey];
        if (terminal)
            return terminal;

        if (self = [super init]) {
            self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
            self.refreshTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(refresh)];
            self.scrollToBottomTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(scrollToBottom)];
#if !ISH_LINUX
            lock_init(&_dataLock);
            cond_init(&_dataConsumed);
#endif

            [terminals setObject:self forKey:self.terminalsKey];
            self.uuid = [NSUUID UUID];
            [terminalsByUUID setObject:self forKey:self.uuid];
            self.loaded = YES;
        }
        return self;
    }
}

- (GhosttyTerminalDisplay *)displayView {
    if (_displayView == nil) {
        CGRect displaySize = CGRectMake(0, 0, 10000, 10000);
        _displayView = [[GhosttyTerminalDisplay alloc] initWithFrame:displaySize];
        _displayView.delegate = self;
    }
    return _displayView;
}

#if !ISH_LINUX
+ (Terminal *)createPseudoTerminal:(struct tty **)tty {
    *tty = pty_open_fake(&ios_pty_driver);
    if (IS_ERR(*tty))
        return nil;
    return (__bridge Terminal *) (*tty)->data;
}
#endif

- (void)setTty:(tty_t)tty {
    @synchronized (self) {
        _tty = tty;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self syncWindowSize];
    });
}

- (void)syncWindowSize {
    int cols = self.displayView.columns;
    int rows = self.displayView.rows;
    if (self.tty == NULL)
        return;
#if !ISH_LINUX
    lock(&self.tty->lock);
    tty_set_winsize(self.tty, (struct winsize_) {.col = cols, .row = rows});
    unlock(&self.tty->lock);
#else
    async_do_in_workqueue(^{
        self->_tty->ops->resize(self->_tty, cols, rows);
    });
#endif
}

- (void)setEnableVoiceOverAnnounce:(BOOL)enableVoiceOverAnnounce {
    _enableVoiceOverAnnounce = enableVoiceOverAnnounce;
    self.displayView.enableVoiceOverAnnounce = enableVoiceOverAnnounce;
}

- (int)sendOutput:(const void *)buf length:(int)len {
#if !ISH_LINUX
    lock(&_dataLock);
    if (!NSThread.isMainThread) {
        // The main thread is the only one that can unblock this, so sleeping here would be a deadlock.
        // The only reason for this to be called on the main thread is if input is echoed.
        while (_pendingData.length > BUF_SIZE)
            wait_for_ignore_signals(&_dataConsumed, &_dataLock, NULL);
    }
    [_pendingData appendData:[NSData dataWithBytes:buf length:len]];
    [self.refreshTask schedule];
    unlock(&_dataLock);
#else
    @synchronized (self) {
        int room = [self roomForOutput];
        if (len > room)
            len = room;
        if (len > 0) {
            [_pendingData appendData:[NSData dataWithBytes:buf length:len]];
            [_refreshTask schedule];
        }
    }
#endif
    return len;
}

#if ISH_LINUX
- (int)roomForOutput {
    @synchronized (self) {
        if (_pendingData.length > BUF_SIZE)
            return 0;
        return BUF_SIZE - (int) _pendingData.length;
    }
}
#endif

- (void)sendInput:(NSData *)input {
    if (self.tty == NULL)
        return;
#if !ISH_LINUX
    tty_input(self.tty, input.bytes, input.length, 0);
#else
    async_do_in_workqueue(^{
        NSData *inputRef = input;
        self.tty->ops->send_input(self.tty, inputRef.bytes, inputRef.length);
    });
#endif
    [self.scrollToBottomTask schedule];
}

- (void)scrollToBottom {
    [self.displayView scrollToBottom];
}

- (NSString *)arrow:(char)direction {
    return [NSString stringWithFormat:@"\x1b%c%c", self.displayView.applicationCursorMode ? 'O' : '[', direction];
}

- (void)refresh {
    if (!self.loaded)
        return;

#if !ISH_LINUX
    lock(&_dataLock);
    if (_outputInProgress) {
        [self.refreshTask schedule];
        unlock(&_dataLock);
        return;
    }
    NSData *data = _pendingData;
    _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
    _outputInProgress = YES;
    notify(&self->_dataConsumed);
    unlock(&_dataLock);
#else
    NSData *data;
    @synchronized (self) {
        if (_outputInProgress) {
            [self.refreshTask schedule];
            return;
        }
        data = _pendingData;
        _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
        _outputInProgress = YES;
        if (self->_tty)
            async_do_in_irq(^{
                self->_tty->ops->can_output(self->_tty);
            });
    }
#endif

    [self.displayView writeBytes:data.bytes length:data.length completion:^{
#if !ISH_LINUX
        lock(&self->_dataLock);
        self->_outputInProgress = NO;
        unlock(&self->_dataLock);
#else
        @synchronized (self) {
            self->_outputInProgress = NO;
        }
#endif
    }];
}

- (void)ghosttyTerminalDisplayDidResize:(GhosttyTerminalDisplay *)display columns:(int)columns rows:(int)rows {
    (void) display;
    (void) columns;
    (void) rows;
    [self syncWindowSize];
}

- (void)ghosttyTerminalDisplay:(GhosttyTerminalDisplay *)display writePtyBytes:(const uint8_t *)bytes length:(size_t)length {
    (void) display;
    NSData *data = [NSData dataWithBytes:bytes length:length];
    [self sendInput:data];
}

- (void)ghosttyTerminalDisplayDidUpdateScrollback:(GhosttyTerminalDisplay *)display totalRows:(NSUInteger)totalRows offset:(NSUInteger)offset visibleRows:(NSUInteger)visibleRows {
    (void) display;
    (void) totalRows;
    (void) offset;
    (void) visibleRows;
}

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize {
    char *p = argv;
    for (NSString *cmd in command) {
        const char *c = cmd.UTF8String;
        // Save space for the final NUL byte in argv
        while (p < argv + maxSize - 1 && (*p++ = *c++));
        // If we reach the end of the buffer, the last string still needs to be
        // NUL terminated
        *p = '\0';
    }
    // Add the final NUL byte to argv
    *++p = '\0';
}

+ (Terminal *)terminalWithType:(int)type number:(int)number {
    return [[Terminal alloc] initWithType:type number:number];
}

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid {
    @synchronized (Terminal.class) {
        return [terminalsByUUID objectForKey:uuid];
    }
}

- (void)destroy {
    tty_t tty = self.tty;
    if (tty != NULL) {
#if !ISH_LINUX
        if (tty != NULL) {
            lock(&tty->lock);
            tty_hangup(tty);
            unlock(&tty->lock);
        }
#else
        tty->ops->hangup(tty);
#endif
    }
    @synchronized (Terminal.class) {
        [terminals removeObjectForKey:self.terminalsKey];
    }
}

+ (void)initialize {
    if (self == Terminal.class) {
        terminals = [NSMapTable strongToWeakObjectsMapTable];
        terminalsByUUID = [NSMapTable strongToWeakObjectsMapTable];
    }
}

@end

#if ISH_LINUX
nsobj_t Terminal_terminalWithType_number(int type, int number) {
    return CFBridgingRetain([Terminal terminalWithType:type number:number]);
}
int Terminal_sendOutput_length(nsobj_t _self, const char *data, int size) {
    return [(__bridge Terminal *) _self sendOutput:data length:size];
}
int Terminal_roomForOutput(nsobj_t _self) {
    return [(__bridge Terminal *) _self roomForOutput];
}
void Terminal_setLinuxTTY(nsobj_t _self, struct linux_tty *tty) {
    return [(__bridge Terminal *) _self setTty:tty];
}
#endif

#if !ISH_LINUX
static int ios_tty_init(struct tty *tty) {
    // This is called with ttys_lock but that results in deadlock since the main thread can also acquire ttys_lock. So release it.
    unlock(&ttys_lock);
    void (^init_block)(void) = ^{
        Terminal *terminal = [Terminal terminalWithType:tty->type number:tty->num];
        tty->data = (void *) CFBridgingRetain(terminal);
        terminal.tty = tty;
    };
    if ([NSThread isMainThread])
        init_block();
    else
        dispatch_sync(dispatch_get_main_queue(), init_block);

    lock(&ttys_lock);
    return 0;
}

static int ios_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    Terminal *terminal = (__bridge Terminal *) tty->data;
    return [terminal sendOutput:buf length:(int) len];
}

static void ios_tty_cleanup(struct tty *tty) {
    Terminal *terminal = CFBridgingRelease(tty->data);
    tty->data = NULL;
    terminal.tty = NULL;
}

struct tty_driver_ops ios_tty_ops = {
    .init = ios_tty_init,
    .write = ios_tty_write,
    .cleanup = ios_tty_cleanup,
};
DEFINE_TTY_DRIVER(ios_console_driver, &ios_tty_ops, TTY_CONSOLE_MAJOR, 64);
struct tty_driver ios_pty_driver = {.ops = &ios_tty_ops};
#endif
