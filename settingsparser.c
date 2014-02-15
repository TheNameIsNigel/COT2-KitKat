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
#include "iniparser/iniparser.h"
#include "iniparser/dictionary.h"

COTSETTINGS = "/sdcard/cotrecovery/settings.ini";
int fallback_settings = 0;

int backupprompt = 0;
int orswipeprompt = 0;
int orsreboot = 0;
int signature_check_enabled = 0;
char * currenttheme;

void create_default_settings(void) {
  FILE * ini;
  if (ini = fopen_path(COTSETTINGS, "r")) {
    fclose(COTSETTINGS);
    remove(COTSETTINGS);
  }
  
  ini = fopen_path(COTSETTINGS, "w");
  fprintf(ini,
    ";\n"
    "; COT Settings INI\n"
    ";\n"
    "\n"
    "[settings]\n"
    "theme = hydro ;\n"
    "orsreboot = 0 ;\n"
    "orswipeprompt = 1 ;\n"
    "backupprompt = 1 ;\n"
    "signaturecheckenabled = 1s ;\n"
    "\n");
  fclose(ini);
}

void load_fallback_settings() {
  ui_print("Unable to mount sdcard,\nloading failsafe setting...\n\nSettings will not work\nwithout an sdcard...\n");
  fallback_settings = 1;
  currenttheme = "hydro";
  signature_check_enabled = 1;
  backupprompt = 1;
  orswipeprompt = 1;
  orsreboot = 0;
}

void update_cot_settings(void) {
  LOGI("Updating COT Settings...\n");
  dictionary * ini ;
  char       * ini_name ;
  ini_name = COTSETTINGS;
  LOGI("Loading current ini...\n");
  ini = iniparser_load(ini_name);
  if (ini==NULL) {
    LOGI("Can't load current INI!\n");
  } else {
    LOGI("Current INI loaded!\n");
  }
  iniparser_set(ini, "settings:theme", "hydro");
  iniparser_set(ini, "settings:orsreboot", "0");
  iniparser_set(ini, "settings:orswipeprompt", "1");
  iniparser_set(ini, "settings:backupprompt", "1");
  iniparser_set(ini, "settings:signaturecheckenabled", "1");
  iniparser_dump_ini(ini, ini_name);
  iniparser_freedict(ini);
  LOGI("Settings updated!\n");
  parse_settings();
}

void parse_settings() {
  dictionary * ini ;
  char       * ini_name ;
  ini_name = COTSETTINGS;
  if (ensure_path_mounted("/sdcard") != 0) {
    load_fallback_settings();
    handle_theme(currenttheme);
    return;
  }
  ini = iniparser_load(ini_name);
  if (ini==NULL) {
    ui_print("Can't load COT settings!\nSetting defaults...\n");
    create_default_settings();
    ini = iniparser_load(ini_name);
  }
  orsreboot = iniparser_getint(ini, "settings:orsreboot", NULL);
  orswipeprompt = iniparser_getint(ini, "settings:orswipeprompt", NULL);
  backupprompt = iniparser_getint(ini, "settings:backupprompt", NULL);
  signature_check_enabled = iniparser_getint(ini, "settings:signaturecheckenabled", NULL);
  currenttheme = iniparser_getstring(ini, "settings:theme", NULL);
  LOGI("ORSReboot: %d\n", orsreboot);
  LOGI("ORSWipePrompt: %d\n", orswipeprompt);
  LOGI("BackupPrompt: %d\n", backupprompt);
  LOGI("SigCheck: %d\n", signature_check_enabled);
  LOGI("Theme: %s\n", currenttheme);
  LOGI("Settings loaded!\n");
  handle_theme(currenttheme);
}

void handle_theme(char * theme_name) {
  LOGI("Attempting to load theme %s\n", theme_name);
  dictionary * ini ;
  char * theme_base = "/res/theme/theme_";
  char * theme_end = ".ini";
  char * full_theme_file = malloc(strlen(theme_base) + strlen(theme_end) + strlen(theme_name));
  strcpy(full_theme_file, theme_base);
  strcat(full_theme_file, theme_name);
  strcat(full_theme_file, theme_end);
  
  ini = iniparser_load(full_theme_file);
  if (ini==NULL) {
    LOGI("Can't load theme %s!\n", full_theme_file);
    return;
  }
  LOGI("Theme %s loaded!\n", theme_name);
  
  UICOLOR0 = iniparser_getint(ini, "theme:uicolor0", NULL);
  UICOLOR1 = iniparser_getint(ini, "theme:uicolor1", NULL);
  UICOLOR2 = iniparser_getint(ini, "theme:uicolor2", NULL);
  UITHEME = iniparser_getint(ini, "theme:bgicon", NULL);
  
  ui_dyn_background();
  ui_reset_icons();
}