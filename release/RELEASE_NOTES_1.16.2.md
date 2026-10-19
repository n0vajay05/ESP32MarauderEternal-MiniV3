# Marauder Eternal Mini V3 1.16.2

Firmware 1.16.2 and desktop Flasher 1.5.0 add keyboard control to the USB Screen
Mirror introduced in [1.16.1](RELEASE_NOTES_1.16.1.md). Open **Screen Mirror**,
click the mirrored screen, and use **arrow keys** for the directional buttons
and **C** for Center/select.

## Keyboard controls

- Enable keyboard input only while screen sharing is active and the viewer
  has focus. The Mini's physical buttons remain usable.
- Preserve button presses, releases, held-arrow scrolling, and C hold behavior
  across menus, dashboards, and games through shared button handling.
- Release virtual buttons when focus leaves the viewer, Save PNG opens, or the
  viewer disconnects. A 600 ms input lease releases stale keys if updates stop.
- Tie input to the active mirror session; reject input outside that session.
  Reconnect with all keys released.
- Preserve short taps, ignore keyboard autorepeat as additional button presses,
  and handle X11 release/press repeat pairs. Refresh input during USB retries
  so a focus change cannot keep resending an old held-key state.
- Leave desktop modifier shortcuts available. F11 and Esc retain the viewer's
  fullscreen controls.
- Keep firmware with the original mirror support viewable using physical
  controls. Keyboard support is advertised as `screen-mirror-input-v1`.

Keyboard state travels in the existing USB mirror requests. The Mini retains
the single 32 KiB screen capture buffer and bounded row packets. The viewer
keeps its fullscreen, PNG export, and reconnect features.

## Release packages

- Update firmware identity, versioned release filenames, manifests, checksums,
  desktop package identity, documentation, and GitHub Actions artifact versions.
- Flasher 1.5.0 includes firmware 1.16.2. Linux and Windows packages build on
  their respective GitHub Actions runners with package checksums.
- Hardware remains Marauder Mini V3 / ESP32-C5 with 8 MB flash and PSRAM,
  Arduino ESP32 core 3.3.4, and partition layout `mini-v3-c5-8m-ota-v1`.
- Application updates write both OTA slots and preserve settings and filesystem
  data. The merged factory image replaces the complete flash.

All enhancements documented in [1.16.0](RELEASE_NOTES_1.16.0.md) and the USB
screen mirror from [1.16.1](RELEASE_NOTES_1.16.1.md) remain included.

## Validation

- Build the firmware and pass all native regression checks and 44 desktop tests.
- Pass source and bundled Linux GUI checks, including keyboard bindings, held
  keys, focus loss, desktop shortcuts, and closing with a held key. Pass the
  standalone flasher self-test and verify its bundled firmware digest.
- Flash both application slots and verify both hashes. The Mini reports
  `v1.16.2` and passes isolated FindMy diagnostics and eight mirror input/session
  checks, including inactive-session rejection, key expiry, legacy viewing
  polls, clean disconnect, reconnect, and abrupt viewer loss.
- The keyboard implementation also passed live viewer navigation and held-key
  checks on the Mini, including operating Snake and holding C to exit Blackjack.
  Those checks preceded this version update; firmware behavior is unchanged
  from that verified keyboard build apart from its version identity.
- Build and verify the Linux executable locally. Windows packaging is prepared
  in GitHub Actions and requires its Windows runner.
