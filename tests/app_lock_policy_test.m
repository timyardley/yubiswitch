#import <AppKit/AppKit.h>
#import "../yubiswitch/AppDelegate.h"
#include <assert.h>
#include <stdio.h>

@interface AppDelegate (TestAccess)
- (BOOL)applyInitialKeyPolicy;
@end

@interface FakeKey : NSObject
@property(nonatomic) NSUInteger enables;
@property(nonatomic) NSUInteger disables;
@property(nonatomic) BOOL disabled;
- (BOOL)enable;
- (BOOL)disable;
- (BOOL)isStateKnown;
- (BOOL)state;
@end

@implementation FakeKey
- (BOOL)enable { self.enables++; self.disabled = NO; return YES; }
- (BOOL)disable { self.disables++; self.disabled = YES; return YES; }
- (BOOL)isStateKnown { return YES; }
- (BOOL)state { return self.disabled; }
@end

@interface FakeMonitor : NSObject
@property(nonatomic) BOOL allowsDisable;
- (BOOL)allowsAutomaticDisable;
@end

@implementation FakeMonitor
- (BOOL)allowsAutomaticDisable { return self.allowsDisable; }
@end

@interface TestDelegate : AppDelegate
- (void)useKey:(FakeKey *)key monitor:(FakeMonitor *)monitor;
- (NSTimer *)pendingDisableTimer;
@end

@implementation TestDelegate
- (void)useKey:(FakeKey *)key monitor:(FakeMonitor *)monitor {
    yk = (YubiKey *)key;
    state_monitor = (ComputerStateMonitor *)monitor;
}
- (NSTimer *)pendingDisableTimer { return reDisableTimer; }
- (void)notify:(NSString *)message { (void)message; }
@end

int main(void) {
    @autoreleasepool {
        TestDelegate *delegate = [TestDelegate new];
        FakeKey *key = [FakeKey new];
        FakeMonitor *monitor = [FakeMonitor new];
        [delegate useKey:key monitor:monitor];

        // Locked or unknown at launch: preserve the key for login.
        monitor.allowsDisable = NO;
        assert([delegate applyInitialKeyPolicy]);
        assert(key.enables == 1 && key.disables == 0);

        // An unlocked launch follows the normal disabled policy.
        monitor.allowsDisable = YES;
        assert([delegate applyInitialKeyPolicy]);
        assert(key.enables == 1 && key.disables == 1);

        // A timed shutoff must remain pending while lock state is uncertain.
        key.disabled = NO;
        monitor.allowsDisable = NO;
        [delegate reDisableYK];
        assert(key.disables == 1);
        NSTimer *retry = [delegate pendingDisableTimer];
        assert(retry != nil && [retry isValid]);

        // Once the lock state allows it, the pending timer shuts the key off.
        monitor.allowsDisable = YES;
        [retry fire];
        assert(key.disables == 2 && key.disabled);
        assert([delegate pendingDisableTimer] == nil);
        monitor.allowsDisable = NO;
        [delegate reDisableYK];
        assert(key.disables == 2);
        assert([delegate pendingDisableTimer] == nil);
        puts("app_lock_policy_test: startup and timer recovery passed");
    }
    return 0;
}
