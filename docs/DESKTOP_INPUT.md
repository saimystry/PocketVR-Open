# Xbox 360 controller and mouse input

Updated 2026-11-03: Xbox/XInput and local mouse buttons now work together.
Xbox detection is enabled by default; an absent/disconnected pad leaves the
mouse available. For a game that already supports Xbox input directly, use that
native support. Use this helper for virtual VR controllers or gaze-click menus.
Avoid mapping the same action through both native and translated gamepad inputs
if the title processes them twice. Second-phone controllers are deferred.

Enable **Allow controller while a game or another window is in front** when a
game needs desktop focus. Only the controller may run in the background; mouse
clicks remain local to this window. This option resets to off when the helper
closes. **Ctrl+Alt+F8** pauses/resumes in this mode; the mode is unavailable if
another app owns that shortcut. **Escape** pauses both input sources. Choose
**Resume input** and release all controls before continuing. A held button cannot
carry over across pause, source changes or a controller reconnect.

Verification: an actual XInput controller is detected in slot 0. The window
layout, Pause button and foreground shortcut resume path were checked. Native
regressions cover combined holds, background permission, disconnected mouse
fallback and neutral rearming. Physical controller actions in individual
SteamVR titles remain user acceptance; detection does not establish gameplay.

Build the Windows targets and use the updated driver package. Restart SteamVR
after replacing a driver; registering a different build path also requires a
restart. The Android APK does not change for this feature.

In `pocketvr_control.exe`, choose **Mouse / controller input**, or run the sibling
`pocketvr_input.exe` directly. Connect the phone and enter headset view. Keep the
input window in the foreground, or opt into background controller mode above.
Release buttons/sticks once to arm input.

Mouse controls work with or without a connected Xbox controller:

| Control | VR action |
|---|---|
| Look with the headset | Aim the right virtual controller along head forward |
| Left mouse button | Right trigger/select |
| Right mouse button | Right grip |
| Middle mouse button | Application menu |
| Space | System/dashboard button |
| R | Recenter head yaw |
| Escape | Pause all translated input; choose Resume input to continue |

Clicks must be inside the input window; ordinary desktop clicks are not captured
globally. Dragged buttons remain captured until release. Switching applications
or minimizing releases input. Mouse movement does not rotate the headset or
control a Windows desktop cursor in VR. SteamVR/app bindings decide how to show
the controller pointer and interpret selection; this is not a separate dashboard
overlay. Some applications may require a binding adjustment.

For a wired controller, leave **Xbox 360 / XInput controller + mouse** checked. XInput is preferred; if no
XInput pad is detected, a Windows multimedia joystick fallback handles generic
USB pads. Discovery retries once per second while disconnected. It selects the
first available controller. Generic controllers vary: the default button/axis
layout below needs verification on the user's actual clone controller.

| Xbox control | Virtual input |
|---|---|
| Left/right stick | Left/right trackpad axes |
| D-pad | Left trackpad axes |
| LT / RT | Left/right analog trigger |
| A | Right trigger/select |
| LB / RB | Left/right grip |
| B | Right grip |
| X / Start | Left/right application menu |
| Y | Right system/dashboard button |
| Stick presses | Corresponding trackpad click |
| Back | Recenter |

Generic fallback uses buttons 1/8 for right trigger, 7 for left trigger, 5/6
for grips, 2 for right grip, 3/10 for menus, 4 for dashboard, 9 for recenter,
11/12 for trackpad clicks; X/Y and optional R/U axes plus POV supply sticks.
These are 1-based button numbers, not guaranteed Xbox labels. Windows joystick
calibration can be checked with `joy.cpl`. Runtime mapping editor and generic
analog-trigger calibration remain future work.

Two virtual controllers expose Vive-style trigger, grip, trackpad and menu
components. Mouse right-hand pose follows head forward with a small forward
offset; gamepad poses are head-relative approximations. Neither supplies real
independent hand tracking. Phone touch/second-phone controllers, haptics and
title-specific default bindings remain separate controller milestones.

The input window sends a latest-state snapshot over the existing current-user
named pipe from a dedicated worker. Slow device discovery/RPC does not run in
OpenVR, encoding or networking callbacks. The driver expires a missing update
after 250 ms, releases input on focus/transport/tracking loss and requires a
neutral sample before rearming. Unplugging a pad releases its state; plugging it
back in is detected without restarting SteamVR. Focus is required unless
background controller mode is explicitly enabled.

Verification must distinguish synthetic mapping/lease/pose tests from physical
controller and actual SteamVR dashboard acceptance. The user's wired controller
was not connected during implementation. No optical or gameplay acceptance is
implied by passing the Windows suite.

2026-11-03 verification: fresh Windows Debug build and all **94 CTest checks**
passed (including four new input tests), with installed dependency/SDK license
audit passing. Builds/logs are under `<build root>` and
`<build root>`.
The driver package is `home-input-debug\package\pocketvr`; the desktop programs
are `home-input-debug\pc\control\pocketvr_control.exe` and `pocketvr_input.exe`.
The ordinary branch/index and copied home work are retained; a separate local
`checkpoint/desktop-input-2026-11-03` branch records the integrated milestone.
Nothing is pushed automatically.
