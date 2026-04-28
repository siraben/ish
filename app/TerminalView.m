//
//  TerminalView.m
//  iSH
//
//  Created by Theodore Dubois on 11/3/17.
//

#import "ScrollbarView.h"
#import "TerminalView.h"
#import "UserPreferences.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"

struct rowcol {
    int row;
    int col;
};

@interface TerminalTextPosition : UITextPosition
@property (nonatomic) NSInteger offset;
+ (instancetype)positionWithOffset:(NSInteger)offset;
@end

@implementation TerminalTextPosition
+ (instancetype)positionWithOffset:(NSInteger)offset {
    TerminalTextPosition *position = [TerminalTextPosition new];
    position.offset = offset;
    return position;
}
@end

@interface TerminalTextRange : UITextRange
@property (nonatomic, strong) TerminalTextPosition *terminalStart;
@property (nonatomic, strong) TerminalTextPosition *terminalEnd;
+ (instancetype)rangeWithStart:(NSInteger)start end:(NSInteger)end;
@end

@implementation TerminalTextRange
+ (instancetype)rangeWithStart:(NSInteger)start end:(NSInteger)end {
    TerminalTextRange *range = [TerminalTextRange new];
    range.terminalStart = [TerminalTextPosition positionWithOffset:MIN(start, end)];
    range.terminalEnd = [TerminalTextPosition positionWithOffset:MAX(start, end)];
    return range;
}
- (UITextPosition *)start { return self.terminalStart; }
- (UITextPosition *)end { return self.terminalEnd; }
- (BOOL)isEmpty { return self.terminalStart.offset == self.terminalEnd.offset; }
@end

@interface TerminalSelectionRect : UITextSelectionRect
@property (nonatomic) CGRect terminalRect;
@property (nonatomic, strong) UITextRange *terminalRange;
@property (nonatomic) BOOL terminalContainsStart;
@property (nonatomic) BOOL terminalContainsEnd;
@end

@implementation TerminalSelectionRect
- (CGRect)rect { return self.terminalRect; }
- (UITextRange *)range { return self.terminalRange; }
- (UITextWritingDirection)writingDirection { return UITextWritingDirectionLeftToRight; }
- (BOOL)containsStart { return self.terminalContainsStart; }
- (BOOL)containsEnd { return self.terminalContainsEnd; }
- (BOOL)isVertical { return NO; }
@end

@interface TerminalView ()

@property (nonatomic) NSMutableArray<UIKeyCommand *> *keyCommands;
@property ScrollbarView *scrollbarView;
@property (nonatomic) BOOL terminalFocused;

@property (nullable, nonatomic, copy) NSString *markedText;
@property (nullable, nonatomic, copy) NSString *selectedText;
@property (nonatomic, strong) UITextRange *markedRange;
@property (nonatomic, strong) UITextRange *selectedRange;

@property struct rowcol floatingCursor;
@property CGSize floatingCursorSensitivity;
@property CGSize actualFloatingCursorSensitivity;
@property BOOL updatingScrollOffsetFromTerminal;
@property UITapGestureRecognizer *focusTapGesture;
@property (nonatomic) id<UIInteraction> textInteraction;

@end

@implementation TerminalView
@synthesize inputDelegate;
@synthesize tokenizer;
@synthesize canBecomeFirstResponder;

- (void)awakeFromNib {
    [super awakeFromNib];
    self.inputAssistantItem.leadingBarButtonGroups = @[];
    self.inputAssistantItem.trailingBarButtonGroups = @[];

    ScrollbarView *scrollbarView = self.scrollbarView = [[ScrollbarView alloc] initWithFrame:self.bounds];
    scrollbarView.delegate = self;
    scrollbarView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    scrollbarView.bounces = NO;
    [self addSubview:scrollbarView];

    UserPreferences *prefs = UserPreferences.shared;
    [prefs observe:@[@"capsLockMapping", @"optionMapping", @"backtickMapEscape", @"overrideControlSpace"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_keyCommands = nil;
        });
    }];
    [prefs observe:@[@"colorScheme", @"fontFamily", @"fontSize", @"theme", @"cursorStyle", @"blinkCursor"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyle];
        });
    }];

    tokenizer = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
    self.markedRange = [TerminalTextRange rangeWithStart:0 end:0];
    self.selectedRange = [TerminalTextRange rangeWithStart:0 end:0];

    self.focusTapGesture = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(focusTerminal:)];
    self.focusTapGesture.cancelsTouchesInView = NO;
    [self addGestureRecognizer:self.focusTapGesture];

    if (@available(iOS 13.0, *)) {
        self.textInteraction = [UITextInteraction textInteractionForMode:UITextInteractionModeEditable];
        [self addInteraction:self.textInteraction];
    }
}

- (void)dealloc {
    self.terminal = nil;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context {
    if (object == _terminal) {
        if (_terminal.loaded) {
            [self installTerminalView];
            [self _updateStyle];
        }
    }
}

- (void)setTerminal:(Terminal *)terminal {
    if (_terminal) {
        [_terminal removeObserver:self forKeyPath:@"loaded"];
        [self uninstallTerminalView];
    }

    _terminal = terminal;
    [_terminal addObserver:self forKeyPath:@"loaded" options:NSKeyValueObservingOptionInitial context:nil];
    if (_terminal.loaded)
        [self installTerminalView];
}

- (void)installTerminalView {
    NSAssert(_terminal.loaded, @"should probably not be installing a non-loaded terminal");
    UIView *superview = self.terminal.displayView.superview;
    if (superview != nil) {
        NSAssert(superview == self.scrollbarView, @"installing terminal that is already installed elsewhere");
        return;
    }

    GhosttyTerminalDisplay *displayView = _terminal.displayView;
    displayView.delegate = self;
    _terminal.enableVoiceOverAnnounce = YES;
    displayView.frame = self.bounds;
    self.opaque = displayView.opaque = YES;
    displayView.backgroundColor = UIColor.clearColor;
    displayView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    self.scrollbarView.contentView = displayView;
    [self.scrollbarView addSubview:displayView];
}

- (void)uninstallTerminalView {
    // remove old terminal
    UIView *superview = _terminal.displayView.superview;
    if (superview != self.scrollbarView) {
        NSAssert(superview == nil, @"uninstalling terminal that is installed elsewhere");
        return;
    }

    [_terminal.displayView removeFromSuperview];
    self.scrollbarView.contentView = nil;
    _terminal.enableVoiceOverAnnounce = NO;
    _terminal.displayView.delegate = _terminal;
}

#pragma mark Styling

- (void)_updateStyle {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    if (!self.terminal.loaded)
        return;
    UserPreferences *prefs = [UserPreferences shared];
    if (_overrideFontSize == prefs.fontSize.doubleValue)
        _overrideFontSize = 0;
    Palette *palette = prefs.palette;
    if (self.overrideAppearance != OverrideAppearanceNone) {
        palette = self.overrideAppearance == OverrideAppearanceLight ? prefs.theme.lightPalette : prefs.theme.darkPalette;
    }
    [self.terminal.displayView updateFontFamily:prefs.fontFamily
                                       fontSize:self.effectiveFontSize
                                foregroundColor:palette.foregroundColor
                                backgroundColor:palette.backgroundColor
                                    cursorColor:palette.cursorColor
                          colorPaletteOverrides:palette.colorPaletteOverrides
                                    blinkCursor:prefs.blinkCursor
                                    cursorShape:prefs.htermCursorShape];
    [self updateFloatingCursorSensitivity];
}

- (void)setOverrideFontSize:(CGFloat)overrideFontSize {
    _overrideFontSize = overrideFontSize;
    [self _updateStyle];
}

- (void)setOverrideAppearance:(enum OverrideAppearance)overrideAppearance {
    _overrideAppearance = overrideAppearance;
    [self _updateStyle];
}

- (CGFloat)effectiveFontSize {
    if (self.overrideFontSize != 0)
        return self.overrideFontSize;
    return UserPreferences.shared.fontSize.doubleValue;
}

#pragma mark Focus and scrolling

- (void)setTerminalFocused:(BOOL)terminalFocused {
    _terminalFocused = terminalFocused;
    self.terminal.displayView.terminalFocused = terminalFocused;
}

- (BOOL)becomeFirstResponder {
    self.terminalFocused = YES;
    [self reloadInputViews];
    return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
    self.terminalFocused = NO;
    return [super resignFirstResponder];
}
- (void)windowDidBecomeKey:(NSNotification *)notif {
    self.terminalFocused = YES;
}
- (void)windowDidResignKey:(NSNotification *)notif {
    self.terminalFocused = NO;
}

- (IBAction)loseFocus:(id)sender {
    [self resignFirstResponder];
}

- (void)focusTerminal:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateEnded)
        return;
    if (self.terminal.displayView.hasSelection) {
        self.selectedTextRange = [TerminalTextRange rangeWithStart:0 end:0];
        return;
    }
    [self becomeFirstResponder];
}

- (void)willMoveToWindow:(UIWindow *)newWindow {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    if (self.window != nil) {
        [center removeObserver:self
                          name:UIWindowDidBecomeKeyNotification
                        object:self.window];
        [center removeObserver:self
                          name:UIWindowDidResignKeyNotification
                        object:self.window];
    }
    if (newWindow != nil) {
        [center addObserver:self
                   selector:@selector(windowDidBecomeKey:)
                       name:UIWindowDidBecomeKeyNotification
                     object:newWindow];
        [center addObserver:self
                   selector:@selector(windowDidResignKey:)
                       name:UIWindowDidResignKeyNotification
                     object:newWindow];
    }
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    if (self.updatingScrollOffsetFromTerminal)
        return;
    CGFloat row = scrollView.contentOffset.y / MAX(1, self.terminal.displayView.characterSize.height);
    [self.terminal.displayView scrollToRowOffset:(NSUInteger) llround(row)];
}

- (void)ghosttyTerminalDisplayDidResize:(GhosttyTerminalDisplay *)display columns:(int)columns rows:(int)rows {
    [self.terminal ghosttyTerminalDisplayDidResize:display columns:columns rows:rows];
    [self updateFloatingCursorSensitivity];
}

- (void)ghosttyTerminalDisplay:(GhosttyTerminalDisplay *)display writePtyBytes:(const uint8_t *)bytes length:(size_t)length {
    [self.terminal ghosttyTerminalDisplay:display writePtyBytes:bytes length:length];
}

- (void)ghosttyTerminalDisplayDidUpdateScrollback:(GhosttyTerminalDisplay *)display totalRows:(NSUInteger)totalRows offset:(NSUInteger)offset visibleRows:(NSUInteger)visibleRows {
    (void) display;
    CGFloat rowHeight = MAX(1, self.terminal.displayView.characterSize.height);
    CGSize contentSize = CGSizeMake(0, MAX(self.scrollbarView.bounds.size.height, totalRows * rowHeight));
    CGPoint contentOffset = CGPointMake(0, offset * rowHeight);
    self.updatingScrollOffsetFromTerminal = YES;
    self.scrollbarView.contentSize = contentSize;
    if (fabs(self.scrollbarView.contentOffset.y - contentOffset.y) >= 1)
        [self.scrollbarView setContentOffset:contentOffset animated:NO];
    self.updatingScrollOffsetFromTerminal = NO;
    (void) visibleRows;
}

- (void)setKeyboardAppearance:(UIKeyboardAppearance)keyboardAppearance {
    BOOL needsFirstResponderDance = self.isFirstResponder && _keyboardAppearance != keyboardAppearance;
    if (needsFirstResponderDance) {
        [self resignFirstResponder];
    }
    _keyboardAppearance = keyboardAppearance;
    if (needsFirstResponderDance) {
        [self becomeFirstResponder];
    }
    if (keyboardAppearance == UIKeyboardAppearanceLight) {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleBlack;
    } else {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleWhite;
    }
}

#pragma mark Keyboard Input

// implementing these makes a keyboard pop up when this view is first responder

- (void)insertText:(NSString *)text {
    self.markedText = nil;
    if (self.terminal.displayView.hasSelection)
        self.selectedTextRange = [TerminalTextRange rangeWithStart:0 end:0];

    if (self.controlKey.highlighted)
        self.controlKey.selected = YES;
    if (self.controlKey.selected) {
        if (!self.controlKey.highlighted)
            self.controlKey.selected = NO;
        if (text.length == 1)
            return [self insertControlChar:[text characterAtIndex:0]];
    }

    text = [text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    [self.terminal sendInput:data];
}

- (void)insertControlChar:(char)ch {
    if (strchr(controlKeys, ch) != NULL) {
        if (ch == ' ') ch = '\0';
        if (ch == '2') ch = '@';
        if (ch == '6') ch = '^';
        if (ch != '\0')
            ch = toupper(ch) ^ 0x40;
        [self.terminal sendInput:[NSData dataWithBytes:&ch length:1]];
    }
}

- (void)deleteBackward {
    [self insertText:@"\x7f"];
}

- (BOOL)hasText {
    return YES; // it's always ok to send a "delete"
}

#pragma mark IME Input and Selection

- (void)setMarkedText:(nullable NSString *)markedText selectedRange:(NSRange)selectedRange {
    self.markedText = markedText;
}

- (void)unmarkText {
    [self insertText:self.markedText];
}

- (UITextRange *)markedTextRange {
    if (self.markedText != nil)
        return self.markedRange;
    return nil;
}

// The only reason to have this selected range is to prevent the "speak selection" context action from failing to get the current selection and falling back on calling copy:. It doesn't even have to work, it seems...

- (UITextRange *)selectedTextRange {
    return self.selectedRange;
}

- (NSString *)textInRange:(UITextRange *)range {
    if (range == self.markedRange)
        return self.markedText;
    return [self.terminal.displayView textInCellRange:[self cellRangeFromTextRange:range]];
}

- (id)insertDictationResultPlaceholder {
    return @"";
}
- (void)removeDictationResultPlaceholder:(id)placeholder willInsertResult:(BOOL)willInsertResult {
}

#pragma mark Keyboard Actions

- (void)paste:(id)sender {
    NSString *string = UIPasteboard.generalPasteboard.string;
    if (string) {
        [self insertText:string];
    }
}

- (void)copy:(id)sender {
    NSString *selection = [self textInRange:self.selectedTextRange];
    if (selection.length > 0)
        UIPasteboard.generalPasteboard.string = selection;
    else
        [self.terminal.displayView copyScreenToPasteboard];
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(copy:))
        return self.terminal.displayView.hasSelection;
    if (action == @selector(paste:))
        return UIPasteboard.generalPasteboard.string != nil;
    return [super canPerformAction:action withSender:sender];
}

- (void)clearScrollback:(UIKeyCommand *)command {
    [self.terminal.displayView clearScrollback];
}

#pragma mark Floating cursor

- (void)updateFloatingCursorSensitivity {
    CGSize charSize = self.terminal.displayView.characterSize;
    double sensitivity = 0.5;
    self.floatingCursorSensitivity = CGSizeMake(charSize.width / sensitivity, charSize.height / sensitivity);
}

- (struct rowcol)rowcolFromPoint:(CGPoint)point {
    CGSize sensitivity = self.actualFloatingCursorSensitivity;
    return (struct rowcol) {
        .row = (int) (-point.y / sensitivity.height),
        .col = (int) (point.x / sensitivity.width),
    };
}

- (void)beginFloatingCursorAtPoint:(CGPoint)point {
    self.actualFloatingCursorSensitivity = self.floatingCursorSensitivity;
    self.floatingCursor = [self rowcolFromPoint:point];
}

- (void)updateFloatingCursorAtPoint:(CGPoint)point {
    struct rowcol newPos = [self rowcolFromPoint:point];
    int rowDiff = newPos.row - self.floatingCursor.row;
    int colDiff = newPos.col - self.floatingCursor.col;
    NSMutableString *arrows = [NSMutableString string];
    for (int i = 0; i < abs(rowDiff); i++) {
        [arrows appendString:[self.terminal arrow:rowDiff > 0 ? 'A': 'B']];
    }
    for (int i = 0; i < abs(colDiff); i++) {
        [arrows appendString:[self.terminal arrow:colDiff > 0 ? 'C': 'D']];
    }
    [self insertText:arrows];
    self.floatingCursor = newPos;
}

- (void)endFloatingCursor {
    self.floatingCursor = (struct rowcol) {};
}

#pragma mark Keyboard Traits

- (UITextSmartDashesType)smartDashesType API_AVAILABLE(ios(11)) {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType API_AVAILABLE(ios(11)) {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType API_AVAILABLE(ios(11)) {
    return UITextSmartInsertDeleteTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
// Apparently required on iOS 15+: https://stackoverflow.com/a/72359764
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}

#pragma mark Hardware Keyboard

- (void)handleKeyCommand:(UIKeyCommand *)command {
    NSString *key = command.input;
    if (command.modifierFlags == 0) {
        if ([key isEqualToString:@"`"] && UserPreferences.shared.backtickMapEscape)
            key = UIKeyInputEscape;
        if ([key isEqualToString:UIKeyInputEscape])
            key = @"\x1b";
        else if ([key isEqualToString:UIKeyInputUpArrow])
            key = [self.terminal arrow:'A'];
        else if ([key isEqualToString:UIKeyInputDownArrow])
            key = [self.terminal arrow:'B'];
        else if ([key isEqualToString:UIKeyInputLeftArrow])
            key = [self.terminal arrow:'D'];
        else if ([key isEqualToString:UIKeyInputRightArrow])
            key = [self.terminal arrow:'C'];
        [self insertText:key];
    } else if (command.modifierFlags & UIKeyModifierShift) {
        [self insertText:[key uppercaseString]];
    } else if (command.modifierFlags & UIKeyModifierAlternate) {
        [self insertText:[@"\x1b" stringByAppendingString:key]];
    } else if (command.modifierFlags & UIKeyModifierAlphaShift) {
        [self handleCapsLockWithCommand:command];
    } else if (command.modifierFlags & UIKeyModifierControl || command.modifierFlags & UIKeyModifierAlphaShift) {
        if (key.length == 0)
            return;
        if ([key isEqualToString:@"2"])
            key = @"@";
        else if ([key isEqualToString:@"6"])
            key = @"^";
        else if ([key isEqualToString:@"-"])
            key = @"_";
        [self insertControlChar:[key characterAtIndex:0]];
    }
}

static const char *alphabet = "abcdefghijklmnopqrstuvwxyz";
static const char *controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-=[]\\ ";
static const char *metaKeys = "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./";

- (NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addKeys:controlKeys withModifiers:UIKeyModifierControl];
    for (NSString *specialKey in @[UIKeyInputEscape, UIKeyInputUpArrow, UIKeyInputDownArrow,
                                   UIKeyInputLeftArrow, UIKeyInputRightArrow, @"\t"]) {
        [self addKey:specialKey withModifiers:0];
    }
    if (UserPreferences.shared.capsLockMapping != CapsLockMapNone) {
        if (@available(iOS 13, *)); else {
            [self addKeys:controlKeys withModifiers:UIKeyModifierAlphaShift];
            [self addKeys:alphabet withModifiers:0];
            [self addKeys:alphabet withModifiers:UIKeyModifierShift];
            [self addKey:@"" withModifiers:UIKeyModifierAlphaShift]; // otherwise tap of caps lock can switch layouts
        }
    }
    if (UserPreferences.shared.optionMapping == OptionMapEsc) {
        [self addKeys:metaKeys withModifiers:UIKeyModifierAlternate];
    }
    if (UserPreferences.shared.backtickMapEscape) {
        [self addKey:@"`" withModifiers:0];
    }
    [_keyCommands addObject:[UIKeyCommand keyCommandWithInput:@"k"
                                                modifierFlags:UIKeyModifierCommand|UIKeyModifierShift
                                                       action:@selector(clearScrollback:)
                                         discoverabilityTitle:@"Clear Scrollback"]];
    return _keyCommands;
}

- (void)addKeys:(const char *)keys withModifiers:(UIKeyModifierFlags)modifiers {
    for (size_t i = 0; keys[i] != '\0'; i++) {
        [self addKey:[NSString stringWithFormat:@"%c", keys[i]] withModifiers:modifiers];
    }
}

- (void)addKey:(NSString *)key withModifiers:(UIKeyModifierFlags)modifiers {
    UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:key
                                                modifierFlags:modifiers
                                                       action:@selector(handleKeyCommand:)];
    if (@available(iOS 15, *)) {
        command.wantsPriorityOverSystemBehavior = YES;
    }
    [_keyCommands addObject:command];
}

- (void)keyCommandTriggered:(UIKeyCommand *)sender {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self handleKeyCommand:sender];
    });
}

- (void)handleCapsLockWithCommand:(UIKeyCommand *)command {
    CapsLockMapping target = UserPreferences.shared.capsLockMapping;
    NSString *newInput = command.input ? command.input : @"";
    UIKeyModifierFlags flags = command.modifierFlags;
    flags ^= UIKeyModifierAlphaShift;
    if(target == CapsLockMapEscape) {
        newInput = UIKeyInputEscape;
    } else if(target == CapsLockMapControl) {
        if([newInput length] == 0) {
            return;
        }
        flags |= UIKeyModifierControl;
    } else {
        return;
    }

    UIKeyCommand *newCommand = [UIKeyCommand keyCommandWithInput:newInput
                                                   modifierFlags:flags
                                                          action:@selector(keyCommandTriggered:)];
    [self handleKeyCommand:newCommand];
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (@available(iOS 13.4, *)) {
        UIKey *key = presses.anyObject.key;
        if (UserPreferences.shared.overrideControlSpace &&
            key.keyCode == UIKeyboardHIDUsageKeyboardSpacebar &&
            key.modifierFlags & UIKeyModifierControl) {
            return [self insertControlChar:' '];
        }
    }
    return [super pressesBegan:presses withEvent:event];
}

#pragma mark UITextInput stubs

#if 0
#define LogStub() NSLog(@"%s", __func__)
#else
#define LogStub()
#endif

- (NSInteger)clampedTextOffset:(NSInteger)offset {
    return MAX(0, MIN(self.terminal.displayView.cellCount, offset));
}

- (TerminalTextPosition *)terminalPositionFromTextPosition:(UITextPosition *)position {
    if (![position isKindOfClass:TerminalTextPosition.class])
        return nil;
    return (TerminalTextPosition *) position;
}

- (TerminalTextRange *)terminalRangeFromTextRange:(UITextRange *)range {
    if (![range isKindOfClass:TerminalTextRange.class])
        return nil;
    TerminalTextRange *terminalRange = (TerminalTextRange *) range;
    NSInteger start = [self clampedTextOffset:terminalRange.terminalStart.offset];
    NSInteger end = [self clampedTextOffset:terminalRange.terminalEnd.offset];
    return [TerminalTextRange rangeWithStart:start end:end];
}

- (NSRange)cellRangeFromTextRange:(UITextRange *)range {
    TerminalTextRange *terminalRange = [self terminalRangeFromTextRange:range];
    if (terminalRange == nil)
        return NSMakeRange(0, 0);
    NSInteger start = terminalRange.terminalStart.offset;
    NSInteger end = terminalRange.terminalEnd.offset;
    return NSMakeRange((NSUInteger) start, (NSUInteger) MAX(0, end - start));
}

- (CGPoint)displayPointFromTextInputPoint:(CGPoint)point {
    return [self convertPoint:point toView:self.terminal.displayView];
}

- (CGRect)textInputRectFromDisplayRect:(CGRect)rect {
    return [self convertRect:rect fromView:self.terminal.displayView];
}

- (NSWritingDirection)baseWritingDirectionForPosition:(nonnull UITextPosition *)position inDirection:(UITextStorageDirection)direction { LogStub(); return NSWritingDirectionLeftToRight; }
- (void)setBaseWritingDirection:(NSWritingDirection)writingDirection forRange:(nonnull UITextRange *)range { LogStub(); }
- (UITextPosition *)beginningOfDocument { LogStub(); return [TerminalTextPosition positionWithOffset:0]; }
- (CGRect)caretRectForPosition:(nonnull UITextPosition *)position {
    TerminalTextPosition *terminalPosition = [self terminalPositionFromTextPosition:position];
    if (terminalPosition == nil)
        return CGRectZero;
    NSInteger offset = [self clampedTextOffset:terminalPosition.offset];
    NSInteger rectOffset = MIN(offset, MAX(0, self.terminal.displayView.cellCount - 1));
    CGRect rect = [self textInputRectFromDisplayRect:[self.terminal.displayView firstRectForCellRange:NSMakeRange((NSUInteger) rectOffset, 1)]];
    if (offset == self.terminal.displayView.cellCount)
        rect.origin.x = CGRectGetMaxX(rect);
    rect.size.width = 2;
    return rect;
}
- (nullable UITextRange *)characterRangeAtPoint:(CGPoint)point {
    NSInteger offset = [self.terminal.displayView cellOffsetAtPoint:[self displayPointFromTextInputPoint:point]];
    if (offset >= self.terminal.displayView.cellCount)
        return nil;
    return [TerminalTextRange rangeWithStart:offset end:offset + 1];
}
- (nullable UITextRange *)characterRangeByExtendingPosition:(nonnull UITextPosition *)position inDirection:(UITextLayoutDirection)direction {
    TerminalTextPosition *terminalPosition = [self terminalPositionFromTextPosition:position];
    if (terminalPosition == nil)
        return nil;
    NSInteger offset = [self clampedTextOffset:terminalPosition.offset];
    if (direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp)
        return [TerminalTextRange rangeWithStart:MAX(0, offset - 1) end:offset];
    return [TerminalTextRange rangeWithStart:offset end:MIN(self.terminal.displayView.cellCount, offset + 1)];
}
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point {
    NSInteger offset = [self.terminal.displayView cellOffsetAtPoint:[self displayPointFromTextInputPoint:point]];
    return [TerminalTextPosition positionWithOffset:offset];
}
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point withinRange:(nonnull UITextRange *)range {
    TerminalTextRange *terminalRange = [self terminalRangeFromTextRange:range];
    if (terminalRange == nil)
        return nil;
    NSInteger offset = [self.terminal.displayView cellOffsetAtPoint:[self displayPointFromTextInputPoint:point]];
    offset = MAX(terminalRange.terminalStart.offset, MIN(terminalRange.terminalEnd.offset, offset));
    return [TerminalTextPosition positionWithOffset:offset];
}
- (NSComparisonResult)comparePosition:(nonnull UITextPosition *)position toPosition:(nonnull UITextPosition *)other {
    TerminalTextPosition *lhs = [self terminalPositionFromTextPosition:position];
    TerminalTextPosition *rhs = [self terminalPositionFromTextPosition:other];
    if (lhs == nil || rhs == nil)
        return NSOrderedSame;
    if (lhs.offset < rhs.offset)
        return NSOrderedAscending;
    if (lhs.offset > rhs.offset)
        return NSOrderedDescending;
    return NSOrderedSame;
}
- (UITextPosition *)endOfDocument { LogStub(); return [TerminalTextPosition positionWithOffset:self.terminal.displayView.cellCount]; }
- (CGRect)firstRectForRange:(nonnull UITextRange *)range {
    return [self textInputRectFromDisplayRect:[self.terminal.displayView firstRectForCellRange:[self cellRangeFromTextRange:range]]];
}
- (NSDictionary<NSAttributedStringKey,id> *)markedTextStyle { LogStub(); return nil; }
- (void)setMarkedTextStyle:(NSDictionary<NSAttributedStringKey,id> *)markedTextStyle { LogStub(); }
- (NSInteger)offsetFromPosition:(nonnull UITextPosition *)from toPosition:(nonnull UITextPosition *)toPosition {
    TerminalTextPosition *start = [self terminalPositionFromTextPosition:from];
    TerminalTextPosition *end = [self terminalPositionFromTextPosition:toPosition];
    if (start == nil || end == nil)
        return 0;
    return end.offset - start.offset;
}
- (nullable UITextPosition *)positionFromPosition:(nonnull UITextPosition *)position inDirection:(UITextLayoutDirection)direction offset:(NSInteger)offset {
    TerminalTextPosition *terminalPosition = [self terminalPositionFromTextPosition:position];
    if (terminalPosition == nil)
        return nil;
    NSInteger delta = offset;
    if (direction == UITextLayoutDirectionLeft)
        delta = -offset;
    else if (direction == UITextLayoutDirectionUp)
        delta = -offset * self.terminal.displayView.columns;
    else if (direction == UITextLayoutDirectionDown)
        delta = offset * self.terminal.displayView.columns;
    return [TerminalTextPosition positionWithOffset:[self clampedTextOffset:terminalPosition.offset + delta]];
}
- (nullable UITextPosition *)positionFromPosition:(nonnull UITextPosition *)position offset:(NSInteger)offset {
    TerminalTextPosition *terminalPosition = [self terminalPositionFromTextPosition:position];
    if (terminalPosition == nil)
        return nil;
    return [TerminalTextPosition positionWithOffset:[self clampedTextOffset:terminalPosition.offset + offset]];
}
- (nullable UITextPosition *)positionWithinRange:(nonnull UITextRange *)range farthestInDirection:(UITextLayoutDirection)direction {
    TerminalTextRange *terminalRange = [self terminalRangeFromTextRange:range];
    if (terminalRange == nil)
        return nil;
    if (direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp)
        return terminalRange.terminalStart;
    return terminalRange.terminalEnd;
}
- (void)replaceRange:(nonnull UITextRange *)range withText:(nonnull NSString *)text {
    (void) range;
    [self insertText:text];
}
- (void)setSelectedTextRange:(UITextRange *)selectedTextRange {
    TerminalTextRange *terminalRange = [self terminalRangeFromTextRange:selectedTextRange];
    if (terminalRange == nil)
        terminalRange = [TerminalTextRange rangeWithStart:0 end:0];
    [self.inputDelegate selectionWillChange:self];
    self.selectedRange = terminalRange;
    if (terminalRange.empty)
        [self.terminal.displayView clearSelection];
    else
        [self.terminal.displayView setSelectionFromCellOffset:terminalRange.terminalStart.offset
                                                 toCellOffset:terminalRange.terminalEnd.offset];
    [self.inputDelegate selectionDidChange:self];
}
- (nonnull NSArray<UITextSelectionRect *> *)selectionRectsForRange:(nonnull UITextRange *)range {
    NSRange cellRange = [self cellRangeFromTextRange:range];
    NSArray<NSValue *> *rectValues = [self.terminal.displayView rectsForCellRange:cellRange];
    NSMutableArray<UITextSelectionRect *> *rects = [NSMutableArray arrayWithCapacity:rectValues.count];
    for (NSUInteger i = 0; i < rectValues.count; i++) {
        TerminalSelectionRect *selectionRect = [TerminalSelectionRect new];
        selectionRect.terminalRect = [self textInputRectFromDisplayRect:rectValues[i].CGRectValue];
        selectionRect.terminalRange = range;
        selectionRect.terminalContainsStart = i == 0;
        selectionRect.terminalContainsEnd = i == rectValues.count - 1;
        [rects addObject:selectionRect];
    }
    return rects;
}
- (nullable UITextRange *)textRangeFromPosition:(nonnull UITextPosition *)fromPosition toPosition:(nonnull UITextPosition *)toPosition {
    TerminalTextPosition *start = [self terminalPositionFromTextPosition:fromPosition];
    TerminalTextPosition *end = [self terminalPositionFromTextPosition:toPosition];
    if (start == nil || end == nil)
        return nil;
    return [TerminalTextRange rangeWithStart:[self clampedTextOffset:start.offset]
                                         end:[self clampedTextOffset:end.offset]];
}

// conforming to UITextInput makes this view default to being an accessibility element, which blocks selecting anything in it
- (BOOL)isAccessibilityElement { return NO; }

@end
