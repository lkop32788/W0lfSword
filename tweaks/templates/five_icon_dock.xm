// five_icon_dock.xm - W0lfSword tweak template (catalog id: five_icon_dock, K3.6)
// Allows 5 icons in the dock row on iOS 17-26 SpringBoard.
//
// Why this is a live dock re-config and not one %hook on SBIconListView: the
// dock's column count is not a constant in SpringBoardHome. It is the dock icon
// list MODEL's grid size plus the dock layout CONFIG's portrait column count,
// both live objects, and the class that owns the layout math moves between iOS
// versions. Naming a class offline would be a guess; the chain used here
// (SBIconController sharedInstance -> iconManager -> dockListView -> model
// setGridSize: + layout->layoutConfiguration setNumberOfPortraitColumns:) is the
// one this repo already drives against a live device - kexploit/RemoteCall's
// sbtweak calls exactly these accessors from Filza (sbtweak/sbtweak.m,
// sbt_dock_set_columns) and lara's dock feature uses the same runtime API on
// iOS 17-18.x and 26.0.x. Inside SpringBoard the same calls are plain
// objc_msgSend: no remote scratch page, no thread hijack.
//
// Three layers, in the order of what each one guarantees:
//
//   1. apply once, as soon as the dock objects exist. %ctor runs while
//      SpringBoard is still starting, so the first attempt usually finds
//      nothing; a bounded ladder retries 10x at 1s and stops at the first
//      success, so the ladder always terminates.
//   2. identity-guarded setter hooks. The model's class and the config object
//      come from the LIVE objects (object_getClass), never from a guessed class
//      name, and the swizzle rewrites the value only when
//      `self == the dock object we resolved`. Every other list model / layout
//      config is passed through to the original untouched, so a SpringBoard
//      relayout that re-derives the dock's grid keeps 5 columns instead of
//      silently reverting to 4.
//   3. one re-check 6s after the first success, which re-applies only if
//      something (e.g. the end-of-launch layout pass) put the stock value back.
//
// Every step is guarded: classes are looked up by name at runtime, selectors
// with respondsToSelector:, swizzle targets with class_getInstanceMethod. A
// renamed accessor logs the stage it failed at and leaves the stock dock alone
// instead of crashing SpringBoard. The cached pointers are only ever COMPARED,
// never dereferenced, so a recreated dock model degrades to "dock stays stock
// 4 columns" rather than a dangling-pointer crash.
//
// Safe revert: this tweak writes NO file and edits no system asset - it only
// changes live dock state, so a respring (or
// `./W0lfSword tweaks remove five_icon_dock` + respring) restores stock. There
// is no backup to keep.
//
// NOTE: build-verified only (host). The 4 -> 5 column change needs a
// jailbroken device and a respring; these are the lines to read back (unified
// log, or `idevicesyslog | grep W0lfSword`):
//   [W0lfSword] five_icon_dock: dock columns 4 -> 5 (read back 5)
//   [W0lfSword] five_icon_dock: dock grid setter guard installed (was 4 columns)
// If the ladder logs "dock objects not found after 10 tries", the accessor
// chain moved on this iOS version - check `sbtweak` on the same device and
// update both together.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

#define W0LF_DOCK_COLUMNS   5
#define W0LF_DOCK_ATTEMPTS  10
#define W0LF_DOCK_RETRY_S   1.0
#define W0LF_DOCK_RECHECK_S 6.0

// The dock objects we resolved. Pointer identity only - see layer 2 above.
static id gDockModel = nil;
static id gDockConfig = nil;

// Original implementations captured by the swizzles (NULL = no hook installed).
static IMP gOrigSetGridSize = NULL;
static IMP gOrigSetPortraitColumns = NULL;

// Defined with the apply path below, used by it for the read-back verdict.
static int W0lfDockCurrentColumns(void);

// Dock stage logging - one prefix, so a log read shows the whole sequence
// without dragging in unrelated SpringBoard chatter.
static void W0lfDockLog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void W0lfDockLog(NSString *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:args];
    va_end(args);
    NSLog(@"[W0lfSword] five_icon_dock: %@", msg);
    [msg release];
}

// Guarded 0-argument object message: returns nil instead of crashing when the
// receiver is gone or the accessor was renamed.
static id W0lfDockMsg(id obj, const char *selName) {
    if (!obj) return nil;
    SEL sel = sel_registerName(selName);
    if (!sel || ![obj respondsToSelector:sel]) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(obj, sel);
}

// The grid value with only the column field replaced. Every other bit is kept:
// the value is a packed columns+rows pair and we do not know its width from
// here (a packed 32-bit value and an 8-byte pair are both plausible), so
// masking it wider than the field would silently zero the row bits. The setter
// consumes w0 either way, so preserving the rest can only help. This is the one
// deliberate difference from sbtweak, which masks the read value to 32 bits.
static unsigned long long W0lfDockWidenGrid(unsigned long long grid) {
    return (grid & ~0xFFFFULL) | (W0LF_DOCK_COLUMNS & 0xFFFFULL);
}

#pragma mark - layer 2: identity-guarded setters

// Runs for EVERY icon list model once installed (a shared class may own the
// method); only the dock model is rewritten, everything else passes through.
// Pointer comparison, no dereference.
static void W0lfDockSetGridSizeHook(id self, SEL _cmd, unsigned long long grid) {
    if (!gOrigSetGridSize) return;
    unsigned long long out = (self == gDockModel) ? W0lfDockWidenGrid(grid) : grid;
    ((void (*)(id, SEL, unsigned long long))gOrigSetGridSize)(self, _cmd, out);
}

static void W0lfDockSetPortraitColumnsHook(id self, SEL _cmd, unsigned long long columns) {
    if (!gOrigSetPortraitColumns) return;
    unsigned long long out = (self == gDockConfig) ? (unsigned long long)W0LF_DOCK_COLUMNS : columns;
    ((void (*)(id, SEL, unsigned long long))gOrigSetPortraitColumns)(self, _cmd, out);
}

// Returns the previous implementation, or NULL when the method does not exist
// (renamed accessor -> caller logs it and stays a pass-through).
static IMP W0lfDockInstallSetterHook(id obj, const char *selName, IMP replacement) {
    if (!obj) return NULL;
    Class cls = object_getClass(obj);
    if (!cls) return NULL;
    SEL sel = sel_registerName(selName);
    Method m = sel ? class_getInstanceMethod(cls, sel) : NULL;
    if (!m) return NULL;
    return method_setImplementation(m, replacement);
}

#pragma mark - layer 1: resolve + apply

// Resolves the dock chain. NO with *stage naming the step that failed, so the
// ladder log says which accessor to re-check.
static BOOL W0lfDockResolve(id *outDock, id *outModel, id *outConfig, NSString **stage) {
    Class ctrlClass = objc_getClass("SBIconController");
    if (!ctrlClass) { *stage = @"SBIconController missing"; return NO; }

    id ctrl = W0lfDockMsg((id)ctrlClass, "sharedInstance");
    if (!ctrl) { *stage = @"SBIconController sharedInstance returned nil"; return NO; }

    id mgr = W0lfDockMsg(ctrl, "iconManager");
    id dock = mgr ? W0lfDockMsg(mgr, "dockListView") : nil;
    if (!dock) dock = W0lfDockMsg(ctrl, "dockListView");
    if (!dock) { *stage = @"dockListView unreachable from iconManager and controller"; return NO; }

    id model = W0lfDockMsg(dock, "model");
    if (!model) model = W0lfDockMsg(dock, "iconListModel");

    id cfg = nil;
    id layout = W0lfDockMsg(dock, "layout");
    if (layout) cfg = W0lfDockMsg(layout, "layoutConfiguration");

    *outDock = dock;
    *outModel = model;
    *outConfig = cfg;
    return YES;
}

// Applies the widening to the resolved dock. NO = could not (the ladder retries).
static BOOL W0lfDockApply(void) {
    id dock = nil, model = nil, config = nil;
    NSString *stage = nil;
    if (!W0lfDockResolve(&dock, &model, &config, &stage)) {
        W0lfDockLog(@"dock not ready yet (%@)", stage);
        return NO;
    }

    int oldColumns = -1;
    if (model) {
        SEL gridSel = sel_registerName("gridSize");
        if ([model respondsToSelector:gridSel]) {
            unsigned long long oldGrid = ((unsigned long long (*)(id, SEL))objc_msgSend)(model, gridSel);
            oldColumns = (int)(oldGrid & 0xFFFFULL);
            if (oldColumns != W0LF_DOCK_COLUMNS) {
                SEL setSel = sel_registerName("setGridSize:");
                if ([model respondsToSelector:setSel]) {
                    ((void (*)(id, SEL, unsigned long long))objc_msgSend)(model, setSel,
                                                                         W0lfDockWidenGrid(oldGrid));
                }
            }
        }
    }

    if (config) {
        SEL setCols = sel_registerName("setNumberOfPortraitColumns:");
        if ([config respondsToSelector:setCols]) {
            ((void (*)(id, SEL, unsigned long long))objc_msgSend)(config, setCols,
                                                                 (unsigned long long)W0LF_DOCK_COLUMNS);
        }
    }

    // Layer 2: remember the live objects and intercept later writes to them.
    gDockModel = model;
    gDockConfig = config;
    if (!gOrigSetGridSize && model) {
        gOrigSetGridSize = W0lfDockInstallSetterHook(model, "setGridSize:", (IMP)W0lfDockSetGridSizeHook);
        if (gOrigSetGridSize) W0lfDockLog(@"dock grid setter guard installed (was %d columns)", oldColumns);
        else W0lfDockLog(@"dock grid setter not swizzled (method not found) - apply only");
    }
    if (!gOrigSetPortraitColumns && config) {
        gOrigSetPortraitColumns = W0lfDockInstallSetterHook(config, "setNumberOfPortraitColumns:",
                                                            (IMP)W0lfDockSetPortraitColumnsHook);
        if (gOrigSetPortraitColumns) W0lfDockLog(@"dock layout config guard installed");
        else W0lfDockLog(@"dock layout config not swizzled (method not found) - apply only");
    }

    // Cheap and it makes the change visible without waiting for the next
    // natural relayout.
    SEL needsLayout = sel_registerName("setNeedsLayout");
    if ([dock respondsToSelector:needsLayout]) {
        ((void (*)(id, SEL))objc_msgSend)(dock, needsLayout);
    }

    // Read the column field back instead of trusting "the call did not error":
    // on a device this is the line that says whether the dock actually took the
    // new value, and "-1" means the grid getter stopped answering.
    int after = W0lfDockCurrentColumns();
    if (oldColumns >= 0) W0lfDockLog(@"dock columns %d -> %d (read back %d)", oldColumns, W0LF_DOCK_COLUMNS, after);
    else W0lfDockLog(@"dock columns set to %d (grid unreadable)", W0LF_DOCK_COLUMNS);
    return YES;
}

// Current dock column count, or -1 when the chain is gone.
static int W0lfDockCurrentColumns(void) {
    id dock = nil, model = nil, config = nil;
    NSString *stage = nil;
    if (!W0lfDockResolve(&dock, &model, &config, &stage) || !model) return -1;
    SEL gridSel = sel_registerName("gridSize");
    if (![model respondsToSelector:gridSel]) return -1;
    unsigned long long grid = ((unsigned long long (*)(id, SEL))objc_msgSend)(model, gridSel);
    return (int)(grid & 0xFFFFULL);
}

static void W0lfDockLadder(int attempt) {
    if (W0lfDockApply()) {
        // Layer 3: one bounded re-check - the end-of-launch layout pass runs
        // after %ctor and can re-derive the stock grid.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(W0LF_DOCK_RECHECK_S * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            int cols = W0lfDockCurrentColumns();
            if (cols == W0LF_DOCK_COLUMNS) { W0lfDockLog(@"re-check: still %d columns", cols); return; }
            W0lfDockLog(@"re-check: columns are %d again, re-applying", cols);
            W0lfDockApply();
        });
        return;
    }
    if (attempt >= W0LF_DOCK_ATTEMPTS) {
        W0lfDockLog(@"dock objects not found after %d tries - dock left stock", W0LF_DOCK_ATTEMPTS);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(W0LF_DOCK_RETRY_S * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        W0lfDockLadder(attempt + 1);
    });
}

%ctor {
    W0lfDockLog(@"loaded - target %d dock columns", W0LF_DOCK_COLUMNS);
    // %ctor can run on a non-main thread and the dock is UI: hop to main first.
    dispatch_async(dispatch_get_main_queue(), ^{
        W0lfDockLadder(1);
    });
}
