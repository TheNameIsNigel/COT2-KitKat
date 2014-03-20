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

#include "mmcutils/mmcutils.h"
#include "voldclient/voldclient.h"

#include "adb_install.h"

extern struct selabel_handle *sehandle;

typedef struct _saved_log_file {
    char* name;
    struct stat st;
    unsigned char* data;
    struct _saved_log_file* next;
} saved_log_file;

static const char *CACHE_LOG_DIR = "/cache/recovery";
static const char *CACHE_ROOT = "/cache";

int erase_volume(const char *volume) {
    bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);

    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();

    saved_log_file* head = NULL;

    if (is_cache) {
        // If we're reformatting /cache, we load any
        // "/cache/recovery/last*" files into memory, so we can restore
        // them after the reformat.

        ensure_path_mounted(volume);

        DIR* d;
        struct dirent* de;
        d = opendir(CACHE_LOG_DIR);
        if (d) {
            char path[PATH_MAX];
            strcpy(path, CACHE_LOG_DIR);
            strcat(path, "/");
            int path_len = strlen(path);
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "last", 4) == 0) {
                    saved_log_file* p = (saved_log_file*) malloc(sizeof(saved_log_file));
                    strcpy(path+path_len, de->d_name);
                    p->name = strdup(path);
                    if (stat(path, &(p->st)) == 0) {
                        // truncate files to 512kb
                        if (p->st.st_size > (1 << 19)) {
                            p->st.st_size = 1 << 19;
                        }
                        p->data = (unsigned char*) malloc(p->st.st_size);
                        FILE* f = fopen(path, "rb");
                        fread(p->data, 1, p->st.st_size, f);
                        fclose(f);
                        p->next = head;
                        head = p;
                    } else {
                        free(p);
                    }
                }
            }
            closedir(d);
        } else {
            if (errno != ENOENT) {
                printf("opendir failed: %s\n", strerror(errno));
            }
        }
    }

    ui_print("Formatting %s...\n", volume);

    ensure_path_unmounted(volume);
    int result = format_volume(volume);

    if (is_cache) {
        while (head) {
            FILE* f = fopen_path(head->name, "wb");
            if (f) {
                fwrite(head->data, 1, head->st.st_size, f);
                fclose(f);
                chmod(head->name, head->st.st_mode);
                chown(head->name, head->st.st_uid, head->st.st_gid);
            }
            free(head->name);
            free(head->data);
            saved_log_file* temp = head->next;
            free(head);
            head = temp;
        }

        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
        copy_logs();
    }
    ui_dyn_background();
    return result;
}

void wipe_data(int confirm) {
    if (confirm && !confirm_selection( "Confirm wipe of all user data?", "Yes - Wipe all user data"))
        return;

    ui_print("\n-- Wiping data...\n");
    device_wipe_data();
    erase_volume("/data");
    erase_volume("/cache");
    if (has_datadata()) {
        erase_volume("/datadata");
    }
    erase_volume("/sd-ext");
    erase_volume(get_android_secure_path());
    ui_print("Data wipe complete.\n");
}

void erase_cache(int orscallback) {
  if (orscallback) {
    if (orswipeprompt && !confirm_selection("Confirm wipe?", "Yes - Wipe Cache")) {
      ui_print("Skipping cache wipe...\n");
      return;
    }
  } else if (!confirm_selection("Confirm wipe?", "Yes - Wipe Cache")) {
    return;
  }
  ui_print("\n-- Wiping cache...\n");
  erase_volume("/cache");
  ui_print("Cache wipe complete.");
  if (!ui_text_visible()) return;
  return;
}

void erase_dalvik_cache(int orscallback) {
  if (orscallback) {
    if (orswipeprompt && !confirm_selection("Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
      ui_print("Skipping dalvik cache wipe...\n");
      return;
    }
  } else if (!confirm_selection("Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
    return;
  }
  if (0 != ensure_path_mounted("/data"))
    return;
  ensure_path_mounted("/sd-ext");
  ensure_path_mounted("/cache");
  __system("rm -r /data/dalvik-cache");
  __system("rm -r /cache/dalvik-cache");
  __system("rm -r /sd-ext/dalvik-cache");
  ui_print("Dalvik Cache wiped.\n");
  ensure_path_unmounted("/data");
  return;
}

void wipe_all(int orscallback) {
  if (orscallback) {
    if (orswipeprompt && !confirm_selection("Confirm wipe all?", "Yes - Wipe All")) {
      ui_print("Skipping full wipe...\n");
      return;
    }
  } else if (!confirm_selection("Confirm wipe all?", "Yes - Wipe All")) {
    return;
  }
  ui_print("\n-- Wiping system, data, cache...\n");
  erase_volume("/system");
  erase_volume("/data");
  erase_volume("/cache");
  ui_print("\nFull wipe complete!\n");
  if (!ui_text_visible()) return;
  return;
}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"
extern void reset_ext4fs_info();

typedef struct {
  char mount[255];
  char unmount[255];
  char path[PATH_MAX];
} MountMenuEntry;

typedef struct {
  char txt[255];
  char path[PATH_MAX];
  char type[255];
} FormatMenuEntry;

typedef struct {
  char *name;
  int can_mount;
  int can_format;
} MFMatrix;

MFMatrix get_mnt_fmt_capabilities(char *fs_type, char *mount_point) {
  MFMatrix mfm = { mount_point, 1, 1 };
  
  const int NUM_FS_TYPES = 5;
  MFMatrix *fs_matrix = malloc(NUM_FS_TYPES * sizeof(MFMatrix));
  // Defined capabilities:   fs_type     mnt fmt
  fs_matrix[0] = (MFMatrix){ "bml",       0,  1 };
  fs_matrix[1] = (MFMatrix){ "datamedia", 0,  1 };
  fs_matrix[2] = (MFMatrix){ "emmc",      0,  1 };
  fs_matrix[3] = (MFMatrix){ "mtd",       0,  0 };
  fs_matrix[4] = (MFMatrix){ "ramdisk",   0,  0 };
  
  const int NUM_MNT_PNTS = 6;
  MFMatrix *mp_matrix = malloc(NUM_MNT_PNTS * sizeof(MFMatrix));
  // Defined capabilities:   mount_point   mnt fmt
  mp_matrix[0] = (MFMatrix){ "/misc",       0,  0 };
  mp_matrix[1] = (MFMatrix){ "/radio",      0,  0 };
  mp_matrix[2] = (MFMatrix){ "/bootloader", 0,  0 };
  mp_matrix[3] = (MFMatrix){ "/recovery",   0,  0 };
  mp_matrix[4] = (MFMatrix){ "/efs",        0,  0 };
  mp_matrix[5] = (MFMatrix){ "/wimax",      0,  0 };
  
  int i;
  for (i = 0; i < NUM_FS_TYPES; i++) {
    if (strcmp(fs_type, fs_matrix[i].name) == 0) {
      mfm.can_mount = fs_matrix[i].can_mount;
      mfm.can_format = fs_matrix[i].can_format;
    }
  }
  for (i = 0; i < NUM_MNT_PNTS; i++) {
    if (strcmp(mount_point, mp_matrix[i].name) == 0) {
      mfm.can_mount = mp_matrix[i].can_mount;
      mfm.can_format = mp_matrix[i].can_format;
    }
  }
  
  free(fs_matrix);
  free(mp_matrix);
  
  // User-defined capabilities
  char *custom_mp;
  char custom_forbidden_mount[PROPERTY_VALUE_MAX];
  char custom_forbidden_format[PROPERTY_VALUE_MAX];
  property_get("ro.cwm.forbid_mount", custom_forbidden_mount, "");
  property_get("ro.cwm.forbid_format", custom_forbidden_format, "");
  
  custom_mp = strtok(custom_forbidden_mount, ",");
  while (custom_mp != NULL) {
    if (strcmp(mount_point, custom_mp) == 0) {
      mfm.can_mount = 0;
    }
    custom_mp = strtok(NULL, ",");
  }
  
  custom_mp = strtok(custom_forbidden_format, ",");
  while (custom_mp != NULL) {
    if (strcmp(mount_point, custom_mp) == 0) {
      mfm.can_format = 0;
    }
    custom_mp = strtok(NULL, ",");
  }
  
  return mfm;
}


int format_device(const char *device, const char *path, const char *fs_type) {
  if (is_data_media_volume_path(path)) {
    return format_unknown_device(NULL, path, NULL);
  }
  if (strstr(path, "/data") == path && is_data_media()) {
    return format_unknown_device(NULL, path, NULL);
  }
  
  Volume* v = volume_for_path(path);
  if (v == NULL) {
    // silent failure for sd-ext
    if (strcmp(path, "/sd-ext") != 0)
      LOGE("unknown volume '%s'\n", path);
    return -1;
  }
  
  if (strcmp(fs_type, "ramdisk") == 0) {
    // you can't format the ramdisk.
    LOGE("can't format_volume \"%s\"", path);
    return -1;
  }
  
  if (strcmp(fs_type, "rfs") == 0) {
    if (ensure_path_unmounted(path) != 0) {
      LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
      return -1;
    }
    if (0 != format_rfs_device(device, path)) {
      LOGE("format_volume: format_rfs_device failed on %s\n", device);
      return -1;
    }
    return 0;
  }
  
  if (strcmp(v->mount_point, path) != 0) {
    return format_unknown_device(v->blk_device, path, NULL);
  }
  
  if (ensure_path_unmounted(path) != 0) {
    LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
    return -1;
  }
  
  if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
    mtd_scan_partitions();
    const MtdPartition* partition = mtd_find_partition_by_name(device);
    if (partition == NULL) {
      LOGE("format_volume: no MTD partition \"%s\"\n", device);
      return -1;
    }
    
    MtdWriteContext *write = mtd_write_partition(partition);
    if (write == NULL) {
      LOGW("format_volume: can't open MTD \"%s\"\n", device);
      return -1;
    } else if (mtd_erase_blocks(write, -1) == (off_t) - 1) {
      LOGW("format_volume: can't erase MTD \"%s\"\n", device);
      mtd_write_close(write);
      return -1;
    } else if (mtd_write_close(write)) {
      LOGW("format_volume: can't close MTD \"%s\"\n", device);
      return -1;
    }
    return 0;
  }
  
  if (strcmp(fs_type, "ext4") == 0) {
    int length = 0;
    if (strcmp(v->fs_type, "ext4") == 0) {
      // Our desired filesystem matches the one in fstab, respect v->length
      length = v->length;
    }
    reset_ext4fs_info();
    int result = make_ext4fs(device, length, v->mount_point, sehandle);
    if (result != 0) {
      LOGE("format_volume: make_ext4fs failed on %s\n", device);
      return -1;
    }
    return 0;
  }
  #ifdef USE_F2FS
  if (strcmp(fs_type, "f2fs") == 0) {
    int result = make_f2fs_main(device, v->mount_point);
    if (result != 0) {
      LOGE("format_volume: mkfs.f2f2 failed on %s\n", device);
      return -1;
    }
    return 0;
  }
  #endif
  return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type) {
  LOGI("Formatting unknown device.\n");
  
  if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
    return erase_raw_partition(fs_type, device);
  
  // if this is SDEXT:, don't worry about it if it does not exist.
  if (0 == strcmp(path, "/sd-ext")) {
    struct stat st;
    Volume *vol = volume_for_path("/sd-ext");
    if (vol == NULL || 0 != stat(vol->blk_device, &st)) {
      LOGI("No app2sd partition found. Skipping format of /sd-ext.\n");
      return 0;
    }
  }
  
  if (NULL != fs_type) {
    if (strcmp("ext3", fs_type) == 0) {
      LOGI("Formatting ext3 device.\n");
      if (0 != ensure_path_unmounted(path)) {
	LOGE("Error while unmounting %s.\n", path);
	return -12;
      }
      return format_ext3_device(device);
    }
    
    if (strcmp("ext2", fs_type) == 0) {
      LOGI("Formatting ext2 device.\n");
      if (0 != ensure_path_unmounted(path)) {
	LOGE("Error while unmounting %s.\n", path);
	return -12;
      }
      return format_ext2_device(device);
    }
  }
  
  if (0 != ensure_path_mounted(path)) {
    ui_print("Error mounting %s!\n", path);
    ui_print("Skipping format...\n");
    return 0;
  }
  
  char tmp[PATH_MAX];
  if (strcmp(path, "/data") == 0) {
    sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
    __system(tmp);
    // if the /data/media sdcard has already been migrated for android 4.2,
    // prevent the migration from happening again by writing the .layout_version
    struct stat st;
    if (0 == lstat("/data/media/0", &st)) {
      char* layout_version = "2";
      FILE* f = fopen("/data/.layout_version", "wb");
      if (NULL != f) {
	fwrite(layout_version, 1, 2, f);
	fclose(f);
      } else {
	LOGI("error opening /data/.layout_version for write.\n");
      }
    } else {
      LOGI("/data/media/0 not found. migration may occur.\n");
    }
  } else {
    sprintf(tmp, "rm -rf %s/*", path);
    __system(tmp);
    sprintf(tmp, "rm -rf %s/.*", path);
    __system(tmp);
  }
  
  ensure_path_unmounted(path);
  return 0;
}

void partition_sdcard(const char* volume) {
  if (!can_partition(volume)) {
    ui_print("Can't partition device: %s\n", volume);
    return;
  }
  
  static char* ext_sizes[] = { "128M",
    "256M",
    "512M",
    "1024M",
    "2048M",
    "4096M",
    NULL };
    
    static char* swap_sizes[] = { "0M",
      "32M",
      "64M",
      "128M",
      "256M",
      NULL };
      
      static char* partition_types[] = { "ext3",
	"ext4",
	NULL };
	
	static const char* ext_headers[] = { "Ext Size", "", NULL };
	static const char* swap_headers[] = { "Swap Size", "", NULL };
	static const char* fstype_headers[] = { "Partition Type", "", NULL };
	
	int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
	if (ext_size < 0)
	  return;
	
	int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
	if (swap_size < 0)
	  return;
	
	int partition_type = get_menu_selection(fstype_headers, partition_types, 0, 0);
	if (partition_type < 0)
	  return;
	
	char sddevice[256];
	Volume *vol = volume_for_path(volume);
	
	// can_partition() ensured either blk_device or blk_device2 has /dev/block/mmcblk format
	if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL)
	  strcpy(sddevice, vol->blk_device);
	else
	  strcpy(sddevice, vol->blk_device2);
	
	// we only want the mmcblk, not the partition
	sddevice[strlen("/dev/block/mmcblkX")] = '\0';
	char cmd[PATH_MAX];
	setenv("SDPATH", sddevice, 1);
	sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], partition_types[partition_type]);
	ui_print("Partitioning SD Card... please wait...\n");
	if (0 == __system(cmd))
	  ui_print("Done!\n");
	else
	  ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
}

int show_partition_menu() {
  static const char* headers[] = { "Storage Management", "", NULL };
  
  static char* confirm_format = "Confirm format?";
  static char* confirm = "Yes - Format";
  char confirm_string[255];
  
  static MountMenuEntry* mount_menu = NULL;
  static FormatMenuEntry* format_menu = NULL;
  static char* list[256];
  
  int i, mountable_volumes, formatable_volumes;
  int num_volumes;
  int chosen_item = 0;
  
  num_volumes = get_num_volumes();
  
  if (!num_volumes)
    return 0;
  
  mountable_volumes = 0;
  formatable_volumes = 0;
  
  mount_menu = malloc(num_volumes * sizeof(MountMenuEntry));
  format_menu = malloc(num_volumes * sizeof(FormatMenuEntry));
  
  for (i = 0; i < num_volumes; i++) {
    Volume* v = get_device_volumes() + i;
    
    if (fs_mgr_is_voldmanaged(v) && !vold_is_volume_available(v->mount_point)) {
      continue;
    }
    
    MFMatrix mfm = get_mnt_fmt_capabilities(v->fs_type, v->mount_point);
    
    if (mfm.can_mount) {
      sprintf(mount_menu[mountable_volumes].mount, "mount %s", v->mount_point);
      sprintf(mount_menu[mountable_volumes].unmount, "unmount %s", v->mount_point);
      sprintf(mount_menu[mountable_volumes].path, "%s", v->mount_point);
      ++mountable_volumes;
    }
    if (mfm.can_format) {
      sprintf(format_menu[formatable_volumes].txt, "format %s", v->mount_point);
      sprintf(format_menu[formatable_volumes].path, "%s", v->mount_point);
      sprintf(format_menu[formatable_volumes].type, "%s", v->fs_type);
      ++formatable_volumes;
    }
  }
  
  for (;;) {
    for (i = 0; i < mountable_volumes; i++) {
      MountMenuEntry* e = &mount_menu[i];
      if (is_path_mounted(e->path))
	list[i] = e->unmount;
      else
	list[i] = e->mount;
    }
    
    for (i = 0; i < formatable_volumes; i++) {
      FormatMenuEntry* e = &format_menu[i];
      list[mountable_volumes + i] = e->txt;
    }
    
    if (!is_data_media()) {
      list[mountable_volumes + formatable_volumes] = "mount USB storage";
      list[mountable_volumes + formatable_volumes + 1] = '\0';
    } else {
      list[mountable_volumes + formatable_volumes] = "format /data and /data/media (/sdcard)";
      list[mountable_volumes + formatable_volumes + 1] = "mount USB storage";
      list[mountable_volumes + formatable_volumes + 2] = '\0';
    }
    
    chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item == GO_BACK || chosen_item == REFRESH)
      break;
    if (chosen_item == (mountable_volumes + formatable_volumes)) {
      if (!is_data_media()) {
	show_mount_usb_storage_menu();
      } else {
	if (!confirm_selection("format /data and /data/media (/sdcard)", confirm))
	  continue;
	ignore_data_media_workaround(1);
	ui_print("Formatting /data...\n");
	if (0 != format_volume("/data"))
	  ui_print("Error formatting /data!\n");
	else
	  ui_print("Done.\n");
	ignore_data_media_workaround(0);
      }
    } else if (is_data_media() && chosen_item == (mountable_volumes + formatable_volumes + 1)) {
      show_mount_usb_storage_menu();
    } else if (chosen_item < mountable_volumes) {
      MountMenuEntry* e = &mount_menu[chosen_item];
      
      if (is_path_mounted(e->path)) {
	ignore_data_media_workaround(1);
	if (0 != ensure_path_unmounted(e->path))
	  ui_print("Error unmounting %s!\n", e->path);
	ignore_data_media_workaround(0);
      } else {
	if (0 != ensure_path_mounted(e->path))
	  ui_print("Error mounting %s!\n", e->path);
      }
    } else if (chosen_item < (mountable_volumes + formatable_volumes)) {
      chosen_item = chosen_item - mountable_volumes;
      FormatMenuEntry* e = &format_menu[chosen_item];
      
      sprintf(confirm_string, "%s - %s", e->path, confirm_format);
      
      // support user choice fstype when formatting external storage
      // ensure fstype==auto because most devices with internal vfat storage cannot be formatted to other types
      if (strcmp(e->type, "auto") == 0) {
	format_sdcard(e->path);
	continue;
      }
      
      if (!confirm_selection(confirm_string, confirm))
	continue;
      ui_print("Formatting %s...\n", e->path);
      if (0 != format_volume(e->path))
	ui_print("Error formatting %s!\n", e->path);
      else
	ui_print("Done.\n");
    }
  }
  
  free(mount_menu);
  free(format_menu);
  return chosen_item;
}

void format_sdcard(const char* volume) {
  if (is_data_media_volume_path(volume))
    return;
  
  Volume *v = volume_for_path(volume);
  if (v == NULL || strcmp(v->fs_type, "auto") != 0)
    return;
  if (!fs_mgr_is_voldmanaged(v) && !can_partition(volume))
    return;
  
  const char* headers[] = { "Format device:", volume, "", NULL };
  
  static char* list[] = { "default",
    "vfat",
    "exfat",
    "ntfs",
    "ext4",
    "ext3",
    "ext2",
    NULL };
    
    int ret = -1;
    char cmd[PATH_MAX];
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item < 0) // REFRESH or GO_BACK
      return;
    if (!confirm_selection("Confirm formatting?", "Yes - Format device"))
      return;
    
    if (ensure_path_unmounted(v->mount_point) != 0)
      return;
    
    switch (chosen_item) {
      case 0:
	ret = format_volume(v->mount_point);
	break;
      case 1:
      case 2:
      case 3:
      case 4: {
	if (fs_mgr_is_voldmanaged(v)) {
	  ret = vold_custom_format_volume(v->mount_point, list[chosen_item], 1) == CommandOkay ? 0 : -1;
	} else if (strcmp(list[chosen_item], "vfat") == 0) {
	  sprintf(cmd, "/sbin/newfs_msdos -F 32 -O android -c 8 %s", v->blk_device);
	  ret = __system(cmd);
	} else if (strcmp(list[chosen_item], "exfat") == 0) {
	  sprintf(cmd, "/sbin/mkfs.exfat %s", v->blk_device);
	  ret = __system(cmd);
	} else if (strcmp(list[chosen_item], "ntfs") == 0) {
	  sprintf(cmd, "/sbin/mkntfs -f %s", v->blk_device);
	  ret = __system(cmd);
	} else if (strcmp(list[chosen_item], "ext4") == 0) {
	  ret = make_ext4fs(v->blk_device, v->length, volume, sehandle);
	}
	break;
      }
      case 5:
      case 6: {
	// workaround for new vold managed volumes that cannot be recognized by prebuilt ext2/ext3 bins
	const char *device = v->blk_device2;
	if (device == NULL)
	  device = v->blk_device;
	ret = format_unknown_device(device, v->mount_point, list[chosen_item]);
	break;
      }
    }
    
    if (ret)
      ui_print("Could not format %s (%s)\n", volume, list[chosen_item]);
    else
      ui_print("Done formatting %s (%s)\n", volume, list[chosen_item]);
}
