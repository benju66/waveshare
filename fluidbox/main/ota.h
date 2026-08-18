#pragma once

#include "esp_err.h"

// Over-the-air updates from the GitHub release feed (OTA_RELEASE_URL in
// config.h). The net task calls the check on its own schedule; on a version
// mismatch the new image is streamed into the spare OTA slot and the device
// reboots into it. Rollback is armed in the bootloader: an image that dies
// before proving it can reach Wi-Fi gets rolled back automatically.

// Compares the published image's version against the running one and
// applies it if different. Blocks the calling task for the duration of the
// download (up to a couple of minutes). Reboots on success; returns on
// "already up to date" or error.
esp_err_t ota_check_and_apply(void);

// Marks the running image good, cancelling a pending bootloader rollback.
// Call once the app has proven itself (first successful IP acquisition).
void ota_mark_healthy(void);
