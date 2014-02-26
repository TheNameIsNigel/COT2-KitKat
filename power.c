/*
 * Copyright (C) 2012 Drew Walton & Nathan Bass
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "make_ext4fs.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"
#include "cutils/android_reboot.h"
#include "settings.h"
#include "settingsparser.h"
#include "power.h"

void show_power_options_menu() {
  static char* headers[] = { "Power Options",
    "",
    NULL
  };
  
  #define POWER_OPTIONS_ITEM_REBOOT	0
  #define POWER_OPTIONS_ITEM_BOOTLOADER	1
  #define POWER_OPTIONS_ITEM_POWEROFF	2
  
  static char* list[4];
  list[0] = "Reboot Recovery";
  char bootloader_mode[PROPERTY_VALUE_MAX];
  property_get("ro.bootloader.mode", bootloader_mode, "");
  if (!strcmp(bootloader_mode, "download")) {
    list[1] = "Download Mode";
  } else {
    list[1] = "Bootloader";
  }
  list[2] = "Power Off";
  list[3] = NULL;
  for (;;) {
    int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
    switch (chosen_item) {
      case GO_BACK:
	return;
      case POWER_OPTIONS_ITEM_REBOOT:
	ui_print("Rebooting recovery...\n");
	reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
	break;
      case POWER_OPTIONS_ITEM_BOOTLOADER:
	if (!strcmp(bootloader_mode, "download")) {
	  ui_print("Rebooting to download mode...\n");
	  reboot_main_system(ANDROID_RB_RESTART2, 0, "download");
	} else {
	  ui_print("Rebooting to bootloader...\n");
	  reboot_main_system(ANDROID_RB_RESTART2, 0, "bootloader");
	}
	break;
      case POWER_OPTIONS_ITEM_POWEROFF:
	pass_shutdown_cmd();
	break;
    }
  }
}

/* On the off chance that your device requires a non-standard command
 * to reboot or power off from recovery simply replace that command in
 * one of the following.
 *
 * It is highly unlikely you will need to change this for your device
 * this was added for the soul purpose of patching reboot in the Amazon
 * Kindle Fire which uses a binary command to reset the idme bootmode. */
void pass_normal_reboot() {
	reboot_main_system(ANDROID_RB_RESTART, 0, 0);
}

void pass_shutdown_cmd() {
	reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
}