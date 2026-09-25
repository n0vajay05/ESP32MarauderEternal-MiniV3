# Marauder Eternal Mini V3 1.15.6

This release fixes Funny SSID and Rick Roll beacon transmission and makes
Evil Portal deauthentication respect every selected AP while prioritizing
connected portal clients.

## Funny SSID and Rick Roll

- Generate correctly sized beacon frames with a distinct, stable BSSID for
  each preset SSID.
- Start on channel 1 in 2.4 GHz mode, including after a 5 GHz workflow.
- Wait for each transmission to complete before submitting another beacon,
  preventing transmit-buffer exhaustion and `ESP_ERR_NO_MEM` failures.
- Retain the current preset and back off when the driver rejects a frame.
- Show accepted transmissions and failures per second, with driver errors
  reported on screen and over USB serial.

## Evil Portal

- Preserve all selected AP targets when choosing the portal SSID and channel,
  through either the grouped menu or the USB `setap` command.
- Visit each selected BSSID in turn, including separate 2.4 GHz and 5 GHz
  radios advertising the same SSID.
- Wait for all queued frames to complete before returning to the portal channel.
- Defer other channels while clients are connected to the portal and resume
  those targets after the last client disconnects.
- Show selected AP counts by band and the number of temporarily deferred APs.
- Move **Select EP HTML File** from **WiFi > General** to
  **WiFi > Attacks > Evil Portal**. Back and file selection return to Evil Portal.

## Compatibility

- Hardware remains Marauder Mini V3 / ESP32-C5 with 8 MB flash and PSRAM.
- Arduino ESP32 core remains pinned to `3.3.4`; the partition layout remains
  `mini-v3-c5-8m-ota-v1`.
- The application updater writes both OTA slots and preserves saved settings
  and filesystem data. The merged factory image replaces the complete flash.
- The single radio serves one channel at a time. Driver transmission status
  does not prove that a remote client received a frame or disconnected.
- Flasher 1.3.5 packages firmware 1.15.6 using the updated release manifest.
