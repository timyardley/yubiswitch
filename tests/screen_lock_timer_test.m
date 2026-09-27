#import <Foundation/Foundation.h>
#import "../yubiswitch/ComputerStateMonitor.h"
#include <assert.h>
#include <stdio.h>

@interface ComputerStateMonitor (TestAccess)
- (void)receive:(NSNotification *)notification;
- (BOOL)isScreenLocked;
- (BOOL)allowsAutomaticDisable;
- (instancetype)initWithYubiKey:(YubiKey *)key
              sessionDictionary:(NSDictionary *)sessionDictionary;
- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary;
@end

@interface FakeKey : NSObject
@property(nonatomic) NSUInteger enables;
@property(nonatomic) NSUInteger disables;
- (BOOL)enable;
- (BOOL)disable;
@end

@implementation FakeKey
- (BOOL)enable { self.enables++; return YES; }
- (BOOL)disable { self.disables++; return YES; }
@end

int main(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @YES}
                   forName:NSArgumentDomain];
        FakeKey *key = [FakeKey new];
        ComputerStateMonitor *initiallyLocked = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @YES}];
        assert(![initiallyLocked allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        ComputerStateMonitor *initiallyUnlocked = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        assert([initiallyUnlocked allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        ComputerStateMonitor *unknown = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key sessionDictionary:nil];
        assert(![unknown allowsAutomaticDisableWithSessionDictionary:nil]);
        ComputerStateMonitor *monitor = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        assert(![monitor isScreenLocked]);
        assert([monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        // A lock missed during startup must still block the timer callback.
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:nil]);
        [monitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsLocked" object:nil]];
        assert([monitor isScreenLocked]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        assert(key.enables == 1);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @NO}
                   forName:NSArgumentDomain];
        assert([monitor allowsAutomaticDisableWithSessionDictionary:nil]);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @YES}
                   forName:NSArgumentDomain];
        [monitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsUnlocked" object:nil]];
        assert(![monitor isScreenLocked]);
        assert([monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        assert(key.disables == 1);
        puts("screen_lock_timer_test: lock state tracked");
    }
    return 0;
}
