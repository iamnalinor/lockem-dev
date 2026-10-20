LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := lockemd
LOCAL_SRC_FILES := ../lockemd.c ../input.c ../pattern.c ../actions.c ../config.c ../util.c ../pkgname.c ../autolock.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/..
LOCAL_CFLAGS    := -Wall -Wextra -O2 -std=c11
LOCAL_LDLIBS    := -llog
# 16 KB page alignment (required by newer Android images, e.g. the ps16k emulator)
LOCAL_LDFLAGS   := -Wl,-z,max-page-size=16384
include $(BUILD_EXECUTABLE)
