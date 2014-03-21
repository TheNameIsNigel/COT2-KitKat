##################################################################
# Device Specific Config                                         #
# These can go under device BoardConfig.mk or they can stay here #
# By PhilZ for PhilZ Touch recovery                              #
# Modified for Cannibal Open Touch				 #
##################################################################

#LG Optimus G ATT (e970) - Canada (e973) - Sprint (ls970) - Intl (e975)
ifneq ($(filter $(TARGET_PRODUCT),cm_e970 cm_e973 cm_ls970 cm_e975),)
    TARGET_COMMON_NAME := LG_Optimus_G-$(TARGET_PRODUCT)
    DEVICE_RESOLUTION := 1280x768
    BRIGHTNESS_SYS_FILE := "/sys/class/leds/lcd-backlight/brightness"

#LGE Nexus 4 - mako
else ifeq ($(TARGET_PRODUCT), cm_mako)
    TARGET_COMMON_NAME := Nexus_4
    DEVICE_RESOLUTION := 1280x768
    BRIGHTNESS_SYS_FILE := "/sys/class/leds/lcd-backlight/brightness"

#ASUS Nexus 7 (Wifi) - tilapia (grouper)
else ifneq ($(filter $(TARGET_PRODUCT),cm_tilapia cm_grouper),)
    TARGET_COMMON_NAME := ASUS_Nexus_7-$(TARGET_PRODUCT)
    DEVICE_RESOLUTION := 1280x720
    BRIGHTNESS_SYS_FILE := "/sys/class/backlight/pwm-backlight/brightness"

endif
# end device specific settings

# The below flags must always be defined as default in BoardConfig.mk, unless defined above:
# device name to display in About dialog
ifndef TARGET_COMMON_NAME
    TARGET_COMMON_NAME := $(TARGET_PRODUCT)
endif

# battery level default path
ifndef BATTERY_LEVEL_PATH
    BATTERY_LEVEL_PATH := "/sys/class/power_supply/battery/capacity"
endif
