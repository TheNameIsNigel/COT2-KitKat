/*
 * Copyright (C) 2012 Project Open Cannibal
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
#include "eraseandformat.h"
#include "recovery_settings.h"

int UICOLOR0 = 0;
int UICOLOR1 = 0;
int UICOLOR2 = 0;
int UITHEME = 0;

int UI_COLOR_DEBUG = 0;

void show_cot_options_menu() {
  static char* headers[] = {
    "COT Options",
    "",
    NULL
  };
  
#define COT_OPTIONS_ITEM_ADVANCED	0
#define COT_OPTIONS_ITEM_SETTINGS	1
#define COT_OPTIONS_ITEM_AROMA		2
  

  static char* list[4];
  list[0] = "Advanced Options";
  list[1] = "COT Settings";
  list[2] = "Aroma File Manager";
  list[3] = NULL;
  
  for (;;) {
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
      case GO_BACK:
      {
	return;
      }
      case COT_OPTIONS_ITEM_ADVANCED:
      {
	show_advanced_options_menu();
	break;
      }
      case COT_OPTIONS_ITEM_SETTINGS:
      {
	show_settings_menu();
	break;
      }
      case COT_OPTIONS_ITEM_AROMA:
      {
	run_aroma_browser();
	break;
      }
    }
  }
}

void show_advanced_options_menu() {
  
  #ifdef RECOVERY_DEBUG_BUILD
  #ifdef ENABLE_LOKI
  #define FIXED_ADVANCED_ENTRIES 3
  #else
  #define FIXED_ADVANCED_ENTRIES 2
  #endif
  #else
  #ifdef ENABLE_LOKI
  #define FIXED_ADVANCED_ENTRIES 2
  #else
  #define FIXED_ADVANCED_ENTRIES 1
  #endif
  #endif
  
  char buf[80];
  int i = 0, j = 0, chosen_item = 0;
  char* primary_path = get_primary_storage_path();
  char** extra_paths = get_extra_storage_paths();
  int num_extra_volumes = get_num_extra_volumes();
  static char* list[MAX_NUM_MANAGED_VOLUMES + FIXED_ADVANCED_ENTRIES + 1];
  
  static const char* headers[] = {
    "Advanced Options",
    "",
    NULL
  };
  
  #ifdef RECOVERY_DEBUG_BUILD
  list[0] = "Recovery Debugging";
  list[1] = "Wipe Dalvik-Cache";
  #ifdef ENABLE_LOKI
  list[2] = "Toggle Loki";
  #endif
  #else
  list[0] = "Wipe Dalvik-Cache";
  #ifdef ENABLE_LOKI
  list[1] = "Toggle Loki";
  #endif
  #endif
  
  char list_prefix[] = "Partition ";
  if (can_partition(primary_path)) {
    sprintf(buf, "%s%s", list_prefix, primary_path);
    list[FIXED_ADVANCED_ENTRIES] = strdup(buf);
    j++;
  }
  
  if (extra_paths != NULL) {
    for (i = 0; i < num_extra_volumes; i++) {
      if (can_partition(extra_paths[i])) {
	sprintf(buf, "%s%s", list_prefix, extra_paths[i]);
	list[FIXED_ADVANCED_ENTRIES + j] = strdup(buf);
	j++;
      }
    }
  }
  list[FIXED_ADVANCED_ENTRIES + j] = NULL;
  
  for (;;) {
    chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
    switch (chosen_item) {
      case GO_BACK:
      {
	return;
      }
      #ifdef RECOVERY_DEBUG_BUILD
      case 0:
      {
	show_recovery_debugging_menu();
	break;
      }
      case 1:
	#else
      case 0:
	#endif
      {
	if (0 != ensure_path_mounted("/data"))
	  break;
	if (volume_for_path("/sd-ext") != NULL)
	  ensure_path_mounted("/sd-ext");
	ensure_path_mounted("/cache");
	if (confirm_selection("Confirm Wipe?", "Yes - Wipe Dalvik-Cache")) {
	  __system("rm -r /data/dalvik-cache");
	  __system("rm -r /cache/dalvik-cache");
	  __system("rm -r /sd-ext/dalvik-cache");
	  ui_print("Dalvik-Cache wiped.\n");
	}
	ensure_path_unmounted("/data");
	break;
      }
      #ifdef ENABLE_LOKI
      #ifdef RECOVERY_DEBUG_BUILD
      case 2:
	#else
      case 1:
	#endif
      {
	toggle_loki_support();
	break;
      }
      #endif
      default:
      {
	partition_sdcard(list[chosen_item] + strlen(list_prefix));
	break;
      }
    }
  }
}

void show_recovery_debugging_menu() {
  static char* headers[] = { "Recovery Debugging",
    "",
    NULL
  };
  
  static char* list[] = { "Report Error",
    "Key Test",
    "Show Log",
    "Toggle UI Debugging",
    NULL
  };
  
  for (;;) {
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if(chosen_item == GO_BACK)
      break;
    switch(chosen_item) {
      case 0:
      {
	handle_failure(1);
	break;
      }
      case 1:
      {
	ui_print("Outputting key codes.\n");
	ui_print("Go back to end debugging.\n");
	int key;
	int action;
	do
	{
	  key = ui_wait_key();
	  action = device_handle_key(key, 1);
	  ui_print("Key: %d\n", key);
	}
	while (action != GO_BACK);
	break;
      }
      case 2:
      {
	ui_printlogtail(12);
	break;
      }
      case 3:
      {
	toggle_ui_debugging();
	break;
      }
    }
  }
}

void show_settings_menu() {
  static char* headers[] = {
    "COT Settings",
    "",
    NULL
  };
  
  #define SETTINGS_ITEM_THEME         0
  #define SETTINGS_ITEM_ORS_REBOOT    1
  #define SETTINGS_ITEM_ORS_WIPE      2
  #define SETTINGS_ITEM_NAND_PROMPT   3
  #define SETTINGS_ITEM_SIGCHECK      4
  #define SETTINGS_ITEM_DEV_OPTIONS   5
  
  static char* list[6];
  
  list[0] = "Theme";
  if (orsreboot == 1) {
    list[1] = "Disable forced reboots";
  } else {
    list[1] = "Enable forced reboots";
  }
  if (orswipeprompt == 1) {
    list[2] = "Disable wipe prompt";
  } else {
    list[2] = "Enable wipe prompt";
  }
  if (backupprompt == 1) {
    list[3] = "Disable zip flash nandroid prompt";
  } else {
    list[3] = "Enable zip flash nandroid prompt";
  }
  if (signature_check_enabled == 1) {
    list[4] = "Disable md5 signature check";
  } else {
    list[4] = "Enable md5 signature check";
  }
  list[5] = NULL;
  
  for (;;) {
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
      case GO_BACK:
	return;
      case SETTINGS_ITEM_THEME:
      {
	static char* ui_colors[] = {"Hydro (default)",
	  "Blood Red",
	  NULL
	};
	static char* ui_header[] = {"COT Theme", "", NULL};
	
	int ui_color = get_menu_selection(ui_header, ui_colors, 0, 0);
	if(ui_color == GO_BACK)
	  continue;
	else {
	  switch(ui_color) {
	    case 0:
	      currenttheme = "hydro";
	      break;
	    case 1:
	      currenttheme = "bloodred";
	      break;
	  }
	  break;
	}
      }
      case SETTINGS_ITEM_ORS_REBOOT:
      {
	if (orsreboot == 1) {
	  ui_print("Disabling forced reboots.\n");
	  list[1] = "Enable forced reboots";
	  orsreboot = 0;
	} else {
	  ui_print("Enabling forced reboots.\n");
	  list[1] = "Disable forced reboots";
	  orsreboot = 1;
	}
	break;
      }
      case SETTINGS_ITEM_ORS_WIPE:
      {
	if (orswipeprompt == 1) {
	  ui_print("Disabling wipe prompt.\n");
	  list[2] = "Enable wipe prompt";
	  orswipeprompt = 0;
	} else {
	  ui_print("Enabling wipe prompt.\n");
	  list[2] = "Disable wipe prompt";
	  orswipeprompt = 1;
	}
	break;
      }
      case SETTINGS_ITEM_NAND_PROMPT:
      {
	if (backupprompt == 1) {
	  ui_print("Disabling zip flash nandroid prompt.\n");
	  list[3] = "Enable zip flash nandroid prompt";
	  backupprompt = 0;
	} else {
	  ui_print("Enabling zip flash nandroid prompt.\n");
	  list[3] = "Disable zip flash nandroid prompt";
	  backupprompt = 1;
	}
	break;
      }
      case SETTINGS_ITEM_SIGCHECK:
      {
	if (signature_check_enabled == 1) {
	  ui_print("Disabling md5 signature check.\n");
	  list[4] = "Enable md5 signature check";
	  signature_check_enabled = 0;
	} else {
	  ui_print("Enabling md5 signature check.\n");
	  list[4] = "Disable md5 signature check";
	  signature_check_enabled = 1;
	}
	break;
      }
      default:
	return;
    }
    update_cot_settings();
  }
}

void show_about_menu() {
  static char* headers[] = {
    "About COT",
    "",
    NULL
  };

  static char* list[4];
  list[0] = "RECOVERY_VERSION";
  list[1] = "RECOVERY_BUILD_DATE";
  list[2] = "RECOVERY_BUILDER_STRING";
  list[3] = "Copyright (C) 2014 Project Open Cannibal";
  for (;;) {
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
      default:
	return;
    }
  }
}

void clear_screen() {
  ui_print("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
}

// launch aromafm.zip from default locations
static int default_aromafm(const char* root) {
    char aroma_file[PATH_MAX];
    sprintf(aroma_file, "%s/%s", root, AROMA_FM_PATH);

    if (file_found(aroma_file)) {
        install_zip(aroma_file);
        return 0;
    }
    return -1;
}

void run_aroma_browser() {
    // look for AROMA_FM_PATH in storage paths
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();
    int ret = -1;
    int i = 0;

    vold_mount_all();
    ret = default_aromafm(get_primary_storage_path());
    if (extra_paths != NULL) {
        i = 0;
        while (ret && i < num_extra_volumes) {
            ret = default_aromafm(extra_paths[i]);
            ++i;
        }
    }
    if (ret != 0)
        LOGE("No %s in storage paths\n", AROMA_FM_PATH);
}
//------ end aromafm launcher functions