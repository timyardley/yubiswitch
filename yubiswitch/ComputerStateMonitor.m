//
//  ComputerStateMonitor.m
//  yubiswitch
//
//  Created by Angelo Failla on 8/29/15.
//  Copyright (c) 2015 Angelo Failla. All rights reserved.
//

#import "ComputerStateMonitor.h"
#import <Foundation/NSDistributedNotificationCenter.h>
#include <CoreGraphics/CGSession.h>

@interface ComputerStateMonitor ()
- (instancetype)initWithYubiKey:(YubiKey *)yubikey
              sessionDictionary:(NSDictionary *)sessionDictionary;
@end

@implementation ComputerStateMonitor

- (id)initWithYubiKey:(YubiKey *)yubikey {
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [self initWithYubiKey:yubikey sessionDictionary:session];
}

- (instancetype)initWithYubiKey:(YubiKey *)yubikey
              sessionDictionary:(NSDictionary *)sessionDictionary {
    self = [super init];
    if (self == nil || yubikey == nil) return nil;
    yk = yubikey;
    // The lock key is absent from some session dictionaries. Preserve the
    // key for login until an explicit unlock event resolves unknown state.
    id locked = sessionDictionary[@"CGSSessionScreenIsLocked"];
    screenLocked = ![locked isKindOfClass:[NSNumber class]] || [locked boolValue];

    NSDistributedNotificationCenter *center =
        [NSDistributedNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(receive:)
                  name:@"com.apple.screenIsLocked" object:nil];
    [center addObserver:self selector:@selector(receive:)
                  name:@"com.apple.screenIsUnlocked" object:nil];
    return self;
}

-(void) receive: (NSNotification*) notification {
    if ([[notification name] isEqualToString:@"com.apple.screenIsLocked"]) {
        screenLocked = YES;
    } else if ([[notification name] isEqualToString:@"com.apple.screenIsUnlocked"]) {
        screenLocked = NO;
    } else {
        return;
    }
    lockStateFromNotification = YES;
    BOOL activated =
        [[NSUserDefaults standardUserDefaults] boolForKey:@"disableAtLockSleep"];

    if (!activated) {
        return;
    }

    NSLog(@"Received notification %s", [[notification name] UTF8String]);

    if ([[notification name]  isEqual:@"com.apple.screenIsLocked"]) {
        NSLog(@"Screen is locked, renabling yubikey");
        [yk enable];
    }

    if ([[notification name]  isEqual:@"com.apple.screenIsUnlocked"]) {
        NSLog(@"Screen is unlocked, disabling yubikey");
        [yk disable];
    }
}

- (BOOL)isScreenLocked {
    return screenLocked;
}

- (BOOL)allowsAutomaticDisable {
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [self allowsAutomaticDisableWithSessionDictionary:session];
}

- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary {
    if (![[NSUserDefaults standardUserDefaults] boolForKey:@"disableAtLockSleep"])
        return YES;
    id locked = sessionDictionary[@"CGSSessionScreenIsLocked"];
    return (!lockStateFromNotification || !screenLocked) &&
        [locked isKindOfClass:[NSNumber class]] &&
        ![locked boolValue];
}

@end
