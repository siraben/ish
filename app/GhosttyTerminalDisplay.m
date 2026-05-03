//
//  GhosttyTerminalDisplay.m
//  iSH
//
//  Native terminal display backed by libghostty-vt.
//

#import "GhosttyTerminalDisplay.h"
#import "Theme.h"

#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#define GHOSTTY_STATIC 1
#import <ghostty/vt.h>

static const int DefaultColumns = 80;
static const int DefaultRows = 24;
static const size_t MaxScrollbackBytes = 10 * 1000 * 1000;
static const CGFloat CellWidthAdjustment = -0.5;

static void GhosttyWritePty(GhosttyTerminal terminal, void *userdata, const uint8_t *data, size_t len) {
    (void) terminal;
    GhosttyTerminalDisplay *display = (__bridge GhosttyTerminalDisplay *) userdata;
    [display.delegate ghosttyTerminalDisplay:display writePtyBytes:data length:len];
}

static GhosttyColorRgb GhosttyColorFromHex(NSString *hex, GhosttyColorRgb fallback) {
    UIColor *color = [[UIColor alloc] ish_initWithHexString:hex];
    CGFloat r = 0;
    CGFloat g = 0;
    CGFloat b = 0;
    CGFloat a = 0;
    if (![color getRed:&r green:&g blue:&b alpha:&a])
        return fallback;
    return (GhosttyColorRgb) {
        .r = (uint8_t) lrint(r * 255),
        .g = (uint8_t) lrint(g * 255),
        .b = (uint8_t) lrint(b * 255),
    };
}

static UIColor *UIColorFromGhosttyColor(GhosttyColorRgb color) {
    return [UIColor colorWithRed:color.r / 255.0 green:color.g / 255.0 blue:color.b / 255.0 alpha:1];
}

static UIFont *TerminalFontForFamily(NSString *fontFamily, CGFloat size, UIFontWeight weight) {
    if (@available(iOS 13.4, *)) {
        if ([fontFamily isEqualToString:@"ui-monospace"])
            return [UIFont monospacedSystemFontOfSize:size weight:weight];
    }

    UIFont *font = [UIFont fontWithName:fontFamily size:size];
    if (font == nil)
        font = [UIFont fontWithName:@"Menlo" size:size];
    if (font == nil) {
        if (@available(iOS 13.0, *))
            return [UIFont monospacedSystemFontOfSize:size weight:weight];
        return [UIFont systemFontOfSize:size weight:weight];
    }

    if (weight <= UIFontWeightRegular)
        return font;

    UIFontDescriptorSymbolicTraits traits = font.fontDescriptor.symbolicTraits | UIFontDescriptorTraitBold;
    UIFontDescriptor *boldDescriptor = [font.fontDescriptor fontDescriptorWithSymbolicTraits:traits];
    return boldDescriptor ? [UIFont fontWithDescriptor:boldDescriptor size:size] : font;
}

static CGFloat PixelRound(CGFloat value, CGFloat scale) {
    if (scale <= 0)
        scale = 1;
    return round(value * scale) / scale;
}

static CGFloat TerminalASCIIAdvance(UIFont *font) {
    enum { CharacterCount = 127 - 32 };
    UniChar characters[CharacterCount] = {};
    for (NSUInteger i = 0; i < CharacterCount; i++)
        characters[i] = (UniChar) (32 + i);

    CTFontRef ctFont = (__bridge CTFontRef) font;
    CGGlyph glyphs[CharacterCount] = {};
    if (!CTFontGetGlyphsForCharacters(ctFont, characters, glyphs, CharacterCount))
        return [@"W" sizeWithAttributes:@{NSFontAttributeName: font}].width;

    CGSize advances[CharacterCount] = {};
    CTFontGetAdvancesForGlyphs(ctFont, kCTFontOrientationHorizontal, glyphs, advances, CharacterCount);

    CGFloat maxAdvance = 0;
    for (NSUInteger i = 0; i < CharacterCount; i++)
        maxAdvance = MAX(maxAdvance, advances[i].width);

    if (maxAdvance <= 0)
        maxAdvance = [@"W" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    return maxAdvance;
}

@interface GhosttyTerminalDisplay ()
@property (nonatomic) int columns;
@property (nonatomic) int rows;
@property (nonatomic) CGSize characterSize;
@property (nonatomic) UIFont *regularFont;
@property (nonatomic) UIFont *boldFont;
@property (nonatomic) GhosttyColorRgb foregroundColor;
@property (nonatomic) GhosttyColorRgb backgroundColor;
@property (nonatomic) GhosttyColorRgb cursorColor;
@property (nonatomic) BOOL cursorColorSet;
@property (nonatomic) BOOL blinkCursor;
@property (nonatomic) NSString *cursorShape;
@end

@implementation GhosttyTerminalDisplay {
    GhosttyTerminal _terminal;
    GhosttyRenderState _renderState;
    GhosttyRenderStateRowIterator _rowIterator;
    GhosttyRenderStateRowCells _rowCells;
    BOOL _needsRenderStateUpdate;
    BOOL _updatingScrollView;
    BOOL _benchmarkEnabled;
    NSUInteger _benchmarkSamples;
    size_t _benchmarkBytes;
    CFTimeInterval _benchmarkVTTime;
    CFTimeInterval _benchmarkRenderTime;
    CFTimeInterval _benchmarkDrawTime;
    NSMutableArray<NSMutableArray<NSString *> *> *_visibleCells;
    BOOL _hasSelection;
    NSInteger _selectionStartOffset;
    NSInteger _selectionEndOffset;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.opaque = YES;
        self.contentMode = UIViewContentModeRedraw;
        self.layer.needsDisplayOnBoundsChange = YES;
        self.regularFont = TerminalFontForFamily(@"ui-monospace", 12, UIFontWeightRegular);
        self.boldFont = TerminalFontForFamily(@"ui-monospace", 12, UIFontWeightBold);
        self.foregroundColor = (GhosttyColorRgb) {.r = 255, .g = 255, .b = 255};
        self.backgroundColor = (GhosttyColorRgb) {.r = 0, .g = 0, .b = 0};
        self.cursorColor = self.foregroundColor;
        self.cursorShape = @"BLOCK";
        self.columns = DefaultColumns;
        self.rows = DefaultRows;
        _benchmarkEnabled = [NSProcessInfo.processInfo.environment[@"ISH_BENCH_TERMINAL_DISPLAY"] boolValue];
        _visibleCells = [NSMutableArray new];

        [self updateCharacterSize];

        GhosttyTerminalOptions options = {
            .cols = DefaultColumns,
            .rows = DefaultRows,
            .max_scrollback = MaxScrollbackBytes,
        };
        if (ghostty_terminal_new(NULL, &_terminal, options) != GHOSTTY_SUCCESS) {
            NSLog(@"failed to initialize libghostty-vt terminal");
            return self;
        }
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, (__bridge void *) self);
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *) GhosttyWritePty);
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &_foregroundColor);
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &_backgroundColor);

        ghostty_render_state_new(NULL, &_renderState);
        ghostty_render_state_row_iterator_new(NULL, &_rowIterator);
        ghostty_render_state_row_cells_new(NULL, &_rowCells);
        _needsRenderStateUpdate = YES;
    }
    return self;
}

- (id<CAAction>)actionForLayer:(CALayer *)layer forKey:(NSString *)event {
    if (layer == self.layer &&
        ([event isEqualToString:@"bounds"] ||
         [event isEqualToString:@"position"] ||
         [event isEqualToString:@"contents"]))
        return (id<CAAction>) NSNull.null;
    return [super actionForLayer:layer forKey:event];
}

- (void)dealloc {
    ghostty_render_state_row_cells_free(_rowCells);
    ghostty_render_state_row_iterator_free(_rowIterator);
    ghostty_render_state_free(_renderState);
    ghostty_terminal_free(_terminal);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self resizeTerminalToBounds];
}

- (void)updateCharacterSize {
    CGFloat scale = self.window.screen.scale ?: UIScreen.mainScreen.scale;
    CGFloat width = TerminalASCIIAdvance(self.regularFont) + CellWidthAdjustment;
    CGFloat height = self.regularFont.ascender - self.regularFont.descender + self.regularFont.leading;
    if (height <= 0)
        height = self.regularFont.lineHeight;
    self.characterSize = CGSizeMake(MAX(1, PixelRound(width, scale)),
                                    MAX(1, PixelRound(height, scale)));
}

- (void)resizeTerminalToBounds {
    if (_terminal == NULL || self.bounds.size.width <= 0 || self.bounds.size.height <= 0)
        return;

    [self updateCharacterSize];
    int columns = MAX(1, (int) floor(self.bounds.size.width / self.characterSize.width));
    int rows = MAX(1, (int) floor(self.bounds.size.height / self.characterSize.height));
    if (columns == self.columns && rows == self.rows)
        return;

    self.columns = columns;
    self.rows = rows;
    CGFloat scale = self.window.screen.scale ?: UIScreen.mainScreen.scale;
    ghostty_terminal_resize(_terminal, columns, rows,
                            (uint32_t) lrint(self.characterSize.width * scale),
                            (uint32_t) lrint(self.characterSize.height * scale));
    _needsRenderStateUpdate = YES;
    [self updateRenderStateIfNeeded];
    [self updateScrollbar];
    [self.delegate ghosttyTerminalDisplayDidResize:self columns:columns rows:rows];
    [self setNeedsDisplay];
}

- (void)writeBytes:(const void *)bytes length:(size_t)length completion:(void (^)(void))completion {
    if (!NSThread.isMainThread) {
        NSData *data = [NSData dataWithBytes:bytes length:length];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self writeBytes:data.bytes length:data.length completion:completion];
        });
        return;
    }
    if (_terminal == NULL) {
        if (completion)
            completion();
        return;
    }

    CFTimeInterval start = CACurrentMediaTime();
    ghostty_terminal_vt_write(_terminal, bytes, length);
    CFTimeInterval vtEnd = CACurrentMediaTime();
    _needsRenderStateUpdate = YES;
    [self updateRenderStateIfNeeded];
    CFTimeInterval renderEnd = CACurrentMediaTime();
    [self updateScrollbar];
    [self setNeedsDisplay];
    if (_benchmarkEnabled)
        [self addBenchmarkBytes:length vtTime:vtEnd - start renderTime:renderEnd - vtEnd drawTime:0];
    if (completion)
        completion();
}

- (void)updateRenderStateIfNeeded {
    if (!_needsRenderStateUpdate || _renderState == NULL || _terminal == NULL)
        return;
    _needsRenderStateUpdate = NO;
    ghostty_render_state_update(_renderState, _terminal);
    [self updateVisibleCells];
}

- (void)updateVisibleCells {
    [_visibleCells removeAllObjects];
    for (int row = 0; row < self.rows; row++) {
        NSMutableArray<NSString *> *cells = [NSMutableArray arrayWithCapacity:self.columns];
        for (int column = 0; column < self.columns; column++)
            [cells addObject:@" "];
        [_visibleCells addObject:cells];
    }

    if (_renderState == NULL || _rowIterator == NULL || _rowCells == NULL)
        return;
    if (ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &_rowIterator) != GHOSTTY_SUCCESS)
        return;

    int row = 0;
    while (row < self.rows && ghostty_render_state_row_iterator_next(_rowIterator)) {
        if (ghostty_render_state_row_get(_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &_rowCells) != GHOSTTY_SUCCESS) {
            row++;
            continue;
        }
        int column = 0;
        while (column < self.columns && ghostty_render_state_row_cells_next(_rowCells)) {
            uint32_t graphemeLength = 0;
            ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemeLength);
            if (graphemeLength > 0) {
                uint32_t stackGraphemes[8] = {};
                NSMutableData *heapGraphemes = nil;
                void *graphemeBytes = stackGraphemes;
                size_t graphemeBytesLength = graphemeLength * sizeof(uint32_t);
                if (graphemeLength > sizeof(stackGraphemes) / sizeof(stackGraphemes[0])) {
                    heapGraphemes = [NSMutableData dataWithLength:graphemeBytesLength];
                    graphemeBytes = heapGraphemes.mutableBytes;
                }
                ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, graphemeBytes);
                NSString *text = [[NSString alloc] initWithBytes:graphemeBytes
                                                          length:graphemeBytesLength
                                                        encoding:NSUTF32LittleEndianStringEncoding];
                if (text.length > 0)
                    _visibleCells[row][column] = text;
            }
            column++;
        }
        row++;
    }
}

- (void)updateFontFamily:(NSString *)fontFamily
                fontSize:(CGFloat)fontSize
         foregroundColor:(NSString *)foregroundColor
         backgroundColor:(NSString *)backgroundColor
             cursorColor:(NSString *)cursorColor
   colorPaletteOverrides:(NSArray<NSString *> *)colorPaletteOverrides
             blinkCursor:(BOOL)blinkCursor
             cursorShape:(NSString *)cursorShape {
    self.regularFont = TerminalFontForFamily(fontFamily, fontSize, UIFontWeightRegular);
    self.boldFont = TerminalFontForFamily(fontFamily, fontSize, UIFontWeightBold);
    self.foregroundColor = GhosttyColorFromHex(foregroundColor, self.foregroundColor);
    self.backgroundColor = GhosttyColorFromHex(backgroundColor, self.backgroundColor);
    if (cursorColor != nil) {
        self.cursorColor = GhosttyColorFromHex(cursorColor, self.foregroundColor);
        self.cursorColorSet = YES;
    } else {
        self.cursorColorSet = NO;
    }
    self.blinkCursor = blinkCursor;
    self.cursorShape = cursorShape;

    if (_terminal != NULL) {
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &_foregroundColor);
        ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &_backgroundColor);
        if (_cursorColorSet)
            ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &_cursorColor);
        else
            ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, NULL);

        if (colorPaletteOverrides != nil) {
            GhosttyColorRgb palette[256];
            if (ghostty_terminal_get(_terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, &palette) == GHOSTTY_SUCCESS) {
                NSUInteger count = MIN(colorPaletteOverrides.count, 256);
                for (NSUInteger i = 0; i < count; i++)
                    palette[i] = GhosttyColorFromHex(colorPaletteOverrides[i], palette[i]);
                ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
            }
        } else {
            ghostty_terminal_set(_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, NULL);
        }
    }

    [self resizeTerminalToBounds];
    _needsRenderStateUpdate = YES;
    [self setNeedsDisplay];
}

- (void)setTerminalFocused:(BOOL)terminalFocused {
    _terminalFocused = terminalFocused;
    [self setNeedsDisplay];
}

- (void)scrollToBottom {
    GhosttyTerminalScrollViewport scroll = {
        .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM,
    };
    ghostty_terminal_scroll_viewport(_terminal, scroll);
    _needsRenderStateUpdate = YES;
    [self updateRenderStateIfNeeded];
    [self updateScrollbar];
    [self setNeedsDisplay];
}

- (void)scrollToRowOffset:(NSUInteger)rowOffset {
    if (_updatingScrollView)
        return;
    GhosttyTerminalScrollbar scrollbar = {};
    if (ghostty_terminal_get(_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar) != GHOSTTY_SUCCESS)
        return;
    intptr_t delta = (intptr_t) rowOffset - (intptr_t) scrollbar.offset;
    if (delta == 0)
        return;
    GhosttyTerminalScrollViewport scroll = {
        .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
        .value = {.delta = delta},
    };
    ghostty_terminal_scroll_viewport(_terminal, scroll);
    _needsRenderStateUpdate = YES;
    [self updateRenderStateIfNeeded];
    [self updateScrollbar];
    [self setNeedsDisplay];
}

- (void)clearScrollback {
    static const uint8_t clear[] = "\033[3J";
    [self writeBytes:clear length:sizeof(clear) - 1 completion:^{}];
}

- (BOOL)applicationCursorMode {
    bool enabled = false;
    if (_terminal != NULL)
        ghostty_terminal_mode_get(_terminal, GHOSTTY_MODE_DECCKM, &enabled);
    return enabled;
}

- (void)copyScreenToPasteboard {
    if (_terminal == NULL)
        return;
    GhosttyFormatter formatter = NULL;
    GhosttyFormatterTerminalOptions options = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.trim = true;
    if (ghostty_formatter_terminal_new(NULL, &formatter, _terminal, options) != GHOSTTY_SUCCESS)
        return;
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (ghostty_formatter_format_alloc(formatter, NULL, &bytes, &length) == GHOSTTY_SUCCESS) {
        UIPasteboard.generalPasteboard.string = [[NSString alloc] initWithBytes:bytes length:length encoding:NSUTF8StringEncoding];
        ghostty_free(NULL, bytes, length);
    }
    ghostty_formatter_free(formatter);
}

- (BOOL)hasSelection {
    return _hasSelection;
}

- (NSInteger)cellCount {
    return MAX(0, self.rows) * MAX(0, self.columns);
}

- (UIFont *)selectionFont {
    return self.regularFont;
}

- (void)beginSelectionAtPoint:(CGPoint)point {
    NSInteger offset = [self cellOffsetAtPoint:point];
    [self setSelectionFromCellOffset:offset toCellOffset:MIN(offset + 1, self.cellCount)];
}

- (void)updateSelectionAtPoint:(CGPoint)point {
    NSInteger offset = [self cellOffsetAtPoint:point];
    [self setSelectionFromCellOffset:_selectionStartOffset toCellOffset:MIN(offset + 1, self.cellCount)];
}

- (NSInteger)cellOffsetAtPoint:(CGPoint)point {
    if (self.columns <= 0 || self.rows <= 0)
        return 0;

    NSInteger column = (NSInteger) floor(point.x / MAX(1, self.characterSize.width));
    NSInteger row = (NSInteger) floor(point.y / MAX(1, self.characterSize.height));
    column = MAX(0, MIN((NSInteger) self.columns, column));
    row = MAX(0, MIN((NSInteger) self.rows - 1, row));
    return MAX(0, MIN(self.cellCount, row * self.columns + column));
}

- (void)setSelectionFromCellOffset:(NSInteger)startOffset toCellOffset:(NSInteger)endOffset {
    NSInteger cellCount = self.cellCount;
    _selectionStartOffset = MAX(0, MIN(cellCount, startOffset));
    _selectionEndOffset = MAX(0, MIN(cellCount, endOffset));
    _hasSelection = _selectionStartOffset != _selectionEndOffset;
    [self setNeedsDisplay];
}

- (void)clearSelection {
    if (!_hasSelection)
        return;
    _hasSelection = NO;
    _selectionStartOffset = 0;
    _selectionEndOffset = 0;
    [self setNeedsDisplay];
}

- (void)copySelectionToPasteboard {
    NSString *selection = [self textInCellRange:NSMakeRange(MIN(_selectionStartOffset, _selectionEndOffset),
                                                            labs(_selectionEndOffset - _selectionStartOffset))];
    if (selection.length > 0)
        UIPasteboard.generalPasteboard.string = selection;
}

- (NSString *)textInCellRange:(NSRange)range {
    if (_visibleCells.count == 0 || range.length == 0 || self.columns <= 0)
        return @"";

    NSInteger startOffset = MAX(0, MIN(self.cellCount, (NSInteger) range.location));
    NSInteger endOffset = MAX(startOffset, MIN(self.cellCount, (NSInteger) NSMaxRange(range)));
    NSMutableArray<NSString *> *lines = [NSMutableArray new];
    for (NSInteger row = startOffset / self.columns; row <= (endOffset - 1) / self.columns && row < (NSInteger) _visibleCells.count; row++) {
        NSArray<NSString *> *cells = _visibleCells[row];
        NSInteger firstColumn = row == startOffset / self.columns ? startOffset % self.columns : 0;
        NSInteger lastColumnExclusive = row == (endOffset - 1) / self.columns ? ((endOffset - 1) % self.columns) + 1 : (NSInteger) cells.count;
        NSMutableString *line = [NSMutableString new];
        for (NSInteger column = firstColumn; column < lastColumnExclusive && column < (NSInteger) cells.count; column++)
            [line appendString:cells[column]];
        while ([line hasSuffix:@" "])
            [line deleteCharactersInRange:NSMakeRange(line.length - 1, 1)];
        [lines addObject:line];
    }
    return [lines componentsJoinedByString:@"\n"];
}

- (CGRect)selectionBoundingRect {
    if (!_hasSelection)
        return CGRectZero;
    return [self firstRectForCellRange:NSMakeRange(MIN(_selectionStartOffset, _selectionEndOffset),
                                                   labs(_selectionEndOffset - _selectionStartOffset))];
}

- (CGRect)firstRectForCellRange:(NSRange)range {
    return [self rectsForCellRange:range].firstObject.CGRectValue;
}

- (NSArray<NSValue *> *)rectsForCellRange:(NSRange)range {
    if (range.length == 0 || self.columns <= 0 || self.rows <= 0)
        return @[];

    NSInteger startOffset = MAX(0, MIN(self.cellCount, (NSInteger) range.location));
    NSInteger endOffset = MAX(startOffset, MIN(self.cellCount, (NSInteger) NSMaxRange(range)));
    if (startOffset == endOffset)
        return @[];

    NSMutableArray<NSValue *> *rects = [NSMutableArray new];
    NSInteger firstRow = startOffset / self.columns;
    NSInteger lastRow = (endOffset - 1) / self.columns;
    for (NSInteger row = firstRow; row <= lastRow; row++) {
        NSInteger firstColumn = row == firstRow ? startOffset % self.columns : 0;
        NSInteger lastColumnExclusive = row == lastRow ? ((endOffset - 1) % self.columns) + 1 : self.columns;
        CGRect rect = CGRectMake(firstColumn * self.characterSize.width,
                                 row * self.characterSize.height,
                                 MAX(1, lastColumnExclusive - firstColumn) * self.characterSize.width,
                                 self.characterSize.height);
        [rects addObject:[NSValue valueWithCGRect:rect]];
    }
    return rects;
}

- (NSString *)visibleText {
    if (_visibleCells.count == 0)
        return @"";
    NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithCapacity:_visibleCells.count];
    for (NSArray<NSString *> *cells in _visibleCells) {
        NSMutableString *line = [NSMutableString new];
        for (NSString *cell in cells)
            [line appendString:cell];
        while ([line hasSuffix:@" "])
            [line deleteCharactersInRange:NSMakeRange(line.length - 1, 1)];
        [lines addObject:line];
    }
    return [lines componentsJoinedByString:@"\n"];
}

- (BOOL)isCellSelectedAtRow:(NSInteger)row column:(NSInteger)column {
    if (!_hasSelection)
        return NO;

    NSInteger offset = row * self.columns + column;
    NSInteger startOffset = MIN(_selectionStartOffset, _selectionEndOffset);
    NSInteger endOffset = MAX(_selectionStartOffset, _selectionEndOffset);
    return offset >= startOffset && offset < endOffset;
}

- (void)updateScrollbar {
    GhosttyTerminalScrollbar scrollbar = {};
    if (_terminal == NULL || ghostty_terminal_get(_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar) != GHOSTTY_SUCCESS)
        return;
    _updatingScrollView = YES;
    [self.delegate ghosttyTerminalDisplayDidUpdateScrollback:self
                                                   totalRows:(NSUInteger) scrollbar.total
                                                      offset:(NSUInteger) scrollbar.offset
                                                 visibleRows:(NSUInteger) scrollbar.len];
    _updatingScrollView = NO;
}

- (void)drawRect:(CGRect)rect {
    (void) rect;
    CFTimeInterval start = CACurrentMediaTime();
    CGContextRef context = UIGraphicsGetCurrentContext();
    [UIColorFromGhosttyColor(self.backgroundColor) setFill];
    CGContextFillRect(context, self.bounds);

    [self updateRenderStateIfNeeded];
    if (_renderState == NULL)
        return;

    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_colors_get(_renderState, &colors) != GHOSTTY_SUCCESS) {
        colors.foreground = self.foregroundColor;
        colors.background = self.backgroundColor;
    }

    if (ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &_rowIterator) != GHOSTTY_SUCCESS)
        return;

    int row = 0;
    while (ghostty_render_state_row_iterator_next(_rowIterator)) {
        if (ghostty_render_state_row_get(_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &_rowCells) != GHOSTTY_SUCCESS) {
            row++;
            continue;
        }
        int column = 0;
        while (ghostty_render_state_row_cells_next(_rowCells)) {
            CGRect cellRect = CGRectMake(column * self.characterSize.width,
                                         row * self.characterSize.height,
                                         self.characterSize.width,
                                         self.characterSize.height);
            GhosttyColorRgb bg;
            if (ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS) {
                [UIColorFromGhosttyColor(bg) setFill];
                CGContextFillRect(context, cellRect);
            }
            if ([self isCellSelectedAtRow:row column:column]) {
                [[UIColor systemBlueColor] setFill];
                CGContextSetAlpha(context, 0.45);
                CGContextFillRect(context, cellRect);
                CGContextSetAlpha(context, 1);
            }

            uint32_t graphemeLength = 0;
            ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemeLength);
            if (graphemeLength > 0) {
                uint32_t stackGraphemes[8] = {};
                NSMutableData *heapGraphemes = nil;
                void *graphemeBytes = stackGraphemes;
                size_t graphemeBytesLength = graphemeLength * sizeof(uint32_t);
                if (graphemeLength > sizeof(stackGraphemes) / sizeof(stackGraphemes[0])) {
                    heapGraphemes = [NSMutableData dataWithLength:graphemeBytesLength];
                    graphemeBytes = heapGraphemes.mutableBytes;
                }
                ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, graphemeBytes);
                NSString *text = [[NSString alloc] initWithBytes:graphemeBytes
                                                          length:graphemeBytesLength
                                                        encoding:NSUTF32LittleEndianStringEncoding];
                if (text.length > 0) {
                    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                    ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
                    GhosttyColorRgb fg = colors.foreground;
                    ghostty_render_state_row_cells_get(_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
                    NSDictionary *attributes = @{
                        NSFontAttributeName: style.bold ? self.boldFont : self.regularFont,
                        NSForegroundColorAttributeName: UIColorFromGhosttyColor(style.inverse ? colors.background : fg),
                    };
                    [text drawAtPoint:cellRect.origin withAttributes:attributes];
                }
            }
            column++;
        }
        row++;
    }

    [self drawCursorInContext:context colors:&colors];

    GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(_renderState, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
    if (_benchmarkEnabled)
        [self addBenchmarkBytes:0 vtTime:0 renderTime:0 drawTime:CACurrentMediaTime() - start];
}

- (void)drawCursorInContext:(CGContextRef)context colors:(GhosttyRenderStateColors *)colors {
    if (!self.terminalFocused)
        return;
    bool visible = false;
    bool inViewport = false;
    ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
    ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &inViewport);
    if (!visible || !inViewport)
        return;

    uint16_t x = 0;
    uint16_t y = 0;
    ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &x);
    ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &y);
    CGRect cursor = CGRectMake(x * self.characterSize.width, y * self.characterSize.height,
                               self.characterSize.width, self.characterSize.height);
    GhosttyRenderStateCursorVisualStyle style = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    ghostty_render_state_get(_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &style);
    UIColor *cursorColor = UIColorFromGhosttyColor(self.cursorColorSet ? self.cursorColor : colors->foreground);
    [cursorColor setStroke];
    [cursorColor setFill];
    if ([self.cursorShape isEqualToString:@"BEAM"] || style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) {
        CGContextFillRect(context, CGRectMake(cursor.origin.x, cursor.origin.y, 2, cursor.size.height));
    } else if ([self.cursorShape isEqualToString:@"UNDERLINE"] || style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE) {
        CGContextFillRect(context, CGRectMake(cursor.origin.x, CGRectGetMaxY(cursor) - 2, cursor.size.width, 2));
    } else if (style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW) {
        CGContextStrokeRectWithWidth(context, cursor, 1);
    } else {
        CGContextFillRect(context, cursor);
    }
}

- (void)addBenchmarkBytes:(size_t)bytes vtTime:(CFTimeInterval)vtTime renderTime:(CFTimeInterval)renderTime drawTime:(CFTimeInterval)drawTime {
    _benchmarkSamples++;
    _benchmarkBytes += bytes;
    _benchmarkVTTime += vtTime;
    _benchmarkRenderTime += renderTime;
    _benchmarkDrawTime += drawTime;
    if (_benchmarkSamples < 120)
        return;
    NSLog(@"terminal-display-bench samples=%lu bytes=%zu vt_ms=%.3f render_ms=%.3f draw_ms=%.3f",
          (unsigned long) _benchmarkSamples,
          _benchmarkBytes,
          _benchmarkVTTime * 1000.0 / _benchmarkSamples,
          _benchmarkRenderTime * 1000.0 / _benchmarkSamples,
          _benchmarkDrawTime * 1000.0 / _benchmarkSamples);
    _benchmarkSamples = 0;
    _benchmarkBytes = 0;
    _benchmarkVTTime = 0;
    _benchmarkRenderTime = 0;
    _benchmarkDrawTime = 0;
}

@end
