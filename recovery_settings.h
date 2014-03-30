/*
    Define path for all recovery settings files here
*/

#include "cutils/properties.h"  // PROPERTY_VALUE_MAX
#include <linux/limits.h>   // PATH_MAX

#define RECOVERY_NO_CONFIRM_FILE    "cotrecovery/.no_confirm"
#define RECOVERY_MANY_CONFIRM_FILE  "cotrecovery/.many_confirm"
#define RECOVERY_VERSION_FILE       "cotrecovery/.recovery_version"
#define RECOVERY_LAST_INSTALL_FILE  "cotrecovery/.last_install_path"

// nandroid settings
#define NANDROID_HIDE_PROGRESS_FILE  "cotrecovery/.hidenandroidprogress"
#define NANDROID_BACKUP_FORMAT_FILE  "cotrecovery/.default_backup_format"

// aroma
#define AROMA_FM_PATH           "cotrecovery/aromafm/aromafm.zip"
