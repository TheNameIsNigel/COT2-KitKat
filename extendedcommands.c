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
#include "recovery_settings.h"
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

#define ABS_MT_POSITION_X 0x35	/* Center X ellipse position */

#include "mmcutils/mmcutils.h"
#include "voldclient/voldclient.h"

#include "adb_install.h"

int get_filtered_menu_selection(const char** headers, char** items, int menu_only, int initial_selection, int items_count) {
  int index;
  int offset = 0;
  int* translate_table = (int*)malloc(sizeof(int) * items_count);
  char* items_new[items_count];
  
  for (index = 0; index < items_count; index++) {
    items_new[index] = items[index];
  }
  
  for (index = 0; index < items_count; index++) {
    if (items_new[index] == NULL)
      continue;
    char *item = items_new[index];
    items_new[index] = NULL;
    items_new[offset] = item;
    translate_table[offset] = index;
    offset++;
  }
  items_new[offset] = NULL;
  
  initial_selection = translate_table[initial_selection];
  int ret = get_menu_selection(headers, items_new, menu_only, initial_selection);
  if (ret < 0 || ret >= offset) {
    free(translate_table);
    return ret;
  }
  
  ret = translate_table[ret];
  free(translate_table);
  return ret;
}

void write_string_to_file(const char* filename, const char* string) {
  ensure_path_mounted(filename);
  char tmp[PATH_MAX];
  sprintf(tmp, "mkdir -p $(dirname %s)", filename);
  __system(tmp);
  FILE *file = fopen(filename, "w");
  if (file != NULL) {
    fprintf(file, "%s", string);
    fclose(file);
  }
}

void write_recovery_version() {
  char path[PATH_MAX];
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_VERSION_FILE);
  write_string_to_file(path, EXPAND(RECOVERY_VERSION) "\n" EXPAND(TARGET_DEVICE));
  // force unmount /data for /data/media devices as we call this on recovery exit
  ignore_data_media_workaround(1);
  ensure_path_unmounted(path);
  ignore_data_media_workaround(0);
}

static void write_last_install_path(const char* install_path) {
  char path[PATH_MAX];
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_LAST_INSTALL_FILE);
  write_string_to_file(path, install_path);
}

const char* read_last_install_path() {
  static char path[PATH_MAX];
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_LAST_INSTALL_FILE);
  
  ensure_path_mounted(path);
  FILE *f = fopen(path, "r");
  if (f != NULL) {
    fgets(path, PATH_MAX, f);
    fclose(f);
    
    return path;
  }
  return NULL;
}

void toggle_ui_debugging()

{
  switch(UI_COLOR_DEBUG) {
    case 0: {
      ui_print("Enabling UI color debugging; will disable again on reboot.\n");
      UI_COLOR_DEBUG = 1;
      break;
    }
    default: {
      ui_print("Disabling UI color debugging.\n");
      UI_COLOR_DEBUG = 0;
      break;
    }
  }
}

#ifdef ENABLE_LOKI
int loki_support_enabled = 1;
void toggle_loki_support() {
  loki_support_enabled = !loki_support_enabled;
  ui_print("Loki Support: %s\n", loki_support_enabled ? "Enabled" : "Disabled");
}
#endif

int install_zip(const char* packagefilepath) {
  ui_print("\n-- Installing: %s\n", packagefilepath);
  if (device_flash_type() == MTD) {
    set_sdcard_update_bootloader_message();
  }
  
  int status = install_package(packagefilepath);
  ui_reset_progress();
  if (status != INSTALL_SUCCESS) {
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    ui_print("Installation aborted.\n");
    return 1;
  }
  #ifdef ENABLE_LOKI
  if (loki_support_enabled) {
    ui_print("Checking if loki-fying is needed\n");
    status = loki_check();
    if (status != INSTALL_SUCCESS) {
      ui_set_background(BACKGROUND_ICON_CLOCKWORK);
      return 1;
    }
  }
  #endif
  
  ui_print("\nInstall from sdcard complete.\n");
  ui_init_icons();
  return 0;
}

// top fixed menu items, those before extra storage volumes
#define FIXED_TOP_INSTALL_ZIP_MENUS 1
// bottom fixed menu items, those after extra storage volumes
#define FIXED_BOTTOM_INSTALL_ZIP_MENUS 2
#define FIXED_INSTALL_ZIP_MENUS (FIXED_TOP_INSTALL_ZIP_MENUS + FIXED_BOTTOM_INSTALL_ZIP_MENUS)

int show_install_update_menu() {
  char buf[100];
  int i = 0, chosen_item = 0;
  static char* install_menu_items[MAX_NUM_MANAGED_VOLUMES + FIXED_INSTALL_ZIP_MENUS + 1];
  
  char* primary_path = get_primary_storage_path();
  char** extra_paths = get_extra_storage_paths();
  int num_extra_volumes = get_num_extra_volumes();
  
  memset(install_menu_items, 0, MAX_NUM_MANAGED_VOLUMES + FIXED_INSTALL_ZIP_MENUS + 1);
  
  static const char* headers[] = { "ZIP Flashing", "", NULL };
  
  // FIXED_TOP_INSTALL_ZIP_MENUS
  sprintf(buf, "choose zip from %s", primary_path);
  install_menu_items[0] = strdup(buf);
  
  // extra storage volumes (vold managed)
  for (i = 0; i < num_extra_volumes; i++) {
    sprintf(buf, "choose zip from %s", extra_paths[i]);
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i] = strdup(buf);
  }
  
  // FIXED_BOTTOM_INSTALL_ZIP_MENUS
  install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes] = "choose zip from last install folder";
  install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1] = "install zip from sideload";
  
  // extra NULL for GO_BACK
  install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2] = NULL;
  
  for (;;) {
    chosen_item = get_menu_selection(headers, install_menu_items, 0, 0);
    if (chosen_item == 0) {
      show_choose_zip_menu(primary_path);
    } else if (chosen_item >= FIXED_TOP_INSTALL_ZIP_MENUS && chosen_item < FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
      show_choose_zip_menu(extra_paths[chosen_item - FIXED_TOP_INSTALL_ZIP_MENUS]);
    } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
      const char *last_path_used = read_last_install_path();
      if (last_path_used == NULL)
	show_choose_zip_menu(primary_path);
      else
	show_choose_zip_menu(last_path_used);
    } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1) {
      apply_from_adb();
    } else {
      // GO_BACK or REFRESH (chosen_item < 0)
      goto out;
    }
  }
  out:
  // free all the dynamic items
  free(install_menu_items[0]);
  if (extra_paths != NULL) {
    for (i = 0; i < num_extra_volumes; i++)
      free(install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i]);
  }
  return chosen_item;
}

void free_string_array(char** array) {
  if (array == NULL)
    return;
  char* cursor = array[0];
  int i = 0;
  while (cursor != NULL) {
    free(cursor);
    cursor = array[++i];
  }
  free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles) {
  char path[PATH_MAX] = "";
  DIR *dir;
  struct dirent *de;
  int total = 0;
  int i;
  char** files = NULL;
  int pass;
  *numFiles = 0;
  int dirLen = strlen(directory);
  
  dir = opendir(directory);
  if (dir == NULL) {
    ui_print("Couldn't open directory.\n");
    return NULL;
  }
  
  unsigned int extension_length = 0;
  if (fileExtensionOrDirectory != NULL)
    extension_length = strlen(fileExtensionOrDirectory);
  
  int isCounting = 1;
  i = 0;
  for (pass = 0; pass < 2; pass++) {
    while ((de = readdir(dir)) != NULL) {
      // skip hidden files
      if (de->d_name[0] == '.')
	continue;
      
      // NULL means that we are gathering directories, so skip this
      if (fileExtensionOrDirectory != NULL) {
	// make sure that we can have the desired extension (prevent seg fault)
	if (strlen(de->d_name) < extension_length)
	  continue;
	// compare the extension
	if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
	  continue;
      } else {
	struct stat info;
	char fullFileName[PATH_MAX];
	strcpy(fullFileName, directory);
	strcat(fullFileName, de->d_name);
	lstat(fullFileName, &info);
	// make sure it is a directory
	if (!(S_ISDIR(info.st_mode)))
	  continue;
      }
      
      if (pass == 0) {
	total++;
	continue;
      }
      
      files[i] = (char*)malloc(dirLen + strlen(de->d_name) + 2);
      strcpy(files[i], directory);
      strcat(files[i], de->d_name);
      if (fileExtensionOrDirectory == NULL)
	strcat(files[i], "/");
      i++;
    }
    if (pass == 1)
      break;
    if (total == 0)
      break;
    rewinddir(dir);
    *numFiles = total;
    files = (char**)malloc((total + 1) * sizeof(char*));
    files[total] = NULL;
  }
  
  if (closedir(dir) < 0) {
    LOGE("Failed to close directory.\n");
  }
  
  if (total == 0) {
    return NULL;
  }
  // sort the result
  if (files != NULL) {
    for (i = 0; i < total; i++) {
      int curMax = -1;
      int j;
      for (j = 0; j < total - i; j++) {
	if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
	  curMax = j;
      }
      char* temp = files[curMax];
      files[curMax] = files[total - i - 1];
      files[total - i - 1] = temp;
    }
  }
  
  return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
static int no_files_found = 1; //choose_file_menu returns string NULL when no file is found or if we choose no file in selection
                               //no_files_found = 1 when no valid file was found, no_files_found = 0 when we found a valid file
                               //added for custom ors menu support + later kernel restore
                               
char* choose_file_menu(const char* basedir, const char* fileExtensionOrDirectory, const char* headers[]) {
  const char* fixed_headers[20];
  int numFiles = 0;
  int numDirs = 0;
  int i;
  char* return_value = NULL;
  char directory[PATH_MAX];
  int dir_len = strlen(basedir);
  
  strcpy(directory, basedir);
  
  // Append a trailing slash if necessary
  if (directory[dir_len - 1] != '/') {
    strcat(directory, "/");
    dir_len++;
  }
  
  i = 0;
  while (headers[i]) {
    fixed_headers[i] = headers[i];
    i++;
  }
  fixed_headers[i] = directory;
  fixed_headers[i + 1] = "";
  fixed_headers[i + 2] = NULL;
  
  char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
  char** dirs = NULL;
  if (fileExtensionOrDirectory != NULL)
    dirs = gather_files(directory, NULL, &numDirs);
  int total = numDirs + numFiles;
  if (total == 0) {
    no_files_found = 1; //we found no valid file to select
    ui_print("No files found.\n");
  } else {
    char** list = (char**)malloc((total + 1) * sizeof(char*));
    list[total] = NULL;
    
    
    for (i = 0; i < numDirs; i++) {
      list[i] = strdup(dirs[i] + dir_len);
    }
    
    for (i = 0; i < numFiles; i++) {
      list[numDirs + i] = strdup(files[i] + dir_len);
    }
    
    for (;;) {
      int chosen_item = get_menu_selection(fixed_headers, list, 0, 0);
      if (chosen_item == GO_BACK || chosen_item == REFRESH)
	break;
      if (chosen_item < numDirs) {
	char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
	if (subret != NULL) {
	  return_value = strdup(subret);
	  free(subret);
	  break;
	}
	continue;
      }
      return_value = strdup(files[chosen_item - numDirs]);
      break;
    }
    free_string_array(list);
  }
  
  free_string_array(files);
  free_string_array(dirs);
  return return_value;
}

void show_choose_zip_menu(const char *mount_point) {
  if (ensure_path_mounted(mount_point) != 0) {
    LOGE("Can't mount %s\n", mount_point);
    return;
  }
  
  static char *INSTALL_OR_BACKUP_ITEMS[] = {
    "Yes - Backup and install",
    "No - Install without backup",
    "Cancel Install",
    NULL
  };
  
#define ITEM_BACKUP_AND_INSTALL 0
#define ITEM_INSTALL_WOUT_BACKUP 1
#define ITEM_CANCELL_INSTALL 2
  
  static const char* headers[] = { "Choose a zip to apply", "", NULL };
  
  char* file = choose_file_menu(mount_point, ".zip", headers);
  if (file == NULL)
    return;
  
  if (backupprompt == 0) {
    char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
  
    if (confirm_selection("Confirm install?", confirm)) {
      install_zip(file);
      write_last_install_path(dirname(file));
    }
  } else {
    for (;;) {
      int chosen_item = get_menu_selection(headers, INSTALL_OR_BACKUP_ITEMS, 0, 0);
      switch(chosen_item) {
	case ITEM_BACKUP_AND_INSTALL:
	{
	  char backup_path[PATH_MAX];
	  nandroid_generate_timestamp_path(backup_path);
	  nandroid_backup(backup_path);
	  install_zip(file);
	  free(file);
	  return;
	}
	case ITEM_INSTALL_WOUT_BACKUP:
	  install_zip(file);
	  free(file);
	  return;
	default:
	  break;
      }
      break;
    }
  }
  free(file);
}

void show_nandroid_restore_menu(const char* path) {
  if (ensure_path_mounted(path) != 0) {
    LOGE("Can't mount %s\n", path);
    return;
  }
  
  static const char* headers[] = { "Choose an image to restore", "", NULL };
  
  char tmp[PATH_MAX];
  sprintf(tmp, "%s/cotrecovery/backup/", path);
  char* file = choose_file_menu(tmp, NULL, headers);
  if (file == NULL)
    return;
  
  if (confirm_selection("Confirm restore?", "Yes - Restore"))
    nandroid_restore(file, 1, 1, 1, 1, 1, 0);
  
  free(file);
}

void show_nandroid_delete_menu(const char* path) {
  if (ensure_path_mounted(path) != 0) {
    LOGE("Can't mount %s\n", path);
    return;
  }
  
  static const char* headers[] = { "Choose an image to delete", "", NULL };
  
  char tmp[PATH_MAX];
  sprintf(tmp, "%s/cotrecovery/backup/", path);
  char* file = choose_file_menu(tmp, NULL, headers);
  if (file == NULL)
    return;
  
  if (confirm_selection("Confirm delete?", "Yes - Delete")) {
    sprintf(tmp, "rm -rf %s", file);
    __system(tmp);
  }
  
  free(file);
}

int show_lowspace_menu(int i, const char* backup_path) {
  static char *LOWSPACE_MENU_ITEMS[] = {
    "Continue with backup",
    "View and delete old backups",
    "Cancel backup",
    NULL
  };
#define ITEM_CONTINUE_BACKUP 0
#define ITEM_VIEW_DELETE_BACKUPS 1
#define ITEM_CANCEL_BACKUP 2
  static char* headers[] = {
    "Limited space available!",
    "",
    "There may not be enough space",
    "to continue backup.",
    "",
    "What would you like to do?",
    "",
    NULL
  };
  
  for (;;) {
    int chosen_item = get_menu_selection(headers, LOWSPACE_MENU_ITEMS, 0, 0);
    switch(chosen_item) {
      case ITEM_CONTINUE_BACKUP:
      {
	static char tmp;
	ui_print("Proceeding with backup.\n");
	return 0;
      }
      case ITEM_VIEW_DELETE_BACKUPS:
	show_nandroid_delete_menu(get_primary_storage_path());
	break;
      default:
	ui_print("Cancelling backup.\n");
	return 1;
    }
  }
}

static int control_usb_storage(bool on) {
  int i = 0;
  int num = 0;
  
  for (i = 0; i < get_num_volumes(); i++) {
    Volume *v = get_device_volumes() + i;
    if (fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point)) {
      if (on) {
	vold_share_volume(v->mount_point);
      } else {
	vold_unshare_volume(v->mount_point, 1);
      }
      property_set("sys.storage.ums_enabled", on ? "1" : "0");
      num++;
    }
  }
  return num;
}

void show_mount_usb_storage_menu() {
  // Enable USB storage using vold
  if (!control_usb_storage(true))
    return;
  
  static const char* headers[] = { "USB Mass Storage device",
    "Leaving this menu unmounts",
    "your SD card from your PC.",
    "",
    NULL
  };
  
  static char* list[] = { "Unmount", NULL };
  
  for (;;) {
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item == GO_BACK || chosen_item == 0)
      break;
  }
  
  // Disable USB storage
  control_usb_storage(false);
}

int confirm_selection(const char* title, const char* confirm) {
  struct stat info;
  int ret = 0;
  
  char path[PATH_MAX];
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_NO_CONFIRM_FILE);
  ensure_path_mounted(path);
  if (0 == stat(path, &info))
    return 1;
  
  int many_confirm;
  char* confirm_str = strdup(confirm);
  const char* confirm_headers[] = { title, "  THIS CAN NOT BE UNDONE.", "", NULL };
  
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_MANY_CONFIRM_FILE);
  ensure_path_mounted(path);
  many_confirm = 0 == stat(path, &info);
  
  if (many_confirm) {
    char* items[] = { "No",
      "No",
      "No",
      "No",
      "No",
      "No",
      "No",
      confirm_str, // Yes, [7]
      "No",
      "No",
      "No",
      NULL };
      int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
      ret = (chosen_item == 7);
  } else {
    char* items[] = { "No",
      confirm_str, // Yes, [1]
      NULL };
      int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
      ret = (chosen_item == 1);
  }
  free(confirm_str);
  return ret;
}

int confirm_nandroid_backup(const char* title, const char* confirm) {
  struct stat info;
  int ret = 0;
  
  char path[PATH_MAX];
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_NO_CONFIRM_FILE);
  ensure_path_mounted(path);
  if (0 == stat(path, &info))
    return 1;
  
  int many_confirm;
  char* confirm_str = strdup(confirm);
  const char* confirm_headers[] = { title, "  THIS CAN NOT BE UNDONE.", "", NULL };
  
  sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_MANY_CONFIRM_FILE);
  ensure_path_mounted(path);
  many_confirm = 0 == stat(path, &info);
  
  if (many_confirm) {
    char* items[] = { "No",
      "No",
      "No",
      "No",
      "No",
      "No",
      "No",
      confirm_str, // Yes, [7]
      "No",
      "No",
      "No",
      NULL };
      int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
      ret = (chosen_item == 7);
  } else {
    char* items[] = { "No",
      confirm_str, // Yes, [1]
      NULL };
      int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
      ret = (chosen_item == 1);
  }
  free(confirm_str);
  return ret;
}

void show_nandroid_advanced_restore_menu(const char* path) {
  if (ensure_path_mounted(path) != 0) {
    LOGE("Can't mount sdcard\n");
    return;
  }
  
  static const char* advancedheaders[] = { "Choose an image to restore",
    "",
    "Choose an image to restore",
    "first. The next menu will",
    "show you more options.",
    "",
    NULL };
    
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/cotrecovery/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
      return;
    
    static const char* headers[] = { "Advanced Restore", "", NULL };
    
    static char* list[] = { "Restore boot",
      "Restore system",
      "Restore data",
      "Restore cache",
      "Restore sd-ext",
      "Restore wimax",
      NULL };
      
      if (0 != get_partition_device("wimax", tmp)) {
	// disable wimax restore option
	list[5] = NULL;
      }
      
      static char* confirm_restore = "Confirm restore?";
      
      int chosen_item = get_menu_selection(headers, list, 0, 0);
      switch (chosen_item) {
	case 0: {
	  if (confirm_selection(confirm_restore, "Yes - Restore boot"))
	    nandroid_restore(file, 1, 0, 0, 0, 0, 0);
	  break;
	}
	case 1: {
	  if (confirm_selection(confirm_restore, "Yes - Restore system"))
	    nandroid_restore(file, 0, 1, 0, 0, 0, 0);
	  break;
	}
	case 2: {
	  if (confirm_selection(confirm_restore, "Yes - Restore data"))
	    nandroid_restore(file, 0, 0, 1, 0, 0, 0);
	  break;
	}
	case 3: {
	  if (confirm_selection(confirm_restore, "Yes - Restore cache"))
	    nandroid_restore(file, 0, 0, 0, 1, 0, 0);
	  break;
	}
	case 4: {
	  if (confirm_selection(confirm_restore, "Yes - Restore sd-ext"))
	    nandroid_restore(file, 0, 0, 0, 0, 1, 0);
	  break;
	}
	case 5: {
	  if (confirm_selection(confirm_restore, "Yes - Restore wimax"))
	    nandroid_restore(file, 0, 0, 0, 0, 0, 1);
	  break;
	}
      }
      
      free(file);
}

static void run_dedupe_gc() {
  char path[PATH_MAX];
  char* fmt = "%s/cotrecovery/blobs";
  char* primary_path = get_primary_storage_path();
  char** extra_paths = get_extra_storage_paths();
  int i = 0;
  
  sprintf(path, fmt, primary_path);
  ensure_path_mounted(primary_path);
  nandroid_dedupe_gc(path);
  
  if (extra_paths != NULL) {
    for (i = 0; i < get_num_extra_volumes(); i++) {
      ensure_path_mounted(extra_paths[i]);
      sprintf(path, fmt, extra_paths[i]);
      nandroid_dedupe_gc(path);
    }
  }
}

static void choose_default_backup_format() {
  static const char* headers[] = { "Default Backup Format", "", NULL };
  
  int fmt = nandroid_get_default_backup_format();
  
  char **list;
  char* list_tar_default[] = { "tar (default)",
    "dup",
    "tar + gzip",
    NULL };
    char* list_dup_default[] = { "tar",
      "dup (default)",
      "tar + gzip",
      NULL };
      char* list_tgz_default[] = { "tar",
	"dup",
	"tar + gzip (default)",
	NULL };
	
	if (fmt == NANDROID_BACKUP_FORMAT_DUP) {
	  list = list_dup_default;
	} else if (fmt == NANDROID_BACKUP_FORMAT_TGZ) {
	  list = list_tgz_default;
	} else {
	  list = list_tar_default;
	}
	
	char path[PATH_MAX];
	sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), NANDROID_BACKUP_FORMAT_FILE);
	int chosen_item = get_menu_selection(headers, list, 0, 0);
	switch (chosen_item) {
	  case 0: {
	    write_string_to_file(path, "tar");
	    ui_print("Default backup format set to tar.\n");
	    break;
	  }
	  case 1: {
	    write_string_to_file(path, "dup");
	    ui_print("Default backup format set to dedupe.\n");
	    break;
	  }
	  case 2: {
	    write_string_to_file(path, "tgz");
	    ui_print("Default backup format set to tar + gzip.\n");
	    break;
	  }
	}
}

static void add_nandroid_options_for_volume(char** menu, char* path, int offset) {
  char buf[100];
  
  sprintf(buf, "backup to %s", path);
  menu[offset] = strdup(buf);
  
  sprintf(buf, "restore from %s", path);
  menu[offset + 1] = strdup(buf);
  
  sprintf(buf, "delete from %s", path);
  menu[offset + 2] = strdup(buf);
  
  sprintf(buf, "advanced restore from %s", path);
  menu[offset + 3] = strdup(buf);
}

// number of actions added for each volume by add_nandroid_options_for_volume()
// these go on top of menu list
#define NANDROID_ACTIONS_NUM 4
// number of fixed bottom entries after volume actions
#define NANDROID_FIXED_ENTRIES 2

int show_nandroid_menu() {
  char* primary_path = get_primary_storage_path();
  char** extra_paths = get_extra_storage_paths();
  int num_extra_volumes = get_num_extra_volumes();
  int i = 0, offset = 0, chosen_item = 0;
  char* chosen_path = NULL;
  int action_entries_num = (num_extra_volumes + 1) * NANDROID_ACTIONS_NUM;
  
  static const char* headers[] = { "Nandroid", "", NULL };
  
  // (MAX_NUM_MANAGED_VOLUMES + 1) for primary_path (/sdcard)
  // + 1 for extra NULL entry
  static char* list[((MAX_NUM_MANAGED_VOLUMES + 1) * NANDROID_ACTIONS_NUM) + NANDROID_FIXED_ENTRIES + 1];
  
  // actions for primary_path
  add_nandroid_options_for_volume(list, primary_path, offset);
  offset += NANDROID_ACTIONS_NUM;
  
  // actions for voldmanaged volumes
  if (extra_paths != NULL) {
    for (i = 0; i < num_extra_volumes; i++) {
      add_nandroid_options_for_volume(list, extra_paths[i], offset);
      offset += NANDROID_ACTIONS_NUM;
    }
  }
  // fixed bottom entries
  list[offset] = "free unused backup data";
  list[offset + 1] = "choose default backup format";
  offset += NANDROID_FIXED_ENTRIES;
  
  #ifdef RECOVERY_EXTEND_NANDROID_MENU
  extend_nandroid_menu(list, offset, sizeof(list) / sizeof(char*));
  offset++;
  #endif
  
  // extra NULL for GO_BACK
  list[offset] = NULL;
  offset++;
  
  for (;;) {
    chosen_item = get_filtered_menu_selection(headers, list, 0, 0, offset);
    if (chosen_item == GO_BACK || chosen_item == REFRESH)
      break;
    
    // fixed bottom entries
    if (chosen_item == action_entries_num) {
      run_dedupe_gc();
    } else if (chosen_item == (action_entries_num + 1)) {
      choose_default_backup_format();
    } else if (chosen_item < action_entries_num) {
      // get nandroid volume actions path
      if (chosen_item < NANDROID_ACTIONS_NUM) {
	chosen_path = primary_path;
      } else if (extra_paths != NULL) {
	chosen_path = extra_paths[(chosen_item / NANDROID_ACTIONS_NUM) - 1];
      }
      // process selected nandroid action
      int chosen_subitem = chosen_item % NANDROID_ACTIONS_NUM;
      switch (chosen_subitem) {
	case 0: {
	  char backup_path[PATH_MAX];
	  time_t t = time(NULL);
	  struct tm *tmp = localtime(&t);
	  if (tmp == NULL) {
	    struct timeval tp;
	    gettimeofday(&tp, NULL);
	    sprintf(backup_path, "%s/cotrecovery/backup/%ld", chosen_path, tp.tv_sec);
	  } else {
	    char path_fmt[PATH_MAX];
	    strftime(path_fmt, sizeof(path_fmt), "cotrecovery/backup/%F.%H.%M.%S", tmp);
	    // this sprintf results in:
	    // cotrecovery/backup/%F.%H.%M.%S (time values are populated too)
	    sprintf(backup_path, "%s/%s", chosen_path, path_fmt);
	  }
	  nandroid_backup(backup_path);
	  break;
	}
	case 1:
	  show_nandroid_restore_menu(chosen_path);
	  break;
	case 2:
	  show_nandroid_delete_menu(chosen_path);
	  break;
	case 3:
	  show_nandroid_advanced_restore_menu(chosen_path);
	  break;
	default:
	  break;
      }
    } else {
      #ifdef RECOVERY_EXTEND_NANDROID_MENU
      handle_nandroid_menu(action_entries_num + NANDROID_FIXED_ENTRIES, chosen_item);
      #endif
      goto out;
    }
  }
  out:
  for (i = 0; i < action_entries_num; i++)
    free(list[i]);
  return chosen_item;
}

int can_partition(const char* volume) {
  if (is_data_media_volume_path(volume))
    return 0;
  
  Volume *vol = volume_for_path(volume);
  if (vol == NULL) {
    LOGI("Can't format unknown volume: %s\n", volume);
    return 0;
  }
  if (strcmp(vol->fs_type, "auto") != 0) {
    LOGI("Can't partition non-vfat: %s (%s)\n", volume, vol->fs_type);
    return 0;
  }
  // do not allow partitioning of a device that isn't mmcblkX or mmcblkXp1
  // needed with new vold managed volumes and virtual device path links
  int vol_len;
  char *device = NULL;
  if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL) {
    device = vol->blk_device;
  } else if (vol->blk_device2 != NULL && strstr(vol->blk_device2, "/dev/block/mmcblk") != NULL) {
    device = vol->blk_device2;
  } else {
    LOGI("Can't partition non mmcblk device: %s\n", vol->blk_device);
    return 0;
  }
  
  vol_len = strlen(device);
  if (device[vol_len - 2] == 'p' && device[vol_len - 1] != '1') {
    LOGI("Can't partition unsafe device: %s\n", device);
    return 0;
  }
  
  return 1;
}

void flash_kernel_default (const char* kernel_path) {
  static char* headers[] = {  "Flash kernel image",
    NULL
  };
  if (ensure_path_mounted(kernel_path) != 0) {
    LOGE ("Can't mount %s\n", kernel_path);
    return;
  }
  char tmp[PATH_MAX];
  sprintf(tmp, "%s/cotrecovery/.kernel_bak/", kernel_path);
  //without this check, we get 2 errors in log: "directory not found":
  if (access(tmp, F_OK) != -1) {
    //folder exists, but could be empty!
    char* kernel_file = choose_file_menu(tmp, ".img", headers);
    if (kernel_file == NULL) {
      //either no valid files found or we selected no files by pressing back menu
      if (no_files_found == 1) {
	//0 valid files to select
	ui_print("No *.img files in %s\n", tmp);
      }
      return;
    }
    static char* confirm_install = "Confirm flash kernel?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Flash %s", basename(kernel_file));
    if (confirm_selection(confirm_install, confirm)) {
      char tmp[PATH_MAX];
      sprintf(tmp, "kernel-restore.sh %s %s", kernel_file, kernel_path);
      __system(tmp);
    }
  } else {
    ui_print("%s not found.\n", tmp);
    return;
  }
}

void show_efs_menu() {
  static char* headers[] = { "EFS/Boot Backup & Restore",
    "",
    NULL
  };
  
  static char* list[] = { "Backup /boot to /sdcard",
    "Flash /boot from /sdcard",
    "Backup /efs to /sdcard",
    "Restore /efs from /sdcard",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
  };
  
  char *other_sd = NULL;
  if (volume_for_path("/emmc") != NULL) {
    other_sd = "/emmc";
    list[4] = "Backup /boot to Internal sdcard";
    list[5] = "Flash /boot from Internal sdcard";
    list[6] = "Backup /efs to Internal sdcard";
    list[7] = "Restore /efs from Internal sdcard";
  } else if (volume_for_path("/external_sd") != NULL) {
    other_sd = "/external_sd";
    list[4] = "Backup /boot to External sdcard";
    list[5] = "Flash /boot from External sdcard";
    list[6] = "Backup /efs to External sdcard";
    list[7] = "Restore /efs from External sdcard";
  }
  
  for (;;) {
    int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
    if (chosen_item == GO_BACK)
      break;
    switch (chosen_item)
    {
      case 0:
	if (ensure_path_mounted("/sdcard") != 0) {
	  ui_print("Can't mount /sdcard\n");
	  break;
	}
	__system("kernel-backup.sh /sdcard");
	break;
      
      case 1:
	flash_kernel_default("/sdcard");
	break;
      
      case 2:
	if (ensure_path_mounted("/sdcard") != 0) {
	  ui_print("Can't mount /sdcard\n");
	  break;
	}
	ensure_path_unmounted("/efs");
	__system("efs-backup.sh /sdcard");
	break;
	
      case 3:
	if (ensure_path_mounted("/sdcard") != 0) {
	  ui_print("Can't mount /sdcard\n");
	  break;
	}
	ensure_path_unmounted("/efs");
	if (access("/sdcard/cotrecovery/.efsbackup/efs.img", F_OK ) != -1) {
	  if (confirm_selection("Confirm?", "Yes - Restore EFS")) {
	    __system("efs-restore.sh /sdcard");
	  }
	} else {
	  LOGE("No efs.img backup found in /sdcard.\n");
	}
	break;
	
      case 4:
	{
	  if (ensure_path_mounted(other_sd) != 0) {
	    ui_print("Can't mount %s\n", other_sd);
	    break;
	  }
	  char tmp[PATH_MAX];
	  sprintf(tmp, "kernel-backup.sh %s", other_sd);
	  __system(tmp);
	}
	break;
	
      case 5:
	flash_kernel_default(other_sd);
	break;
		
      case 6:
      {
	if (ensure_path_mounted(other_sd) != 0) {
	  ui_print("Can't mount %s\n", other_sd);
	  break;
	}
	ensure_path_unmounted("/efs");
	char tmp[PATH_MAX];
	sprintf(tmp, "efs-backup.sh %s", other_sd);
	__system(tmp);
      }
      break;
      
      case 7:
      {
	if (ensure_path_mounted(other_sd) != 0) {
	  ui_print("Can't mount %s\n", other_sd);
	  break;
	}
	ensure_path_unmounted("/efs");
	char filename[PATH_MAX];
	sprintf(filename, "%s/cotrecovery/.efsbackup/efs.img", other_sd);
	if (access(filename, F_OK ) != -1) {
	  if (confirm_selection("Confirm?", "Yes - Restore EFS")) {
	    char tmp[PATH_MAX];
	    sprintf(tmp, "efs-restore.sh %s", other_sd);
	    __system(tmp);
	  }
	} else {
	  LOGE("No efs.img backup found in %s\n", other_sd);
	}
      }
      break;
    }
  }
}

void write_fstab_root(char *path, FILE *file) {
  Volume *vol = volume_for_path(path);
  if (vol == NULL) {
    LOGW("Unable to get recovery.fstab info for %s during fstab generation!\n", path);
    return;
  }
  
  char device[200];
  if (vol->blk_device[0] != '/')
    get_partition_device(vol->blk_device, device);
  else
    strcpy(device, vol->blk_device);
  
  fprintf(file, "%s ", device);
  fprintf(file, "%s ", path);
  // special case rfs cause auto will mount it as vfat on samsung.
  fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab() {
  struct stat info;
  __system("touch /etc/mtab");
  FILE *file = fopen("/etc/fstab", "w");
  if (file == NULL) {
    LOGW("Unable to create /etc/fstab!\n");
    return;
  }
  Volume *vol = volume_for_path("/boot");
  if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
    write_fstab_root("/boot", file);
  write_fstab_root("/cache", file);
  write_fstab_root("/data", file);
  write_fstab_root("/datadata", file);
  write_fstab_root("/emmc", file);
  write_fstab_root("/system", file);
  write_fstab_root("/sdcard", file);
  write_fstab_root("/sd-ext", file);
  write_fstab_root("/external_sd", file);
  fclose(file);
  LOGI("Completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
  ui_print("Checking %s...\n", path);
  ensure_path_unmounted(path);
  if (0 == ensure_path_mounted(path)) {
    ensure_path_unmounted(path);
    return 0;
  }
  
  Volume *vol = volume_for_path(path);
  if (vol == NULL) {
    LOGE("Unable process volume! Skipping...\n");
    return 0;
  }
  
  ui_print("%s may be rfs. Checking...\n", path);
  char tmp[PATH_MAX];
  sprintf(tmp, "mount -t rfs %s %s", vol->blk_device, path);
  int ret = __system(tmp);
  printf("%d\n", ret);
  return ret == 0 ? 1 : 0;
}

void process_volumes() {
  create_fstab();
  
  if (is_data_media()) {
    setup_data_media();
  }
  
  return;
  
  // dead code.
  if (device_flash_type() != BML)
    return;
  
  ui_print("Checking for ext4 partitions...\n");
  int ret = 0;
  ret = bml_check_volume("/system");
  ret |= bml_check_volume("/data");
  if (has_datadata())
    ret |= bml_check_volume("/datadata");
  ret |= bml_check_volume("/cache");
  
  if (ret == 0) {
    ui_print("Done!\n");
    return;
  }
  
  char backup_path[PATH_MAX];
  time_t t = time(NULL);
  char backup_name[PATH_MAX];
  struct timeval tp;
  gettimeofday(&tp, NULL);
  sprintf(backup_name, "before-ext4-convert-%ld", tp.tv_sec);
  sprintf(backup_path, "%s/cotrecovery/backup/%s", get_primary_storage_path(), backup_name);
  
  ui_set_show_text(1);
  ui_print("Filesystems need to be converted to ext4.\n");
  ui_print("A backup and restore will now take place.\n");
  ui_print("If anything goes wrong, your backup will be\n");
  ui_print("named %s. Try restoring it\n", backup_name);
  ui_print("in case of error.\n");
  
  nandroid_backup(backup_path);
  nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
  ui_set_show_text(0);
}

void handle_failure(int ret) {
  if (ret == 0)
    return;
  if (0 != ensure_path_mounted(get_primary_storage_path()))
    return;
  mkdir("/sdcard/cotrecovery", S_IRWXU | S_IRWXG | S_IRWXO);
  __system("cp /tmp/recovery.log /sdcard/cotrecovery/recovery.log");
  ui_print("/tmp/recovery.log was copied to /sdcard/cotrecovery/recovery.log. Please open COT Manager to report the issue.\n");
}

int is_path_mounted(const char* path) {
  Volume* v = volume_for_path(path);
  if (v == NULL) {
    return 0;
  }
  if (strcmp(v->fs_type, "ramdisk") == 0) {
    // the ramdisk is always mounted.
    return 1;
  }
  
  if (scan_mounted_volumes() < 0)
    return 0;
  
  const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
  if (mv) {
    // volume is already mounted
    return 1;
  }
  return 0;
}

int has_datadata() {
  Volume *vol = volume_for_path("/datadata");
  return vol != NULL;
}

int volume_main(int argc, char **argv) {
  load_volume_table();
  return 0;
}

int verify_root_and_recovery() {
  if (ensure_path_mounted("/system") != 0)
    return 0;
  
  // none of these options should get a "Go Back" option
  int old_val = ui_get_showing_back_button();
  ui_set_showing_back_button(0);
  
  int ret = 0;
  struct stat st;
  // check to see if install-recovery.sh is going to clobber recovery
  // install-recovery.sh is also used to run the su daemon on stock rom for 4.3+
  // so verify that doesn't exist...
  if (0 != lstat("/system/etc/.installed_su_daemon", &st)) {
    // check install-recovery.sh exists and is executable
    if (0 == lstat("/system/etc/install-recovery.sh", &st)) {
      if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
	ui_show_text(1);
	ret = 1;
	if (confirm_selection("ROM may flash stock recovery on boot. Fix?", "Yes - Disable recovery flash")) {
	  __system("chmod -x /system/etc/install-recovery.sh");
	}
      }
    }
  }
  
  
  int exists = 0;
  if (0 == lstat("/system/bin/su", &st)) {
    exists = 1;
    if (S_ISREG(st.st_mode)) {
      if ((st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
	ui_show_text(1);
	ret = 1;
	if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/bin/su)")) {
	  __system("chmod 6755 /system/bin/su");
	}
      }
    }
  }
  
  if (0 == lstat("/system/xbin/su", &st)) {
    exists = 1;
    if (S_ISREG(st.st_mode)) {
      if ((st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
	ui_show_text(1);
	ret = 1;
	if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/xbin/su)")) {
	  __system("chmod 6755 /system/xbin/su");
	}
      }
    }
  }
  
  if (!exists) {
    ui_show_text(1);
    ret = 1;
    if (confirm_selection("Root access is missing. Root device?", "Yes - Root device (/system/xbin/su)")) {
      __system("/sbin/install-su.sh");
    }
  }
  
  ensure_path_unmounted("/system");
  ui_set_showing_back_button(old_val);
  return ret;
}

// check if file or folder exists
int file_found(const char* filename) {
    struct stat s;
    if (strncmp(filename, "/sbin/", 6) != 0 && strncmp(filename, "/res/", 5) != 0 && strncmp(filename, "/tmp/", 5) != 0) {
        // do not try to mount ramdisk, else it will error "unknown volume for path..."
        ensure_path_mounted(filename);
    }
    if (0 == stat(filename, &s))
        return 1;

    return 0;
}