//
//  GhosttyTerminalDisplay.h
//  iSH
//
//  Native terminal display backed by libghostty-vt.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@class GhosttyTerminalDisplay;

@protocol GhosttyTerminalDisplayDelegate <NSObject>
- (void)ghosttyTerminalDisplayDidResize:(GhosttyTerminalDisplay *)display columns:(int)columns rows:(int)rows;
- (void)ghosttyTerminalDisplay:(GhosttyTerminalDisplay *)display writePtyBytes:(const uint8_t *)bytes length:(size_t)length;
- (void)ghosttyTerminalDisplayDidUpdateScrollback:(GhosttyTerminalDisplay *)display totalRows:(NSUInteger)totalRows offset:(NSUInteger)offset visibleRows:(NSUInteger)visibleRows;
@end

@interface GhosttyTerminalDisplay : UIView

@property (nonatomic, weak, nullable) id<GhosttyTerminalDisplayDelegate> delegate;
@property (nonatomic, readonly) int columns;
@property (nonatomic, readonly) int rows;
@property (nonatomic, readonly) CGSize characterSize;
@property (nonatomic) BOOL terminalFocused;
@property (nonatomic) BOOL enableVoiceOverAnnounce;

- (void)writeBytes:(const void *)bytes length:(size_t)length completion:(void (^)(void))completion;
- (void)updateFontFamily:(NSString *)fontFamily
                fontSize:(CGFloat)fontSize
         foregroundColor:(NSString *)foregroundColor
         backgroundColor:(NSString *)backgroundColor
             cursorColor:(nullable NSString *)cursorColor
   colorPaletteOverrides:(nullable NSArray<NSString *> *)colorPaletteOverrides
             blinkCursor:(BOOL)blinkCursor
             cursorShape:(NSString *)cursorShape;
- (void)scrollToBottom;
- (void)scrollToRowOffset:(NSUInteger)rowOffset;
- (void)clearScrollback;
- (void)copyScreenToPasteboard;
- (BOOL)applicationCursorMode;

@end

NS_ASSUME_NONNULL_END
