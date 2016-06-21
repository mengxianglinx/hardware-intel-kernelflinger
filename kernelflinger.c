/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Author: Andrew Boie <andrew.p.boie@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <efi.h>
#include <efiapi.h>
#include <efilib.h>

#include <openssl/sha.h>

#include <fastboot.h>

#include "vars.h"
#include "lib.h"
#include "security.h"
#include "android.h"
#include "ux.h"
#include "options.h"
#include "power.h"
#include "targets.h"
#include "unittest.h"
#include "em.h"
#include "storage.h"
#include "version.h"
#include "trusty.h"
#ifdef HAL_AUTODETECT
#include "blobstore.h"
#endif
#include "oemvars.h"
#include "silentlake.h"
#include "slot.h"

/* Ensure this is embedded in the EFI binary somewhere */
static const CHAR16 __attribute__((used)) magic[] = L"### kernelflinger ###";

/* Default max wait time for console reset in units of milliseconds if no EFI
 * variable is set for this platform.
 * You want this value as small as possible as this is added to
 * the boot time for EVERY boot */
#define EFI_RESET_WAIT_MS           200

/* Interval in ms to check on startup for initial press of magic key */
#define DETECT_KEY_STALL_TIME_MS    1

/* How long (in milliseconds) magic key should be held to force
 * Fastboot mode */
#define FASTBOOT_HOLD_DELAY         (2 * 1000)

/* Magic key to enter fastboot mode or revovery console */
#define MAGIC_KEY          EV_DOWN

/* If we find this in the root of the EFI system partition, unconditionally
 * enter Fastboot mode */
#define FASTBOOT_SENTINEL         L"\\force_fastboot"

/* BIOS Capsule update file */
#define FWUPDATE_FILE             L"\\BIOSUPDATE.fv"

/* Crash event menu settings:
 * Maximum time between the first and the last watchdog reset.  If the
 * current difference exceeds this constant, the watchdog counter is
 * reset to zero. */
#define WATCHDOG_DELAY       (10 * 60)

static EFI_HANDLE g_disk_device;
static EFI_LOADED_IMAGE *g_loaded_image;
static VOID die(VOID) __attribute__ ((noreturn));

#if DEBUG_MESSAGES
static VOID print_rsci_values(VOID)
{
        enum wake_sources raw_wake_source = rsci_get_wake_source();
        enum reset_sources raw_reset_source = rsci_get_reset_source();
        enum reset_types raw_reset_type = rsci_get_reset_type();

        debug(L"wake_source = %s (0x%02hhx)",
              wake_source_string(raw_wake_source),
              raw_wake_source);
        debug(L"reset_source = %s (0x%02hhx)",
              reset_source_string(raw_reset_source),
              raw_reset_source);
        debug(L"reset_type = %s (0x%02hhx)",
              reset_type_string(raw_reset_type),
              raw_reset_type);
        if (raw_reset_source == RESET_PLATFORM_SPECIFIC)
                debug(L"reset_extra_info = 0x%08hhx", rsci_get_reset_extra_info());
}
#endif


static enum boot_target check_fastboot_sentinel(VOID)
{
        debug(L"checking ESP for %s", FASTBOOT_SENTINEL);
        if (file_exists(g_disk_device, FASTBOOT_SENTINEL))
                return FASTBOOT;
        return NORMAL_BOOT;
}


static enum boot_target check_magic_key(VOID)
{
        unsigned long i;
        EFI_STATUS ret = EFI_NOT_READY;
        EFI_INPUT_KEY key;
        unsigned long wait_ms = EFI_RESET_WAIT_MS;

        /* Some systems require a short stall before we can be sure there
         * wasn't a keypress at boot. Read the EFI variable which determines
         * that time for this platform */
        ret = get_efi_variable_long_from_str8(&loader_guid,
                                             MAGIC_KEY_TIMEOUT_VAR,
                                             &wait_ms);
        if (EFI_ERROR(ret)) {
                debug(L"Couldn't read timeout variable; assuming default");
        } else {
                if (wait_ms > 1000) {
                        debug(L"pathological magic key timeout, use default");
                        wait_ms = EFI_RESET_WAIT_MS;
                }
        }

        debug(L"Reset wait time: %d", wait_ms);

        /* Check for 'magic' key. Some BIOSes are flaky about this
         * so wait for the ConIn to be ready after reset */
        for (i = 0; i <= wait_ms; i += DETECT_KEY_STALL_TIME_MS) {
                ret = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2,
                                        ST->ConIn, &key);
                if (ret == EFI_SUCCESS || i == wait_ms)
                        break;
                uefi_call_wrapper(BS->Stall, 1, DETECT_KEY_STALL_TIME_MS * 1000);
        }

        if (EFI_ERROR(ret))
                return NORMAL_BOOT;

        debug(L"ReadKeyStroke: (%d tries) %d %d", i, key.ScanCode, key.UnicodeChar);
        if (ui_keycode_to_event(key.ScanCode) != MAGIC_KEY)
                return NORMAL_BOOT;

        if (ui_enforce_key_held(FASTBOOT_HOLD_DELAY, MAGIC_KEY))
                return FASTBOOT;

        return NORMAL_BOOT;
}


static enum boot_target check_bcb(CHAR16 **target_path, BOOLEAN *oneshot)
{
        EFI_STATUS ret;
        struct bootloader_message bcb;
        CHAR16 *target = NULL;
        enum boot_target t;
        CHAR8 *bcb_cmd;
        BOOLEAN dirty;

        *oneshot = FALSE;
        *target_path = NULL;

        ret = read_bcb(MISC_LABEL, &bcb);
        if (EFI_ERROR(ret)) {
                error(L"Unable to read BCB");
                t = NORMAL_BOOT;
                goto out;
        }

        dirty = bcb.status[0] != '\0';
        /* We own the status field; clear it in case there is any stale data */
        bcb.status[0] = '\0';
        bcb_cmd = (CHAR8 *)bcb.command;
        if (!strncmpa(bcb_cmd, (CHAR8 *)"boot-", 5)) {
                target = stra_to_str(bcb_cmd + 5);
                debug(L"BCB boot target: '%s'", target);
        } else if (!strncmpa(bcb_cmd, (CHAR8 *)"bootonce-", 9)) {
                target = stra_to_str(bcb_cmd + 9);
                bcb_cmd[0] = '\0';
                dirty = TRUE;
                debug(L"BCB oneshot boot target: '%s'", target);
                *oneshot = TRUE;
        }

        if (dirty) {
                ret = write_bcb(MISC_LABEL, &bcb);
                if (EFI_ERROR(ret))
                        error(L"Unable to update BCB contents!");
        }

        if (!target) {
                t = NORMAL_BOOT;
                goto out;
        }

        if (target[0] == L'\\') {
                UINTN len;

                if (!file_exists(g_disk_device, target)) {
                        error(L"Specified BCB file '%s' doesn't exist",
                                        target);
                        t = NORMAL_BOOT;
                        goto out;
                }

                len = StrLen(target);
                if (len > 4) {
                        *target_path = StrDuplicate(target);
                        if (!StrCmp(target + (len - 4), L".efi") ||
                                        !StrCmp(target + (len - 4), L".EFI")) {
                                t = ESP_EFI_BINARY;
                        } else {
                                t = ESP_BOOTIMAGE;
                        }
                        goto out;
                }
                error(L"BCB file '%s' appears to be malformed", target);
                t = NORMAL_BOOT;
                goto out;
        }

        t = name_to_boot_target(target);
        if (t != UNKNOWN_TARGET)
                goto out;

        error(L"Unknown boot target in BCB: '%s'", target);
        t = NORMAL_BOOT;

out:
        FreePool(target);
        return t;
}


static enum boot_target check_loader_entry_one_shot(VOID)
{
        EFI_STATUS ret;
        CHAR16 *target;
        enum boot_target bt;

        debug(L"checking %s", LOADER_ENTRY_ONESHOT);
        target = get_efi_variable_str(&loader_guid, LOADER_ENTRY_ONESHOT);

        del_efi_variable(&loader_guid, LOADER_ENTRY_ONESHOT);

        if (!target)
                return NORMAL_BOOT;

        debug(L"target = %s", target);
        bt = name_to_boot_target(target);
        if (bt == UNKNOWN_TARGET) {
                if (!StrCmp(target, L"dm-verity device corrupted")) {
                        debug(L"Reboot was triggered by dm-verity module\
 because partition is corrupted");
                        ret = slot_set_verity_corrupted(TRUE);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Failed to set the active\
 slot verity eio flag");
                } else
                        error(L"Unknown oneshot boot target: '%s'", target);
                bt = NORMAL_BOOT;
        } else if (bt == CHARGER && !get_off_mode_charge()) {
                debug(L"Off mode charge is not set, powering off.");
                bt = POWER_OFF;
        }

        FreePool(target);
        return bt;
}

static BOOLEAN reset_is_due_to_watchdog_or_panic()
{
        static enum reset_sources WATCHDOG_RESET_SOURCES[] = {
                RESET_KERNEL_WATCHDOG,
                RESET_SECURITY_WATCHDOG,
                RESET_PMIC_WATCHDOG,
                RESET_EC_WATCHDOG
        };
        enum reset_sources reset_source;
        UINTN i;

        reset_source = rsci_get_reset_source();
        for (i = 0; i < ARRAY_SIZE(WATCHDOG_RESET_SOURCES); i++)
                if (reset_source == WATCHDOG_RESET_SOURCES[i]) {
                        debug(L"Watchdog reset source = %d", reset_source);
                        return TRUE;
                }

        return is_reboot_reason(L"kernel_panic") ||
                is_reboot_reason(L"watchdog");
}

/* If more than get_watchdog_counter_max() watchdog (or kernel panic)
 * resets in a row happened in less than WATCHDOG_DELAY seconds, the
 * crash event menu is displayed.  This menu informs the user of the
 * situation and let him choose which boot target he wants. */
static enum boot_target check_watchdog(VOID)
{
        EFI_STATUS ret;
        UINT8 counter;
        EFI_TIME time_ref, now;

        if (!get_crash_event_menu())
                return NORMAL_BOOT;

        ret = get_watchdog_status(&counter, &time_ref);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to get the watchdog status");
                return NORMAL_BOOT;
        }

        if (!reset_is_due_to_watchdog_or_panic()) {
                if (counter != 0) {
                        ret = reset_watchdog_status();
                        if (EFI_ERROR(ret)) {
                                efi_perror(ret, L"Failed to reset the watchdog status");
                                goto error;
                        }
                }
                return NORMAL_BOOT;
        }

#ifdef USER
        if (is_reboot_reason(L"shutdown")) {
                del_reboot_reason();
                return POWER_OFF;
        }
#endif

        ret = uefi_call_wrapper(RT->GetTime, 2, &now, NULL);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to get the current time");
                goto error;
        }

        if (counter > 0) {
                if (efi_time_to_ctime(&now) < efi_time_to_ctime(&time_ref) ||
                    efi_time_to_ctime(&now) - efi_time_to_ctime(&time_ref) > WATCHDOG_DELAY)
                        counter = 0;
        }

        if (counter == 0) {
                time_ref = now;
                ret = set_watchdog_time_reference(&now);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"Failed to set the watchdog time reference");
                        goto error;
                }
        }

        counter++;
        debug(L"Incrementing watchdog counter (%d)", counter);

        if (counter <= get_watchdog_counter_max()) {
                ret = set_watchdog_counter(counter);
                if (EFI_ERROR(ret))
                        efi_perror(ret, L"Failed to set the watchdog counter");
                goto error;
        }

        ret = reset_watchdog_status();
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Failed to reset the watchdog status");

        return ux_prompt_user_for_boot_target(CRASH_EVENT_CODE);

error:
        return NORMAL_BOOT;
}

static enum boot_target check_command_line(VOID)
{
        UINTN argc, pos;
        CHAR16 **argv;
        enum boot_target bt;

        bt = NORMAL_BOOT;

        if (EFI_ERROR(get_argv(g_loaded_image, &argc, &argv)))
                return NORMAL_BOOT;

        for (pos = 0; pos < argc; pos++) {
                debug(L"Argument %d: %s", pos, argv[pos]);

                if (!StrCmp(argv[pos], L"-f")) {
                        bt = FASTBOOT;
                        continue;
                }
#ifndef USER
                if (!StrCmp(argv[pos], L"-U")) {
                        pos++;
                        unittest_main(pos >= argc ? NULL : argv[pos]);
                        FreePool(argv);
                        return EXIT_SHELL;
                }
#endif
                if (!StrCmp(argv[pos], L"-a")) {
                        pos++;
                        if (pos >= argc) {
                                error(L"-a requires a memory address");
                                goto out;
                        }

                        /* For compatibility...just ignore the supplied address
                         * and enter Fastboot mode */
                        bt = FASTBOOT;
                        continue;
                }

                /* If we get here the argument isn't recognized */
                if (pos == 0) {
                        /* EFI is inconsistent and only seems to populate the image
                         * name as argv[0] when called from a shell. Do nothing. */
                        continue;
                } else {
                        error(L"unexpected argument %s", argv[pos]);
                        goto out;
                }
        }

out:
        FreePool(argv);
        return bt;
}

static enum boot_target check_battery_inserted()
{
        enum wake_sources wake_source;

        if (!get_off_mode_charge())
                return NORMAL_BOOT;

        wake_source = rsci_get_wake_source();
        if (wake_source == WAKE_BATTERY_INSERTED)
                return POWER_OFF;

        return NORMAL_BOOT;
}

static enum boot_target check_charge_mode()
{
        enum wake_sources wake_source;

        if (!get_off_mode_charge())
                return NORMAL_BOOT;

        wake_source = rsci_get_wake_source();
        if ((wake_source == WAKE_USB_CHARGER_INSERTED) ||
            (wake_source == WAKE_ACDC_CHARGER_INSERTED)) {
                debug(L"Wake source = %d", wake_source);
                return CHARGER;
        }

        return NORMAL_BOOT;
}

enum boot_target check_battery()
{
        if (!get_off_mode_charge())
                return NORMAL_BOOT;

        if (is_battery_below_boot_OS_threshold()) {
                BOOLEAN charger_plugged = is_charger_plugged_in();
                debug(L"Battery is below boot OS threshold");
                debug(L"Charger is%s plugged", charger_plugged ? L"" : L" not");
                return charger_plugged ? CHARGER : POWER_OFF;
        }

        return NORMAL_BOOT;
}

/* Policy:
 * 1. Check if we had multiple watchdog reported in a short period of
 *    time.  If so, let the user choose the boot target.
 * 2. Check if the "-a xxxxxxxxx" command line was passed in, if so load an
 *    android boot image from RAM at that location.
 * 3. Check if the fastboot sentinel file \force_fastboot is present, and if
 *    so, force fastboot mode. Use in bootable media.
 * 4. Check for "magic key" being held. Short press loads Recovery. Long press
 *    loads Fastboot.
 * 5. Check if wake source is battery inserted, if so power off
 * 6. Check bootloader control block for a boot target, which could be
 *    the name of a boot image that we know how to read from a partition,
 *    or a boot image file in the ESP. BCB can specify oneshot or persistent
 *    targets.
 * 7. Check LoaderEntryOneShot for a boot target
 * 8. Check if we should go into charge mode or normal boot
 *
 * target_path - If ESP_EFI_BINARY or ESP_BOOTIMAGE returned, path to the
 *               image on the EFI System Partition
 * oneshot - Whether this is a one-shot boot, indicating that the image at
 *           target_path should be deleted before chainloading
 *
 */
static enum boot_target choose_boot_target(CHAR16 **target_path, BOOLEAN *oneshot)
{
        enum boot_target ret;

        *target_path = NULL;
        *oneshot = TRUE;

#if DEBUG_MESSAGES
        print_rsci_values();
#endif
        debug(L"Bootlogic: Choosing boot target");

        debug(L"Bootlogic: Check osloader command line...");
        ret = check_command_line();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check fastboot sentinel...");
        ret = check_fastboot_sentinel();
        if (ret != NORMAL_BOOT) {
                goto out;
        }

        debug(L"Bootlogic: Check magic key...");
        ret = check_magic_key();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check watchdog...");
        ret = check_watchdog();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check battery insertion...");
        ret = check_battery_inserted();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check BCB...");
        ret = check_bcb(target_path, oneshot);
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check reboot target...");
        ret = check_loader_entry_one_shot();
        if (ret != DNX && ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check battery level...");
        ret = check_battery();
        if (ret == POWER_OFF)
                ux_display_low_battery(3);
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check charger insertion...");
        ret = check_charge_mode();

out:
        debug(L"Bootlogic: selected '%s'",  boot_target_description(ret));
        return ret;
}

/* Validate an image.
 *
 * Parameters:
 * boot_target    - Boot image to load. Values supported are NORMAL_BOOT,
 *                  RECOVERY, and ESP_BOOTIMAGE (for 'fastboot boot')
 * bootimage      - Bootimage to validate
 * verifier_cert  - Return the certificate that validated the boot image
 *
 * Return values:
 * BOOT_STATE_GREEN  - Boot image is valid against provided certificate
 * BOOT_STATE_YELLOW - Boot image is valid against embedded certificate
 * BOOT_STATE_RED    - Boot image is not valid
 */
static UINT8 validate_bootimage(
                IN enum boot_target boot_target,
                IN VOID *bootimage,
                OUT X509 **verifier_cert)
{
        CHAR16 target[BOOT_TARGET_SIZE];
        CHAR16 *expected;
        CHAR16 *expected2 = NULL;
        UINT8 boot_state;

        boot_state = verify_android_boot_image(bootimage, oem_cert,
                                               oem_cert_size, target,
                                               verifier_cert);

        if (boot_state == BOOT_STATE_RED) {
                debug(L"boot image doesn't verify");
                return boot_state;
        }

        switch (boot_target) {
        case NORMAL_BOOT:
        case MEMORY:
                expected = L"/boot";
                /* in case of multistage ota */
                expected2 = L"/recovery";
                break;
        case CHARGER:
                expected = L"/boot";
                break;
        case RECOVERY:
                if (recovery_in_boot_partition())
                        expected = L"/boot";
                else
                        expected = L"/recovery";
                break;
        case ESP_BOOTIMAGE:
                /* "live" bootable image */
                expected = L"/boot";
                break;
        default:
                expected = NULL;
        }

        if ((!expected || StrCmp(expected, target)) &&
                        (!expected2 || StrCmp(expected2, target))) {
                debug(L"boot image has unexpected target name");
                return BOOT_STATE_RED;
        }

        return boot_state;
}

/* Load a boot image into RAM.
 *
 * boot_target  - Boot image to load. Values supported are NORMAL_BOOT, RECOVERY,
 *                and ESP_BOOTIMAGE (for 'fastboot boot')
 * target_path  - Path to load boot image from for ESP_BOOTIMAGE case, ignored
 *                otherwise.
 * bootimage    - Returned allocated pointer value for the loaded boot image.
 * oneshot      - For ESP_BOOTIMAGE case, flag indicating that the image should
 *                be deleted.
 *
 * Return values:
 * EFI_INVALID_PARAMETER - Unsupported boot target type, key is not well-formed,
 *                         or loaded boot image was missing or corrupt
 * EFI_ACCESS_DENIED     - Validation failed against OEM or embedded certificate,
 *                         boot image still usable
 */
static EFI_STATUS load_boot_image(
                IN enum boot_target boot_target,
                IN CHAR16 *target_path,
                OUT VOID **bootimage,
                IN BOOLEAN oneshot)
{
        EFI_STATUS ret;

        switch (boot_target) {
        case NORMAL_BOOT:
        case CHARGER:
                ret = EFI_NOT_FOUND;
                if (use_slot() && !slot_get_active())
                        break;
                do {
                        const CHAR16 *label = slot_label(BOOT_LABEL);
                        ret = android_image_load_partition(label, bootimage);
                        if (EFI_ERROR(ret)) {
                                efi_perror(ret, L"Failed to load boot image from %s partition",
                                           label);
                                if (use_slot())
                                        slot_boot_failed(boot_target);
                        }
                } while (EFI_ERROR(ret) && slot_get_active());
                break;
        case RECOVERY:
                if (recovery_in_boot_partition()) {
                        ret = load_boot_image(NORMAL_BOOT, target_path, bootimage, oneshot);
                        break;
                }
                if (use_slot() && !slot_recovery_tries_remaining()) {
                        ret = EFI_NOT_FOUND;
                        break;
                }
                ret = android_image_load_partition(RECOVERY_LABEL, bootimage);
                break;
        case ESP_BOOTIMAGE:
                /* "fastboot boot" case */
                ret = android_image_load_file(g_disk_device, target_path, oneshot,
                        bootimage);
                break;
        default:
                *bootimage = NULL;
                return EFI_INVALID_PARAMETER;
        }

        if (!EFI_ERROR(ret))
                debug(L"boot image loaded");

        return ret;
}


/* Chainload another EFI application on the ESP with the specified path,
 * optionally deleting the file before entering */
static EFI_STATUS enter_efi_binary(CHAR16 *path, BOOLEAN delete)
{
        EFI_DEVICE_PATH *edp;
        EFI_STATUS ret;
        EFI_HANDLE image;

        edp = FileDevicePath(g_disk_device, path);
        if (!edp) {
                error(L"Couldn't generate a path");
                return EFI_INVALID_PARAMETER;
        }

        ret = uefi_call_wrapper(BS->LoadImage, 6, FALSE, g_parent_image,
                        edp, NULL, 0, &image);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"BS->LoadImage '%s'", path);
        } else {
                if (delete) {
                        ret = file_delete(g_disk_device, path);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Couldn't delete %s", path);
                }
                ret = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
                uefi_call_wrapper(BS->UnloadImage, 1, image);
        }
        FreePool(edp);
        return ret;
}


#define OEMVARS_MAGIC           "#OEMVARS\n"
#define OEMVARS_MAGIC_SZ        9

static EFI_STATUS set_image_oemvars_nocheck(VOID *bootimage,
                                            const EFI_GUID *restricted_guid)
{
        VOID *oemvars;
        UINT32 osz;
        EFI_STATUS ret;

        ret = get_bootimage_2nd(bootimage, &oemvars, &osz);
        if (ret == EFI_SUCCESS && osz > OEMVARS_MAGIC_SZ &&
            !memcmp(oemvars, OEMVARS_MAGIC, OEMVARS_MAGIC_SZ)) {
                debug(L"secondstage contains raw oemvars");
                return flash_oemvars_silent_write_error((CHAR8*)oemvars + OEMVARS_MAGIC_SZ,
                                                        osz - OEMVARS_MAGIC_SZ,
                                                        restricted_guid);
        }

#ifdef HAL_AUTODETECT
        ret = get_bootimage_blob(bootimage, BLOB_TYPE_OEMVARS, &oemvars, &osz);
        if (EFI_ERROR(ret)) {
                if (ret == EFI_UNSUPPORTED || ret == EFI_NOT_FOUND) {
                        debug(L"No blobstore in this boot image");
                        return EFI_SUCCESS;
                }
                return ret;
        }

        return flash_oemvars_silent_write_error(oemvars, osz, restricted_guid);
#else
        return EFI_NOT_FOUND;
#endif
}

static EFI_STATUS set_image_oemvars(VOID *bootimage)
{
        if (!get_oemvars_update()) {
                debug(L"OEM vars should be up-to-date");
                return EFI_SUCCESS;
        }
        debug(L"OEM vars may need to be updated");
        set_oemvars_update(FALSE);

        return set_image_oemvars_nocheck(bootimage, NULL);
}

static EFI_STATUS load_image(VOID *bootimage, UINT8 boot_state,
                             enum boot_target boot_target,
                             X509 *verifier_cert)
{
        EFI_STATUS ret;
#ifdef USE_TRUSTY
        struct rot_data_t rot_data;
#endif
#ifdef USER
        /* per bootloaderequirements.pdf */
        if (boot_state == BOOT_STATE_ORANGE)
                android_clear_memory();
#endif

        set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
                        &boot_state, FALSE, TRUE);

#ifdef OS_SECURE_BOOT
        ret = set_os_secure_boot(boot_state == BOOT_STATE_GREEN);
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Failed to set os secure boot");
#endif

#ifdef USE_TRUSTY
        if (boot_target == NORMAL_BOOT ||
            boot_target == RECOVERY ||
            boot_target == CHARGER ||
            boot_target == MEMORY) {

                if (boot_state == BOOT_STATE_RED) {
#ifndef USERDEBUG
                        debug(L"Red state: start trusty anyway as ENG build");
#else
                        debug(L"Red state: invalid boot image.Unable to start trusty. Stop");
                        die();
#endif
                }
                debug(L"loading trusty");
                ret = get_rot_data(bootimage, boot_state, verifier_cert, &rot_data);
                if (EFI_ERROR(ret)){
                        efi_perror(ret, L"Unable to get the rot_data for trusty");
                        die();
                }
                ret = start_trusty(&rot_data);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"Unable to start trusty; stop.");
                        die();
                }
        }
#endif

        ret = slot_boot(boot_target);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to write slot boot");
                return ret;
        }

        debug(L"chainloading boot image, boot state is %s",
                        boot_state_to_string(boot_state));
        ret = android_image_start_buffer(g_parent_image, bootimage,
                                         boot_target, boot_state, NULL,
                                         verifier_cert);
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Couldn't load Boot image");

        ret = slot_boot_failed(boot_target);
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Failed to write slot failure");

        return ret;
}

static VOID die(VOID)
{
        /* Allow plenty of time for the error to be visible before the
         * screen goes blank */
        pause(30);
        halt_system();
}

static VOID enter_fastboot_mode(UINT8 boot_state)
        __attribute__ ((noreturn));

/* Enter Fastboot mode. If fastboot_start() returns a valid pointer,
 * try to start the bootimage pointed to. */
static VOID enter_fastboot_mode(UINT8 boot_state)
{
        EFI_STATUS ret = EFI_SUCCESS;
        enum boot_target target;
        EFI_HANDLE image;
        void *efiimage = NULL;
        UINTN imagesize;
        VOID *bootimage;
        X509 *verifier_cert = NULL;

        set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
                         &boot_state, FALSE, TRUE);
        set_oemvars_update(TRUE);

        for (;;) {
                target = UNKNOWN_TARGET;

                ret = fastboot_start(&bootimage, &efiimage, &imagesize, &target);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"Fastboot mode failed");
                        break;
                }

                if (bootimage) {
                        /* 'fastboot boot' case, only allowed on unlocked devices.
                         * check just to make sure */
                        if (device_is_unlocked()) {
                                set_image_oemvars_nocheck(bootimage, NULL);
#ifdef USE_SILENTLAKE
                                ret = silentlake_bind_root_of_trust(UNLOCKED, NULL);
                                if (EFI_ERROR(ret)) {
                                        efi_perror(ret, L"Failed to provide a root of trust to SilentLake");
                                        die();
                                }
#endif
                                validate_bootimage(MEMORY, bootimage, &verifier_cert);
                                load_image(bootimage, BOOT_STATE_ORANGE, MEMORY, verifier_cert);
                        }
                        FreePool(bootimage);
                        bootimage = NULL;
                        continue;
                }

                if (efiimage) {
                        ret = uefi_call_wrapper(BS->LoadImage, 6, FALSE, g_parent_image,
                                                NULL, efiimage, imagesize, &image);
                        FreePool(efiimage);
                        efiimage = NULL;
                        if (EFI_ERROR(ret)) {
                                efi_perror(ret, L"Unable to load the received EFI image");
                                continue;
                        }
                        ret = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Unable to start the received EFI image");

                        uefi_call_wrapper(BS->UnloadImage, 1, image);
                        continue;
                }

                /* Offer a fast path between crashmode and fastboot
                   mode to keep the RAM state.  */
                if (target == CRASHMODE) {
                        target = ux_prompt_user_for_boot_target(NO_ERROR_CODE);
                        if (target == FASTBOOT)
                                continue;
                }

                if (target != UNKNOWN_TARGET)
                        reboot_to_target(target);
        }

        die();
}

static EFI_STATUS push_capsule(
                IN EFI_FILE *root_dir,
                IN CHAR16 *name,
                OUT EFI_RESET_TYPE *resetType)
{
        UINTN len = 0;
        UINT64 max = 0;
        EFI_CAPSULE_HEADER *capHeader = NULL;
        EFI_CAPSULE_HEADER **capHeaderArray;
        EFI_CAPSULE_BLOCK_DESCRIPTOR *scatterList;
        CHAR8 *content = NULL;
        EFI_STATUS ret;

        debug(L"Trying to load capsule: %s", name);
        ret = file_read(root_dir, name, &content, &len);
        if (EFI_SUCCESS == ret) {
                if (len <= 0) {
                        debug(L"Couldn't load capsule data from disk");
                        FreePool(content);
                        return EFI_LOAD_ERROR;
                }
                /* Some capsules might invoke reset during UpdateCapsule
                so delete the file now */
                ret = file_delete(g_disk_device, name);
                if (ret != EFI_SUCCESS) {
                        efi_perror(ret, L"Couldn't delete %s", name);
                        FreePool(content);
                        return ret;
                }
        }
        else {
                debug(L"Error in reading file");
                return ret;
        }

        capHeader = (EFI_CAPSULE_HEADER *) content;
        capHeaderArray = AllocatePool(2*sizeof(EFI_CAPSULE_HEADER*));
        if (!capHeaderArray) {
                FreePool(content);
                return EFI_OUT_OF_RESOURCES;
        }
        capHeaderArray[0] = capHeader;
        capHeaderArray[1] = NULL;
        debug(L"Querying capsule capabilities");
        ret = uefi_call_wrapper(RT->QueryCapsuleCapabilities, 4,
                        capHeaderArray, 1,  &max, resetType);
        if (EFI_SUCCESS == ret) {
                if (len > max) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        return EFI_BAD_BUFFER_SIZE;
                }
                scatterList = AllocatePool(2*sizeof(EFI_CAPSULE_BLOCK_DESCRIPTOR));
                if (!scatterList) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        return EFI_OUT_OF_RESOURCES;
                }
                memset((CHAR8*)scatterList, 0x0,
                        2*sizeof(EFI_CAPSULE_BLOCK_DESCRIPTOR));
                scatterList->Length = len;
                scatterList->Union.DataBlock = (EFI_PHYSICAL_ADDRESS) (UINTN) capHeader;

                debug(L"Calling RT->UpdateCapsule");
                ret = uefi_call_wrapper(RT->UpdateCapsule, 3, capHeaderArray, 1,
                        (EFI_PHYSICAL_ADDRESS) (UINTN) scatterList);
                if (ret != EFI_SUCCESS) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        FreePool(scatterList);
                        return ret;
                }
        }
        return ret;
}

static void bootloader_recover_mode(UINT8 boot_state)
{
        enum boot_target target;

        target = ux_prompt_user_for_boot_target(NOT_BOOTABLE_CODE);
        if (target == FASTBOOT)
                enter_fastboot_mode(boot_state);

        reboot_to_target(target);
        die();
}

static VOID boot_error(enum ux_error_code error_code, UINT8 boot_state,
                       UINT8 *hash, UINTN hash_size)
{
        BOOLEAN power_off = FALSE;
        enum boot_target bt;

        if (boot_state > min_boot_state()) {
                power_off = TRUE;

#ifndef USER
#ifdef NO_DEVICE_UNLOCK
                error(L"NO_DEVICE_UNLOCK set, device should power off");
                error(L"Not a user build, continue anyway");
                power_off = FALSE;
#endif
#endif
        }

        bt = ux_prompt_user(error_code, power_off, boot_state, hash, hash_size);

        if (bt == CRASHMODE) {
                debug(L"Rebooting to bootloader recover mode");
                bootloader_recover_mode(boot_state);
        }

        if (power_off || bt == POWER_OFF)
                halt_system();
}

#ifdef BOOTLOADER_POLICY_EFI_VAR
/* Flash the OEMVARS that include the bootloader policy.  */
static void flash_bootloader_policy(void)
{
        UINT8 verify_state;
        VOID *bootimage;
        EFI_STATUS ret;

        debug(L"Loading bootloader policy");
        ret = load_boot_image(NORMAL_BOOT, NULL, &bootimage, FALSE);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to load the boot image to get bootloader policy");
                return;
        }

        verify_state = validate_bootimage(NORMAL_BOOT, bootimage, NULL);
        if (EFI_ERROR(ret) || verify_state != BOOT_STATE_GREEN) {
                efi_perror(ret, L"Failed to verify the boot image to get bootloader policy");
                goto out;
        }

        /* The bootloader policy EFI variables are using the
           FASTBOOT_GUID. */
        set_image_oemvars_nocheck(bootimage, &fastboot_guid);

        /* It might not be an error.  Some devices have a buggy BIOS
           that does not allowed secured EFI variables to be
           flashed.  */
        if (!blpolicy_is_flashed())
                debug(L"Bootloader Policy EFI variables are not flashed");
out:
        FreePool(bootimage);
}
#endif

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table)
{
        EFI_STATUS ret;
        CHAR16 *target_path = NULL;
        VOID *bootimage = NULL;
        BOOLEAN oneshot = FALSE;
        BOOLEAN lock_prompted = FALSE;
        enum boot_target boot_target = NORMAL_BOOT;
        UINT8 boot_state = BOOT_STATE_GREEN;
        UINT8 *hash = NULL;
        X509 *verifier_cert = NULL;
        CHAR16 *name = NULL;
        EFI_RESET_TYPE resetType;

        /* gnu-efi initialization */
        InitializeLib(image, sys_table);
        ux_display_vendor_splash();

        debug(KERNELFLINGER_VERSION);

        /* populate globals */
        g_parent_image = image;
        ret = uefi_call_wrapper(BS->OpenProtocol, 6, image,
                        &LoadedImageProtocol, (VOID **)&g_loaded_image,
                        image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"OpenProtocol: LoadedImageProtocol");
                return ret;
        }
        g_disk_device = g_loaded_image->DeviceHandle;

        /* loaded from mass storage (not DnX) */
        if (g_disk_device) {
                ret = storage_set_boot_device(g_disk_device);
                if (EFI_ERROR(ret))
                        error(L"Failed to set boot device");
        }

        if (file_exists(g_disk_device, FWUPDATE_FILE)) {
                name = FWUPDATE_FILE;
                push_capsule(g_disk_device, name, &resetType);

                debug(L"I am about to reset the system");

                uefi_call_wrapper(RT->ResetSystem, 4, resetType, EFI_SUCCESS, 0,
                                NULL);
        }

        ret = slot_init();
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Slot management initialization failed");
                return ret;
        }

        /* No UX prompts before this point, do not want to interfere
         * with magic key detection */
        boot_target = choose_boot_target(&target_path, &oneshot);
        if (boot_target == EXIT_SHELL)
                return EFI_SUCCESS;
        if (boot_target == CRASHMODE) {
                boot_target = ux_prompt_user_for_boot_target(NO_ERROR_CODE);
                if (boot_target != FASTBOOT)
                        reboot_to_target(boot_target);
        }

        if (boot_target == POWER_OFF)
                halt_system();

        if (boot_target == CHARGER)
                ux_display_empty_battery();

        if (boot_target == DNX || boot_target == CRASHMODE)
                reboot_to_target(boot_target);

#ifdef USERDEBUG
        debug(L"checking device state");

        if (!is_efi_secure_boot_enabled() && !device_is_provisioning()) {
                debug(L"uefi secure boot is disabled");
                boot_state = BOOT_STATE_ORANGE;
                lock_prompted = TRUE;

                /* Need to warn early, before we even enter Fastboot
                 * or run EFI binaries. Set lock_prompted to true so
                 * we don't ask again later */
                boot_error(SECURE_BOOT_CODE, boot_state, NULL, 0);
        } else  if (device_is_unlocked()) {
                boot_state = BOOT_STATE_ORANGE;
                debug(L"Device is unlocked");
        }

#ifdef USER
        if (device_is_provisioning()) {
                debug(L"device is provisioning, force Fastboot mode");
                enter_fastboot_mode(boot_state);
        }
#endif
#else /* !USERDEBUG */
        /* Make sure it's abundantly clear! */
        error(L"INSECURE BOOTLOADER - SYSTEM SECURITY IN RED STATE");
        pause(1);
        boot_state = BOOT_STATE_RED;
#endif

        /* EFI binaries are validated by the BIOS */
        if (boot_target == ESP_EFI_BINARY) {
                debug(L"entering EFI binary");
                ret = enter_efi_binary(target_path, oneshot);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"EFI Application exited abnormally");
                        pause(3);
                }
                FreePool(target_path);
                reboot(NULL);
        }

#ifdef BOOTLOADER_POLICY_EFI_VAR
        /* Ensure that the bootloader policy is set. */
        if (!device_is_provisioning() && !blpolicy_is_flashed())
                flash_bootloader_policy();
#endif

        if (boot_target == FASTBOOT) {
                debug(L"entering Fastboot mode");
                enter_fastboot_mode(boot_state);
        }

        /* If the device is unlocked the only way to re-lock it is
         * via fastboot. Skip this UX if we already prompted earlier
         * about EFI secure boot being turned off */
        if (boot_state == BOOT_STATE_ORANGE && !lock_prompted)
                boot_error(DEVICE_UNLOCKED_CODE, boot_state, NULL, 0);

        debug(L"Loading boot image");
        ret = load_boot_image(boot_target, target_path, &bootimage, oneshot);
        FreePool(target_path);
        if (EFI_ERROR(ret)) {
                debug(L"issue loading boot image: %r", ret);
                boot_state = BOOT_STATE_RED;
        } else {
                UINT8 new_boot_state;
                debug(L"Validating boot image");
                new_boot_state = validate_bootimage(boot_target, bootimage,
                                                    &verifier_cert);
                if (boot_state != BOOT_STATE_ORANGE)
                        boot_state = new_boot_state;
        }

        if (boot_state == BOOT_STATE_YELLOW) {
                ret = pub_key_sha256(verifier_cert, &hash);
                if (EFI_ERROR(ret))
                        efi_perror(ret, L"Failed to compute pub key hash");
                boot_error(BOOTIMAGE_UNTRUSTED_CODE, boot_state, hash,
                           SHA256_DIGEST_LENGTH);
        }

        if (boot_state == BOOT_STATE_RED) {
                if (boot_target == RECOVERY)
                        boot_error(BAD_RECOVERY_CODE, boot_state, NULL, 0);
                else
                        boot_error(RED_STATE_CODE, boot_state, NULL, 0);
        }

        switch (boot_target) {
        case RECOVERY:
        case ESP_BOOTIMAGE:
                /* We're either about to do an OTA update, or doing a one-shot
                 * boot into an alternate boot image from 'fastboot boot'.
                 * Load the OEM vars in this new boot image, but ensure that
                 * we'll read them again on the next normal boot */
                set_image_oemvars_nocheck(bootimage, NULL);
                set_oemvars_update(TRUE);
                break;
        case NORMAL_BOOT:
        case CHARGER:
                set_image_oemvars(bootimage);
                break;
        default:
                break;
        }

#ifdef USE_SILENTLAKE
        ret = silentlake_bind_root_of_trust(get_current_state(), verifier_cert);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to provide a root of trust to SilentLake");
                die();
        }
#endif

        ret = load_image(bootimage, boot_state, boot_target, verifier_cert);
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Failed to start boot image");

        switch (boot_target) {
        case NORMAL_BOOT:
        case CHARGER:
                if (slot_get_active())
                        reboot_to_target(boot_target);
                break;
        case RECOVERY:
                if (recovery_in_boot_partition()) {
                        if (slot_get_active())
                                reboot_to_target(boot_target);
                } else if (slot_recovery_tries_remaining())
                        reboot_to_target(boot_target);
                break;
        default:
                break;
        }

        bootloader_recover_mode(boot_state);

        return EFI_INVALID_PARAMETER;
}

/* vim: softtabstop=8:shiftwidth=8:expandtab
 */
