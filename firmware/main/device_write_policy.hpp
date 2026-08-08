#pragma once

// HARDWARE SAFETY POLICY: Keep this flag explicit in each device build profile.
// Never change it to 1. ChatESP must not burn eFuses or enable another device
// feature that makes a permanent hardware change, including production builds.
#ifndef CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES
#error "A device build must explicitly disable irreversible writes"
#endif

#if CHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES != 0
#error "ChatESP policy forbids irreversible device writes"
#endif

// These ESP-IDF features can burn eFuses when an image first starts. Keep the
// checks in source code so a profile change fails before it can reach a device.
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
#error "NVS encryption is forbidden because its key setup can burn eFuses"
#endif

#if defined(CONFIG_SECURE_BOOT) && CONFIG_SECURE_BOOT
#error "Secure boot is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_SECURE_FLASH_ENC_ENABLED) && \
    CONFIG_SECURE_FLASH_ENC_ENABLED
#error "Flash encryption is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_FLASH_ENCRYPTION_ENABLED) && \
    CONFIG_FLASH_ENCRYPTION_ENABLED
#error "Flash encryption is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) && \
    CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#error "Application anti-rollback is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE) && \
    CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE
#error "Bootloader anti-rollback is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC) && \
    CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC
#error "Secure flash pseudo-round setup is forbidden because it can burn eFuses"
#endif

#if defined(CONFIG_SECURE_DISABLE_ROM_DL_MODE) && \
    CONFIG_SECURE_DISABLE_ROM_DL_MODE
#error "Permanent ROM download disable is forbidden"
#endif

#if defined(CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE) && \
    CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE
#error "Permanent secure ROM download mode is forbidden"
#endif
