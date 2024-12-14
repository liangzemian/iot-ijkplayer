LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := ijkffmpeg
LOCAL_SRC_FILES := $(MY_APP_FFMPEG_OUTPUT_PATH)/libijkffmpeg.so
include $(PREBUILT_SHARED_LIBRARY)

# include $(CLEAR_VARS)
# LOCAL_MODULE := LebConnection_so
# LOCAL_SRC_FILES := $(MY_APP_FFMPEG_OUTPUT_PATH)/libLebConnection.so
# include $(PREBUILT_SHARED_LIBRARY)