// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#include "research_host.h"

@interface ResearchController : UIViewController
@end
@implementation ResearchController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UITextView* text = [[UITextView alloc] initWithFrame:CGRectZero];
    text.translatesAutoresizingMaskIntoConstraints = NO;
    text.editable = NO;
    text.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    text.adjustsFontForContentSizeCategory = YES;
    text.backgroundColor = UIColor.systemBackgroundColor;
    (void)switch_aot_self_test();
    text.text = [NSString stringWithFormat:
        @"iHorizon\n\n%s\n\n"
         "This is a separate private-development diagnostic app, not Lattice.\n\n"
         "This milestone does not boot a game. Suyu's HLE, graphics, audio, "
         "input and owner-content loading still need iOS integration.\n\n"
         "No JIT entitlement or dynamic game-code loader is requested.",
         switch_aot_report()];
    [self.view addSubview:text];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
        [text.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
        [text.topAnchor constraintEqualToAnchor:safe.topAnchor constant:20],
        [text.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-20]]];
}
@end
@interface ResearchAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation ResearchAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [ResearchController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ResearchAppDelegate.class));
    }
}
