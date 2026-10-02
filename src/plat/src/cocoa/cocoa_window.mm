// Cocoa windows: an NSWindow whose content view (PlatView) is a plain layer we
// hand CPU frames to, plus the NSTextInputClient half of IME support, touchpad
// phases and gestures, and both ends of drag and drop.
#include "cocoa/cocoa_internal.h"

#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using plat::Button;
using plat::Event;
using plat::EventType;
using plat::HitArea;
using plat::Key;
using plat::cocoa::CocoaWindow;

namespace {

// UTF-8 byte length of the first `utf16` code units of s — preedit cursor
// offsets are bytes in plat but NSRange counts UTF-16 units.
int utf8Offset(NSString *s, NSUInteger utf16) {
    utf16 = std::min<NSUInteger>(utf16, s.length);
    return int([[s substringToIndex:utf16] lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
}

NSString *plainString(id s) {
    return [s isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)s string]
                                                        : (NSString *)s;
}

// AppKit hands Cmd/Ctrl chords, arrows and function keys to insertText: as
// control characters or private-use code points (NSUpArrowFunctionKey…);
// none of that is text.
std::string filteredText(NSString *s) {
    NSMutableString *out = [NSMutableString string];
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *sub, NSRange, NSRange, BOOL *) {
                         const unichar c = [sub characterAtIndex:0];
                         if (c < 0x20 || c == 0x7f || (c >= 0xF700 && c <= 0xF8FF))
                             return;
                         [out appendString:sub];
                       }];
    return out.length ? std::string(out.UTF8String) : std::string();
}

// Device-dependent modifier bits (IOKit's NX_DEVICE*KEYMASK) in
// NSEvent.modifierFlags: the only way to tell which of two Shifts changed.
constexpr NSEventModifierFlags kDevLCtrl = 0x0001, kDevLShift = 0x0002, kDevRShift = 0x0004,
                               kDevLCmd = 0x0008, kDevRCmd = 0x0010, kDevLAlt = 0x0020,
                               kDevRAlt = 0x0040, kDevRCtrl = 0x2000;

} // namespace

// ── Frame clock target (CADisplayLink retains its target) ──────────────────
@interface                                PlatFrameTarget : NSObject
@property(nonatomic, assign) CocoaWindow *owner;
@end
@implementation PlatFrameTarget
- (void)tick:(id)link {
    if (_owner)
        _owner->deliverFrameIfDue();
}
@end

// ── Content view ────────────────────────────────────────────────────────────

@implementation PlatView {
    NSTrackingArea          *_tracking;
    NSMutableString         *_marked; // current preedit, nil when none
    NSRange                  _markedSel;
    // State of the key event being run through interpretKeyEvents:
    bool                     _inKeyDown;
    bool                     _imeTouched;
    std::vector<std::string> _pendingText;
    bool                     _keyDownSent[128];
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer                = YES;
        // We own layer.contents; AppKit must never redraw or clear it.
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        // A live-resize frame shows the old image pinned to the top-left
        // until the synchronous repaint lands, instead of stretching it.
        self.layerContentsPlacement    = NSViewLayerContentsPlacementTopLeft;
        [self registerForDraggedTypes:[PlatView dropTypes]];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
} // top-left origin, like plat
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (void)updateLayer {
    // AppKit wants the view redrawn (exposure after deminiaturize, a screen
    // change, …): that is a Frame in plat's terms.
    if (_owner)
        _owner->requestFrame();
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)ev {
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return NO;
} // the hit-test callback decides

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (_owner)
        _owner->viewResized();
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_owner)
        _owner->backingChanged();
}

- (void)updateTrackingAreas {
    if (_tracking)
        [self removeTrackingArea:_tracking];
    // ActiveAlways: hover feedback in inactive windows too, as on the other
    // platforms. InVisibleRect keeps it sized to the view for free.
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                     NSTrackingCursorUpdate | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

// ── pointer ─────────────────────────────────────────────────────────────────

- (void)cursorUpdate:(NSEvent *)ev {
    if (_owner)
        _owner->applyCursor();
}

- (void)mouseEntered:(NSEvent *)ev {
    if (!_owner || _owner->pointerInside)
        return;
    _owner->pointerInside = true;
    _owner->applyCursor();
    _owner->emit(
        {.type = EventType::PointerEnter,
         .pos  = _owner->viewPoint(ev),
         .mods = plat::cocoa::modsFromFlags(ev.modifierFlags)}
    );
}

- (void)mouseExited:(NSEvent *)ev {
    if (!_owner || !_owner->pointerInside)
        return;
    _owner->pointerLeft();
}

- (void)movedWith:(NSEvent *)ev {
    if (!_owner)
        return;
    // Motion can precede mouseEntered: (window just mapped under the
    // pointer); the contract wants Enter first.
    if (!_owner->pointerInside)
        [self mouseEntered:ev];
    if (ev.type != NSEventTypeMouseMoved)
        _owner->lastMouseEvent = ev; // what a startDrag() from this Move starts from
    _owner->emit(
        {.type = EventType::PointerMove,
         .pos  = _owner->viewPoint(ev),
         .mods = plat::cocoa::modsFromFlags(ev.modifierFlags)}
    );
}
- (void)mouseMoved:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)mouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)rightMouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}
- (void)otherMouseDragged:(NSEvent *)ev {
    [self movedWith:ev];
}

- (void)button:(Button)b down:(bool)down event:(NSEvent *)ev {
    if (!_owner)
        return;
    const plat::Point p = _owner->viewPoint(ev);
    const auto        i = size_t(b);
    if (down) {
        _owner->swallowUp[i] = false;
        if (b == Button::Left && _owner->hitTest) {
            const HitArea a = _owner->hitTest(p);
            if (plat::isNonClient(a)) {
                _owner->swallowUp[i] = true;
                if (a == HitArea::Caption)
                    [self captionPress:ev];
                // Resize*: macOS resizes from its own edge zone, which sits on
                // and just outside the frame and already took the press if it
                // was there; a press further in is swallowed like on the
                // other backends but starts nothing (AppKit has no public
                // "begin interactive resize").
                return;
            }
        }
        _owner->lastMouseEvent = ev; // what a startDrag() from this Down starts from
        _owner->emit(
            {.type   = EventType::PointerDown,
             .pos    = p,
             .button = b,
             .clicks = int(std::max<NSInteger>(1, ev.clickCount)),
             .mods   = plat::cocoa::modsFromFlags(ev.modifierFlags)}
        );
    } else {
        _owner->lastMouseEvent = nil;
        if (_owner->swallowUp[i]) {
            _owner->swallowUp[i] = false;
            return;
        }
        _owner->emit(
            {.type   = EventType::PointerUp,
             .pos    = p,
             .button = b,
             .mods   = plat::cocoa::modsFromFlags(ev.modifierFlags)}
        );
    }
}

- (void)captionPress:(NSEvent *)ev {
    NSWindow *w = self.window;
    if (ev.clickCount == 2) {
        // The title-bar double-click action is a user setting (System
        // Settings → Desktop & Dock); honour it like a native title bar.
        NSString *action =
            [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
        if ([action isEqualToString:@"None"])
            return;
        if ([action isEqualToString:@"Minimize"] ||
            (!action &&
             [NSUserDefaults.standardUserDefaults boolForKey:@"AppleMiniaturizeOnDoubleClick"]))
            [w performMiniaturize:nil];
        else
            [w performZoom:nil]; // "Maximize", "Fill" (no public API) and the default
        return;
    }
    // Hands the drag to the window server: Spaces, snapping to screen
    // edges and tiling all keep working.
    [w performWindowDragWithEvent:ev];
}

- (Button)otherButton:(NSEvent *)ev ok:(bool *)ok {
    *ok = true;
    switch (ev.buttonNumber) {
    case 2:
        return Button::Middle;
    case 3:
        return Button::Back;
    case 4:
        return Button::Forward;
    default:
        *ok = false;
        return Button::Middle;
    }
}

- (void)mouseDown:(NSEvent *)ev {
    [self button:Button::Left down:true event:ev];
}
- (void)mouseUp:(NSEvent *)ev {
    [self button:Button::Left down:false event:ev];
}
- (void)rightMouseDown:(NSEvent *)ev {
    [self button:Button::Right down:true event:ev];
}
- (void)rightMouseUp:(NSEvent *)ev {
    [self button:Button::Right down:false event:ev];
}
- (void)otherMouseDown:(NSEvent *)ev {
    bool         ok;
    const Button b = [self otherButton:ev ok:&ok];
    if (ok)
        [self button:b down:true event:ev];
}
- (void)otherMouseUp:(NSEvent *)ev {
    bool         ok;
    const Button b = [self otherButton:ev ok:&ok];
    if (ok)
        [self button:b down:false event:ev];
}

- (void)scrollWheel:(NSEvent *)ev {
    if (!_owner)
        return;
    Event e{.type = EventType::Scroll, .pos = _owner->viewPoint(ev)};
    // AppKit's deltas are "content moves by" (+y = towards the top, i.e.
    // scrolling up); plat's +dy is "scroll down", so both axes flip. The
    // user's natural-scrolling choice is already applied to the deltas.
    if (ev.hasPreciseScrollingDeltas) {
        e.precise = true;
        e.dx      = 0.0 - ev.scrollingDeltaX; // not -x: no -0 on Begin/End
        e.dy      = 0.0 - ev.scrollingDeltaY;
    } else {
        e.dx = -ev.deltaX; // notches (AppKit may accelerate them past ±1)
        e.dy = -ev.deltaY;
    }
    // Touchpad phases. A two-finger scroll is Begin, Update…, End when the
    // fingers lift; if the flick coasts, AppKit follows with a momentum
    // sequence, which plat reports as Momentum… closed by a second End. So
    // End always means "no more motion for now", and a Momentum after an End
    // means coasting resumed it. MayBegin (fingers resting, not moving yet)
    // and Stationary carry nothing and are dropped.
    const NSEventPhase p = ev.phase, m = ev.momentumPhase;
    if (m != NSEventPhaseNone) {
        e.phase = (m & (NSEventPhaseEnded | NSEventPhaseCancelled)) ? plat::ScrollPhase::End
                                                                    : plat::ScrollPhase::Momentum;
    } else if (p & NSEventPhaseBegan) {
        e.phase = plat::ScrollPhase::Begin;
    } else if (p & NSEventPhaseChanged) {
        e.phase = plat::ScrollPhase::Update;
    } else if (p & (NSEventPhaseEnded | NSEventPhaseCancelled)) {
        e.phase = plat::ScrollPhase::End;
    } else if (p != NSEventPhaseNone) {
        return; // MayBegin / Stationary
    }
    const bool bracket = e.phase == plat::ScrollPhase::Begin || e.phase == plat::ScrollPhase::End;
    if (e.dx == 0 && e.dy == 0 && !bracket)
        return; // phase bookkeeping without motion
    e.mods = plat::cocoa::modsFromFlags(ev.modifierFlags);
    _owner->emit(e);
}

// ── touchpad gestures ───────────────────────────────────────────────────────

// The one-shot navigation swipe (three fingers, or two with "Swipe between
// pages"). AppKit's deltaX is +1 when the fingers moved right, which is
// "back" — the SwipeGesture contract, so the signs pass straight through.
- (void)swipeWithEvent:(NSEvent *)ev {
    if (!_owner)
        return;
    const auto sign = [](CGFloat v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; };
    _owner->emit(
        {.type    = EventType::SwipeGesture,
         .pos     = _owner->viewPoint(ev),
         .dx      = sign(ev.deltaX),
         .dy      = sign(ev.deltaY),
         .mods    = plat::cocoa::modsFromFlags(ev.modifierFlags),
         .gesture = plat::Gesture::Swipe,
         .fingers = 3}
    );
}

// ── keys ────────────────────────────────────────────────────────────────────

- (Event)keyEvent:(NSEvent *)ev type:(EventType)t {
    return {
        .type     = t,
        .key      = plat::cocoa::keyFromKeyCode(ev.keyCode),
        .scancode = ev.keyCode,
        .mods     = plat::cocoa::modsFromFlags(ev.modifierFlags),
        // -isARepeat throws for FlagsChanged events.
        .repeat   = ev.type == NSEventTypeKeyDown && ev.isARepeat
    };
}

- (void)keyDown:(NSEvent *)ev {
    if (!_owner)
        return;
    const uint16_t vk = ev.keyCode;
    // Command chords never produce text (Cmd+C is a shortcut, not 'c').
    const bool     wantText =
        _owner->textInput.enabled && !(ev.modifierFlags & NSEventModifierFlagCommand);
    if (!wantText) {
        if (vk < 128)
            _keyDownSent[vk] = true;
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
        return;
    }
    // Run the key through the input method first, buffering what it commits,
    // so the order stays KeyDown → TextInput. A key the IME is composing with
    // (dead key, Kana, Pinyin, the Enter that commits a candidate) is the
    // IME's, not the app's: it gets no KeyDown, or Enter would also send.
    const bool hadMarked = _marked != nil;
    _inKeyDown           = true;
    _imeTouched          = false;
    _pendingText.clear();
    [self interpretKeyEvents:@[ ev ]];
    _inKeyDown = false;
    if (!hadMarked && !_imeTouched && _marked == nil) {
        if (vk < 128)
            _keyDownSent[vk] = true;
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
    }
    auto pending = std::move(_pendingText);
    _pendingText.clear();
    for (auto &t : pending)
        if (_owner)
            _owner->emit({.type = EventType::TextInput, .text = std::move(t)});
}

- (void)keyUp:(NSEvent *)ev {
    if (!_owner)
        return;
    const uint16_t vk = ev.keyCode;
    // Only release what we pressed: an IME-consumed key had no KeyDown.
    if (vk < 128) {
        if (!_keyDownSent[vk])
            return;
        _keyDownSent[vk] = false;
    }
    _owner->emit([self keyEvent:ev type:EventType::KeyUp]);
}

- (void)flagsChanged:(NSEvent *)ev {
    if (!_owner)
        return;
    const Key k = plat::cocoa::keyFromKeyCode(ev.keyCode);
    if (k == Key::Unknown) // Fn
        return;
    const NSEventModifierFlags f = ev.modifierFlags;
    if (k == Key::CapsLock) {
        // The event only reports the new lock state; there is no release of
        // the physical key, so give each toggle a press and a release.
        _owner->emit([self keyEvent:ev type:EventType::KeyDown]);
        _owner->emit([self keyEvent:ev type:EventType::KeyUp]);
        return;
    }
    NSEventModifierFlags dev = 0, any = 0;
    switch (k) {
    case Key::ShiftLeft:
        dev = kDevLShift, any = NSEventModifierFlagShift;
        break;
    case Key::ShiftRight:
        dev = kDevRShift, any = NSEventModifierFlagShift;
        break;
    case Key::ControlLeft:
        dev = kDevLCtrl, any = NSEventModifierFlagControl;
        break;
    case Key::ControlRight:
        dev = kDevRCtrl, any = NSEventModifierFlagControl;
        break;
    case Key::AltLeft:
        dev = kDevLAlt, any = NSEventModifierFlagOption;
        break;
    case Key::AltRight:
        dev = kDevRAlt, any = NSEventModifierFlagOption;
        break;
    case Key::SuperLeft:
        dev = kDevLCmd, any = NSEventModifierFlagCommand;
        break;
    case Key::SuperRight:
        dev = kDevRCmd, any = NSEventModifierFlagCommand;
        break;
    default:
        return;
    }
    // Some event sources leave the device bits clear; fall back to the
    // side-less flag then.
    const bool     devKnown = (f & (kDevLShift | kDevRShift | kDevLCtrl | kDevRCtrl | kDevLAlt |
                                    kDevRAlt | kDevLCmd | kDevRCmd)) != 0 ||
                              !(f & (NSEventModifierFlagShift | NSEventModifierFlagControl |
                                     NSEventModifierFlagOption | NSEventModifierFlagCommand));
    const bool     down     = devKnown ? (f & dev) != 0 : (f & any) != 0;
    const uint16_t vk       = ev.keyCode;
    if (vk < 128)
        _keyDownSent[vk] = down;
    _owner->emit([self keyEvent:ev type:down ? EventType::KeyDown : EventType::KeyUp]);
}

// Key equivalents that no menu item claimed come here before keyDown:; let
// them through to keyDown: so the app sees Cmd+C etc. as KeyDown.
- (BOOL)performKeyEquivalent:(NSEvent *)ev {
    return NO;
}

// ── NSTextInputClient ───────────────────────────────────────────────────────

- (void)emitPreedit {
    if (!_owner)
        return;
    Event e{.type = EventType::TextPreedit};
    if (_marked) {
        e.text = _marked.UTF8String;
        if (_markedSel.location != NSNotFound) {
            e.preeditCursorBegin = utf8Offset(_marked, _markedSel.location);
            e.preeditCursorEnd   = utf8Offset(_marked, NSMaxRange(_markedSel));
        }
    }
    _owner->emit(e);
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    if (!_owner)
        return;
    if (_marked) { // a commit ends the composition
        _marked = nil;
        [self emitPreedit];
    }
    std::string t = filteredText(plainString(string));
    if (t.empty())
        return;
    if (_inKeyDown)
        _pendingText.push_back(std::move(t));
    else // character palette, dictation, …
        _owner->emit({.type = EventType::TextInput, .text = std::move(t)});
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
    NSString *s = plainString(string);
    _imeTouched = true;
    _marked     = s.length ? [s mutableCopy] : nil;
    _markedSel  = selectedRange;
    [self emitPreedit];
}

- (void)unmarkText {
    // The input context tells us the composition is over; whatever it wants
    // committed arrives through insertText:, so only the preedit ends here.
    if (!_marked)
        return;
    _marked = nil;
    [self emitPreedit];
}

- (void)discardComposition {
    if (!_marked)
        return;
    [self.inputContext discardMarkedText];
    _marked = nil;
    [self emitPreedit];
}

- (BOOL)hasMarkedText {
    return _marked != nil;
}
- (NSRange)markedRange {
    return _marked ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0);
}
// plat does not expose the document to the IME (no reconversion); the
// selection is an empty range at the end of the composition.
- (NSRange)selectedRange {
    return _marked ? _markedSel : NSMakeRange(0, 0);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange {
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (!_owner || !self.window)
        return NSZeroRect;
    const plat::Rect c        = _owner->textInput.caret;
    // The caret is logical px relative to the window = points in our flipped
    // view; the IME wants screen coordinates (bottom-left origin).
    const NSRect     inView   = NSMakeRect(c.x, c.y, std::max(c.w, 1), std::max(c.h, 1));
    const NSRect     inWindow = [self convertRect:inView toView:nil];
    return [self.window convertRectToScreen:inWindow];
}

// Keys that map to editing commands (insertNewline:, deleteBackward:, …)
// already reached the app as KeyDown; swallowing the selector also stops
// NSResponder's default from beeping.
- (void)doCommandBySelector:(SEL)selector {
}

// ── drag and drop: target ───────────────────────────────────────────────────
// Registered types decide which drags AppKit shows us at all (it matches
// exact types, not conformance): the standard plat types, the common
// native ones they come from, and generic data so plat-to-plat drags of
// custom (dynamic-UTI) types still get DropEnter.

+ (NSArray<NSPasteboardType> *)dropTypes {
    return @[
        NSPasteboardTypeFileURL,
        NSPasteboardTypeURL,
        NSPasteboardTypeString,
        NSPasteboardTypeHTML,
        NSPasteboardTypePNG,
        NSPasteboardTypeTIFF,
        NSPasteboardTypeRTF,
        @"public.data",
        @"public.item",
    ];
}

- (Event)dropEvent:(id<NSDraggingInfo>)info type:(EventType)t withData:(bool)withData {
    const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    Event         e{.type = t, .pos = {p.x, p.y}};
    e.allowedActions = plat::cocoa::dropActionsFromOperation(info.draggingSourceOperationMask);
    e.dropAction     = plat::cocoa::preferredAction(e.allowedActions);
    NSPasteboard *pb = info.draggingPasteboard;
    for (auto &m : plat::cocoa::pasteboardMimes(pb)) {
        plat::DataItem item{m, {}};
        // Data only on Drop, and only for types that are cheap and meant
        // for us: the standard ones and plat-style MIME/dynamic types (not
        // file promises or the TIFF a PNG drag also offers, say).
        const bool     wanted = m == "text/plain;charset=utf-8" || m == "text/html" ||
                                m == "image/png" || m == "text/uri-list" ||
                                (m.find('/') != std::string::npos && m.rfind("image/", 0) != 0);
        if (withData && wanted)
            item.data = plat::cocoa::readPasteboard(pb, m).value_or(std::string());
        e.items.push_back(std::move(item));
    }
    if (withData) {
        e.uris = plat::cocoa::pasteboardUris(pb);
        if (NSString *s = [pb stringForType:NSPasteboardTypeString])
            e.text = s.UTF8String;
    }
    return e;
}

// The app's reply as the operation AppKit shows (cursor badge) and hands
// the source; None — or an action the source does not allow — rejects, and
// then performDragOperation: (our Drop) is never called.
- (NSDragOperation)dropReply:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    const NSDragOperation op = plat::cocoa::operationFromAction(_owner->dropReply);
    return (op & info.draggingSourceOperationMask) ? op : NSDragOperationNone;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    Event e           = [self dropEvent:info type:EventType::DropEnter withData:false];
    // Default answer until the app says otherwise: what the source prefers.
    _owner->dropReply = e.dropAction;
    _owner->emit(std::move(e));
    return [self dropReply:info];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NSDragOperationNone;
    _owner->emit([self dropEvent:info type:EventType::DropMove withData:false]);
    return [self dropReply:info];
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
    if (_owner)
        _owner->emit({.type = EventType::DropLeave});
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (!_owner)
        return NO;
    Event e      = [self dropEvent:info type:EventType::Drop withData:true];
    e.dropAction = _owner->dropReply; // what the target agreed to
    _owner->emit(std::move(e));
    return YES;
}

// ── drag and drop: source ───────────────────────────────────────────────────

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    return _owner ? plat::cocoa::operationFromActions(_owner->dragActions) : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    if (!_owner || !_owner->dragging)
        return;
    _owner->dragging       = false;
    // AppKit consumed the release; nothing of this press is still held.
    _owner->lastMouseEvent = nil;
    _owner->emit(
        {.type       = EventType::DragFinished,
         .dropAction = plat::cocoa::dropActionFromOperation(operation)}
    );
    _owner->app->noteWork();
}

@end

// ── NSWindow subclass: the title-bar zone of Custom windows ─────────────────
// With NSWindowStyleMaskFullSizeContentView our view extends under the
// (transparent) title bar, but AppKit's titlebar view still sits on top and
// takes every press in the top ~28 pt for its own drag/zoom handling — the
// app's hit-test callback and its own buttons there would never see them.
// Route those events to PlatView instead (it applies the hit test and does
// the native drag itself); only the traffic lights keep theirs.
@interface                                PlatNSWindow : NSWindow
@property(nonatomic, assign) CocoaWindow *owner;
@end

@implementation PlatNSWindow {
    NSUInteger _captured; // buttons whose press we took from the title bar
}

- (BOOL)titlebarWouldSteal:(NSEvent *)ev {
    NSView *content = self.contentView;
    NSView *frame   = content.superview; // theme frame: hitTest: takes window coords
    NSView *hit     = [frame hitTest:ev.locationInWindow];
    if (!hit || hit == content || [hit isDescendantOf:content])
        return NO;
    for (NSView *v = hit; v; v = v.superview)
        if ([v isKindOfClass:[NSControl class]]) // close / minimise / zoom
            return NO;
    const NSPoint p = [content convertPoint:ev.locationInWindow fromView:nil];
    return [content mouse:p inRect:content.bounds];
}

- (void)routeToView:(NSEvent *)ev {
    NSView *v = self.contentView;
    switch (ev.type) {
    case NSEventTypeLeftMouseDown:
        [v mouseDown:ev];
        break;
    case NSEventTypeLeftMouseUp:
        [v mouseUp:ev];
        break;
    case NSEventTypeLeftMouseDragged:
        [v mouseDragged:ev];
        break;
    case NSEventTypeRightMouseDown:
        [v rightMouseDown:ev];
        break;
    case NSEventTypeRightMouseUp:
        [v rightMouseUp:ev];
        break;
    case NSEventTypeRightMouseDragged:
        [v rightMouseDragged:ev];
        break;
    case NSEventTypeOtherMouseDown:
        [v otherMouseDown:ev];
        break;
    case NSEventTypeOtherMouseUp:
        [v otherMouseUp:ev];
        break;
    case NSEventTypeOtherMouseDragged:
        [v otherMouseDragged:ev];
        break;
    case NSEventTypeScrollWheel:
        [v scrollWheel:ev];
        break;
    default:
        break;
    }
}

- (void)sendEvent:(NSEvent *)ev {
    if (!_owner || _owner->decorations != plat::Decorations::Custom) {
        [super sendEvent:ev];
        return;
    }
    const NSUInteger bit = NSUInteger(1) << std::min<NSInteger>(ev.buttonNumber, 31);
    switch (ev.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
        if ([self titlebarWouldSteal:ev]) {
            // Bypassing super also bypasses its click-to-focus.
            if (!self.isKeyWindow && self.canBecomeKeyWindow)
                [self makeKeyAndOrderFront:nil];
            _captured |= bit;
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        if (_captured & bit) { // AppKit would send these to the titlebar view
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
        if (_captured & bit) {
            _captured &= ~bit;
            [self routeToView:ev];
            return;
        }
        break;
    case NSEventTypeScrollWheel:
        if ([self titlebarWouldSteal:ev]) {
            [self routeToView:ev];
            return;
        }
        break;
    default:
        break;
    }
    [super sendEvent:ev];
}
@end

// ── Window delegate ─────────────────────────────────────────────────────────
@interface                                PlatWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) CocoaWindow *owner;
@end

@implementation PlatWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    // The app decides; the window only goes away when it is destroyed.
    if (_owner)
        _owner->emit({.type = EventType::CloseRequested});
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->emit({.type = EventType::FocusIn});
    _owner->emit({.type = EventType::StateChanged});
}
- (void)windowDidResignKey:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->emit({.type = EventType::FocusOut});
    _owner->emit({.type = EventType::StateChanged});
}
- (void)windowDidMiniaturize:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidDeminiaturize:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->stateMaybeChanged();
    _owner->requestFrame();
}
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged();
}
- (void)windowDidMove:(NSNotification *)n {
    if (_owner)
        _owner->moved();
}
- (void)windowDidResize:(NSNotification *)n {
    if (_owner)
        _owner->stateMaybeChanged(); // zoom has no notification of its own
}
- (void)windowDidChangeOcclusionState:(NSNotification *)n {
    // Display links stop while occluded; a Frame asked for meanwhile is
    // owed now.
    if (_owner && (_owner->window.occlusionState & NSWindowOcclusionStateVisible) &&
        _owner->framePending)
        _owner->requestFrame();
}
@end

// ── CocoaWindow ─────────────────────────────────────────────────────────────
namespace plat::cocoa {

CocoaWindow::CocoaWindow(CocoaApp *a, const WindowDesc &d) : app(a), decorations(d.decorations) {
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (d.resizable)
        style |= NSWindowStyleMaskResizable;
    if (d.decorations == Decorations::Custom)
        style |= NSWindowStyleMaskFullSizeContentView;

    PlatNSWindow *pw =
        [[PlatNSWindow alloc] initWithContentRect:NSMakeRect(0, 0, d.size.w, d.size.h)
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    pw.owner                  = this;
    window                    = pw;
    window.releasedWhenClosed = NO; // ARC owns it
    window.tabbingMode        = NSWindowTabbingModeDisallowed;
    window.restorable         = NO; // nothing to restore; see CocoaApp()
    // sRGB backing: plat colours are sRGB, and it keeps readback exact.
    window.colorSpace         = NSColorSpace.sRGBColorSpace;
    if (d.decorations == Decorations::Custom) {
        // The app draws the title bar; the traffic lights stay native (they
        // float over our content, top-left), like msga's Qt build does.
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility            = NSWindowTitleHidden;
        // An empty toolbar in the compact unified style gives the traffic
        // lights the standard, centred placement of a unified macOS header
        // (Finder's) instead of a bare title bar's, as msga's Qt build did;
        // titleBarHeight() reports the band they sit in. AppKit owns the
        // buttons and their behaviour; PlatNSWindow routes the presses on the
        // rest of the band (the toolbar's own views included) to PlatView.
        NSToolbar *toolbar                = [[NSToolbar alloc] initWithIdentifier:@"plat.titlebar"];
        toolbar.allowsUserCustomization   = NO;
        window.toolbar                    = toolbar;
        window.toolbarStyle               = NSWindowToolbarStyleUnifiedCompact;
        window.titlebarSeparatorStyle     = NSTitlebarSeparatorStyleNone;
    }
    window.title = [NSString stringWithUTF8String:d.title.c_str()] ?: @"";
    if (d.minSize.w > 0 || d.minSize.h > 0)
        window.contentMinSize = NSMakeSize(d.minSize.w, d.minSize.h);

    delegate        = [PlatWindowDelegate new];
    delegate.owner  = this;
    window.delegate = delegate;

    view               = [[PlatView alloc] initWithFrame:NSMakeRect(0, 0, d.size.w, d.size.h)];
    window.contentView = view;
    view.owner         = this; // after contentView: its setFrameSize must not emit yet
    [window makeFirstResponder:view];
    [window center];
    if (d.position)
        setPosition(*d.position);

    _lastSize                = size();
    _lastScale               = window.backingScaleFactor;
    view.layer.contentsScale = _lastScale;

    _frameTarget       = [PlatFrameTarget new];
    _frameTarget.owner = this;
    if (@available(macOS 14.0, *)) {
        CADisplayLink *link = [view displayLinkWithTarget:_frameTarget selector:@selector(tick:)];
        // Common modes: keeps ticking inside live-resize and menu tracking.
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        link.paused  = YES;
        _displayLink = link;
    }

    if (d.visible)
        show();
}

CocoaWindow::~CocoaWindow() {
    if (_cursorHidden)
        [NSCursor unhide];
    if (@available(macOS 14.0, *))
        [(CADisplayLink *)_displayLink invalidate];
    _frameTarget.owner             = nullptr;
    view.owner                     = nullptr;
    delegate.owner                 = nullptr;
    ((PlatNSWindow *)window).owner = nullptr;
    window.delegate                = nil;
    [window orderOut:nil];
    [window close];
    if (presented)
        CGImageRelease(presented);
    app->forget(this);
}

void CocoaWindow::emit(Event e) {
    e.window = this;
    app->emitEvent(e);
}

Point CocoaWindow::viewPoint(NSEvent *ev) const {
    NSPoint p = ev.locationInWindow;
    // Synthesised events without a window carry screen coordinates.
    if (ev.window != window)
        p = [window convertPointFromScreen:p];
    p = [view convertPoint:p fromView:nil];
    return {p.x, p.y};
}

void CocoaWindow::setTitle(std::string_view t) {
    window.title = [[NSString alloc] initWithBytes:t.data()
                                            length:t.size()
                                          encoding:NSUTF8StringEncoding]
                       ?: @"";
}

Size CocoaWindow::size() const {
    const NSSize s = view.bounds.size;
    return {int(std::lround(s.width)), int(std::lround(s.height))};
}

double CocoaWindow::scale() const {
    return window.backingScaleFactor;
}

void CocoaWindow::setSize(Size s) {
    [window setContentSize:NSMakeSize(s.w, s.h)];
}

void CocoaWindow::setMinSize(Size s) {
    window.contentMinSize = NSMakeSize(s.w, s.h);
}

void CocoaWindow::show() {
    [window makeKeyAndOrderFront:nil];
    requestFrame();
}

void CocoaWindow::hide() {
    [window orderOut:nil];
}

void CocoaWindow::minimize() {
    [window miniaturize:nil];
}

void CocoaWindow::setMaximized(bool on) {
    // macOS has no maximised state; "zoom" to the best size is the equivalent
    // (and what the green button's Option-click and a title double-click do).
    if (window.isZoomed != on)
        [window zoom:nil];
}

void CocoaWindow::setFullscreen(bool on) {
    if (isFullscreen() != on)
        [window toggleFullScreen:nil];
}

void CocoaWindow::setAlwaysOnTop(bool on) {
    window.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

bool CocoaWindow::isAlwaysOnTop() const {
    return window.level >= NSFloatingWindowLevel;
}

void CocoaWindow::setDarkChrome(bool dark) {
    // Left to the system, a dark app on a light-mode Mac gets the Aqua
    // title bar: a bright rim along the top edge, light traffic lights.
    window.appearance =
        [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}

double CocoaWindow::titleBarHeight() const {
    if (decorations != Decorations::Custom)
        return 0;
    // With the full-size content view the content spans the frame; the
    // layout rect is what the title bar (and toolbar) leave of it.
    return std::max(0.0, NSHeight(window.frame) - NSHeight(window.contentLayoutRect));
}

bool CocoaWindow::isMaximized() const {
    return window.isZoomed;
}
bool CocoaWindow::isFullscreen() const {
    return (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}
bool CocoaWindow::isActive() const {
    return window.isKeyWindow;
}

void CocoaWindow::activate() {
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}

// The content view's rect in the flipped virtual desktop: for Custom
// decorations (full-size content view) that includes the title-bar strip,
// the same area the app paints and hit-tests.
std::optional<Point> CocoaWindow::position() const {
    const NSRect r = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
    const Rect   f = flipRect(r);
    return Point{double(f.x), double(f.y)};
}

bool CocoaWindow::setPosition(Point p) {
    // Move the frame by however far the content is from where it should be;
    // setFrameOrigin: does not constrain to the screen (only user drags and
    // -constrainFrameRect: do), so the window lands exactly there.
    const NSRect cur    = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
    const NSRect target = unflipRect(
        {int(std::lround(p.x)),
         int(std::lround(p.y)),
         int(std::lround(cur.size.width)),
         int(std::lround(cur.size.height))}
    );
    NSPoint o = window.frame.origin;
    o.x += target.origin.x - cur.origin.x;
    o.y += target.origin.y - cur.origin.y;
    [window setFrameOrigin:o];
    return true;
}

uint64_t CocoaWindow::monitor() const {
    return screenId(window.screen); // nil (0) while entirely off-screen
}

void CocoaWindow::moved() {
    emit({.type = EventType::Moved});
}

void CocoaWindow::stateMaybeChanged() {
    const bool z = window.isZoomed, m = window.isMiniaturized, f = isFullscreen();
    if (z == _lastZoomed && m == _lastMini && f == _lastFull)
        return;
    _lastZoomed = z;
    _lastMini   = m;
    _lastFull   = f;
    emit({.type = EventType::StateChanged});
}

// ── cursors ─────────────────────────────────────────────────────────────────

namespace {
NSCursor *nsCursor(Cursor c) {
    if (@available(macOS 15.0, *)) {
        // The real window-edge resize cursors (public since macOS 15).
        auto frame = [](NSCursorFrameResizePosition p) {
            return [NSCursor frameResizeCursorFromPosition:p
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        };
        switch (c) {
        case Cursor::ResizeH:
            return frame(NSCursorFrameResizePositionRight);
        case Cursor::ResizeV:
            return frame(NSCursorFrameResizePositionBottom);
        case Cursor::ResizeNWSE:
            return frame(NSCursorFrameResizePositionBottomRight);
        case Cursor::ResizeNESW:
            return frame(NSCursorFrameResizePositionBottomLeft);
        default:
            break;
        }
    }
    switch (c) {
    case Cursor::IBeam:
        return NSCursor.IBeamCursor;
    case Cursor::Hand:
        return NSCursor.pointingHandCursor;
    case Cursor::Crosshair:
        return NSCursor.crosshairCursor;
    case Cursor::NotAllowed:
        return NSCursor.operationNotAllowedCursor;
    case Cursor::Move:
    case Cursor::Grab:
        return NSCursor.openHandCursor;
    case Cursor::Grabbing:
        return NSCursor.closedHandCursor;
    case Cursor::ResizeH:
        return NSCursor.resizeLeftRightCursor;
    case Cursor::ResizeV:
        return NSCursor.resizeUpDownCursor;
    // No public diagonal-resize, busy or spinning cursor before macOS 15
    // (the beach ball is the system's, shown when the app stops pumping).
    default:
        return NSCursor.arrowCursor;
    }
}
} // namespace

void CocoaWindow::setCursor(Cursor c) {
    _cursor = c;
    if (pointerInside)
        applyCursor();
}

void CocoaWindow::applyCursor() {
    const bool hide = _cursor == Cursor::Hidden;
    // [NSCursor hide] nests, so keep exactly one outstanding hide per window.
    if (hide != _cursorHidden) {
        _cursorHidden = hide;
        if (hide)
            [NSCursor hide];
        else
            [NSCursor unhide];
    }
    if (!hide)
        [nsCursor(_cursor) set];
}

void CocoaWindow::pointerLeft() {
    pointerInside = false;
    // A hidden cursor is ours only while it is over us.
    if (_cursorHidden) {
        _cursorHidden = false;
        [NSCursor unhide];
    }
    emit({.type = EventType::PointerLeave});
}

// ── text input ──────────────────────────────────────────────────────────────

void CocoaWindow::setTextInput(const TextInputState &s) {
    const bool caretMoved = std::memcmp(&s.caret, &textInput.caret, sizeof s.caret) != 0;
    textInput             = s;
    if (!s.enabled)
        [view discardComposition];
    else if (caretMoved)
        [view.inputContext invalidateCharacterCoordinates]; // move the candidate window
}

// ── painting ────────────────────────────────────────────────────────────────

void CocoaWindow::viewResized() {
    const Size s = size();
    if (s.w == _lastSize.w && s.h == _lastSize.h)
        return;
    _lastSize = s;
    emit({.type = EventType::Resized});
    if (view.inLiveResize) {
        // AppKit shows the frame as soon as this returns; paint now so the
        // user drags a window with content, not a stale top-left image.
        framePending = false;
        emit({.type = EventType::Frame});
    } else {
        requestFrame();
    }
}

void CocoaWindow::backingChanged() {
    const double s = window.backingScaleFactor;
    if (s == _lastScale)
        return;
    _lastScale               = s;
    view.layer.contentsScale = s;
    emit({.type = EventType::Resized});
    requestFrame();
}

void CocoaWindow::requestFrame() {
    if (!framePending) {
        framePending     = true;
        frameRequestedAt = CFAbsoluteTimeGetCurrent();
    }
    startFrameClock();
}

void CocoaWindow::startFrameClock() {
    if (@available(macOS 14.0, *))
        ((CADisplayLink *)_displayLink).paused = NO;
    // The display link does not tick for a window that is not on a live
    // display (screen asleep, locked session) or before macOS 14; the
    // fallback timer covers those so a requested Frame always arrives.
    app->scheduleFrameFallback(frameRequestedAt + kFrameFallback);
}

void CocoaWindow::deliverFrameIfDue() {
    if (@available(macOS 14.0, *))
        ((CADisplayLink *)_displayLink).paused = YES; // until the next requestFrame
    if (!framePending || !window.isVisible)
        return;
    framePending = false;
    emit({.type = EventType::Frame});
    app->noteWork();
}

Canvas CocoaWindow::beginPaint() {
    const double s = scale();
    const Size   l = size();
    const int    w = std::max(1, int(std::lround(l.w * s))),
                 h = std::max(1, int(std::lround(l.h * s)));
    if (w != _pw || h != _ph || _back.empty()) {
        // Keep what fits of the old frame: undamaged pixels must survive.
        std::vector<uint32_t> nb(size_t(w) * h, 0xff000000);
        for (int y = 0; y < std::min(h, _ph); ++y)
            std::memcpy(&nb[size_t(y) * w], &_back[size_t(y) * _pw], size_t(std::min(w, _pw)) * 4);
        _back.swap(nb);
        _pw = w;
        _ph = h;
    }
    return {_back.data(), _pw, _ph, _pw, s};
}

void CocoaWindow::endPaint(const std::vector<Rect> &) {
    if (_back.empty())
        return;
    @autoreleasepool {
        // The compositor may read the image long after this returns, so it
        // gets its own copy; _back stays ours for the next partial repaint.
        // (Follow-up: IOSurface double-buffering would drop this memcpy.)
        CFDataRef data = CFDataCreate(
            nullptr, reinterpret_cast<const UInt8 *>(_back.data()), CFIndex(_back.size() * 4)
        );
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CFRelease(data);
        CGColorSpaceRef cs  = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef      img = CGImageCreate(
            size_t(_pw),
            size_t(_ph),
            8,
            32,
            size_t(_pw) * 4,
            cs,
            CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little,
            provider,
            nullptr,
            false,
            kCGRenderingIntentDefault
        );
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        [CATransaction begin];
        [CATransaction setDisableActions:YES]; // no implicit cross-fade
        view.layer.contentsScale = scale();
        view.layer.contents      = (__bridge id)img;
        [CATransaction commit];
        if (presented)
            CGImageRelease(presented);
        presented = img;
    }
}

// ── attention ───────────────────────────────────────────────────────────────

void CocoaWindow::requestAttention() {
    // Dock bouncing is per app, not per window, and does nothing for the
    // active app; AppKit cancels the request itself when the app activates.
    // Informational = one bounce, not the critical "bounce until noticed"
    // meant for alerts.
    if (window.isKeyWindow || NSApp.isActive)
        return;
    [NSApp requestUserAttention:NSInformationalRequest];
}

// ── drag source ─────────────────────────────────────────────────────────────

bool CocoaApp::startDrag(Window &source, const DragDesc &d) {
    auto    &w  = static_cast<CocoaWindow &>(source);
    NSEvent *ev = w.lastMouseEvent;
    if (!ev || w.dragging || d.items.empty())
        return false; // not inside a press, or a session already runs
    // AppKit's drag loop polls the hardware button state and ends the drag
    // (dropping wherever the pointer is) on its first event once no button
    // is physically down — measured: a session begun from a press the user
    // has already released, or a synthetic one, ends within ~15 ms. There is
    // no drag to run then, so say so instead of reporting a phantom drop.
    if (!(NSEvent.pressedMouseButtons & 1))
        return false;
    @autoreleasepool {
        NSArray<NSPasteboardItem *> *writers = pasteboardItems(d.items);
        // Image::scale physical pixels per point (a 2x drag image stays
        // crisp on Retina); the window's own scale does not enter into it.
        const double                 s       = d.image.scale > 0 ? d.image.scale : 1.0;
        NSImage                     *image   = nil;
        NSSize                       size    = NSMakeSize(32, 32);
        if (!d.image.empty()) {
            size  = NSMakeSize(d.image.width / s, d.image.height / s);
            image = nsImage(d.image, size);
        }
        if (!image) {
            // The OS default: a generic document icon, grabbed at its centre.
            image      = [NSWorkspace.sharedWorkspace iconForContentType:UTTypeItem];
            image.size = size;
        }
        const Point  hot   = d.image.empty() ? Point{size.width / 2, size.height / 2} : d.hotspot;
        const Point  at    = w.viewPoint(ev);
        // PlatView is flipped, so the frame's origin is its top-left corner.
        const NSRect frame = NSMakeRect(at.x - hot.x, at.y - hot.y, size.width, size.height);
        NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
        for (NSPasteboardItem *pbi in writers) {
            NSDraggingItem *di = [[NSDraggingItem alloc] initWithPasteboardWriter:pbi];
            // Only the first item shows the app's image; further URIs ride along.
            [di setDraggingFrame:frame contents:items.count == 0 ? image : nil];
            [items addObject:di];
        }
        w.dragActions              = d.actions;
        w.dragging                 = true;
        NSDraggingSession *session = [w.view beginDraggingSessionWithItems:items
                                                                     event:ev
                                                                    source:w.view];
        if (!session) {
            w.dragging = false;
            return false;
        }
        session.animatesToStartingPositionsOnCancelOrFail = YES;
        session.draggingFormation                         = NSDraggingFormationNone;
    }
    return true;
}

} // namespace plat::cocoa
