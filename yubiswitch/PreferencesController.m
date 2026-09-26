//  PreferencesController.m
//  yubiswitch

/*
 yubiswitch - enable/disable yubikey
 Copyright (C) 2013-2015  Angelo "pallotron" Failla <pallotron@freaknet.org>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import "PreferencesController.h"
#import <ServiceManagement/ServiceManagement.h>
#include <stdint.h>

static BOOL validHexID(NSString *text) {
  if (![text isKindOfClass:[NSString class]]) return NO;
  unsigned int value = 0;
  NSScanner *scanner = [NSScanner scannerWithString:text];
  return [scanner scanHexInt:&value] && [scanner isAtEnd] &&
         value > 0 && value <= UINT16_MAX;
}

@interface PreferencesController ()

@end

@implementation PreferencesController {
  SRShortcutValidator *_validator;
}

- (void)awakeFromNib {
  [super awakeFromNib];
  controller = [NSUserDefaultsController sharedUserDefaultsController];
  NSString *defaultPrefsFile =
      [[NSBundle mainBundle] pathForResource:@"DefaultPreferences"
                                      ofType:@"plist"];
  NSDictionary *defaultPrefs =
      [NSDictionary dictionaryWithContentsOfFile:defaultPrefsFile];
  [controller setInitialValues:defaultPrefs];
  [controller setAppliesImmediately:FALSE];
  BOOL startAtLogin =
      [[NSUserDefaults standardUserDefaults] boolForKey:@"startAtLogin"];
  [buttonOpenAtLogin setState:startAtLogin];
  [self.hotkeyrecorder bind:NSValueBinding
                   toObject:controller
                withKeyPath:@"values.hotkey"
                    options:nil];
}

- (void)addAppAsLoginItem {
  NSError *error = nil;
  if (![[SMAppService mainAppService] registerAndReturnError:&error]) {
    NSLog(@"Failed to register login item: %@", error);
  }
}

- (void)deleteAppFromLoginItem {
  NSError *error = nil;
  if (![[SMAppService mainAppService] unregisterAndReturnError:&error]) {
    NSLog(@"Failed to unregister login item: %@", error);
  }
}

- (IBAction)SetDefaultsButton:(id)sender {
  [controller revertToInitialValues:self];
  [buttonOpenAtLogin setState:
      [[[controller values] valueForKey:@"startAtLogin"] boolValue]];
}

- (IBAction)OKButton:(id)sender {
  if (![[self window] makeFirstResponder:nil] || ![controller commitEditing]) return;
  NSString *vendor = [[controller values] valueForKey:@"hotKeyVendorID"];
  NSString *product = [[controller values] valueForKey:@"hotKeyProductID"];
  if (!validHexID(vendor) || !validHexID(product)) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Invalid device filter"];
    [alert setInformativeText:@"Enter hexadecimal Vendor and Product IDs."];
    [alert runModal];
    return;
  }
  NSMutableDictionary *devicePreferences = [@{
    @"hotKeyVendorID": vendor,
    @"hotKeyProductID": product
  } mutableCopy];
  [[NSNotificationCenter defaultCenter]
      postNotificationName:@"changeDefaultsPrefs"
                    object:devicePreferences];
  if (![devicePreferences[@"applySucceeded"] boolValue]) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Could not apply device filter"];
    [alert setInformativeText:@"The key state could not be confirmed. Check its connection and try again."];
    [alert runModal];
    return;
  }
  [controller save:self];
  bool state = [[buttonOpenAtLogin selectedCell] state];
  BOOL previousState = [[NSUserDefaults standardUserDefaults]
                         boolForKey:@"startAtLogin"];
  if (state != previousState) {
    if (state) {
      [self addAppAsLoginItem];
    } else {
      [self deleteAppFromLoginItem];
    }
  }
  [[NSUserDefaults standardUserDefaults] setBool:state forKey:@"startAtLogin"];
  [[self window] close];
}

- (IBAction)CancelButton:(id)sender {
  [controller revert:self];
  [[self window] close];
}

@end
