LOCAL_PATH := $(call my-dir)

# Build shared library
include $(CLEAR_VARS)

LOCAL_MODULE := libtar
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := append.c block.c decode.c encode.c extract.c handle.c output.c util.c wrapper.c basename.c strmode.c libtar_hash.c libtar_list.c dirname.c android_utils.c
LOCAL_C_INCLUDES += $(LOCAL_PATH) \
                    external/zlib
LOCAL_SHARED_LIBRARIES += libz libc

LOCAL_C_INCLUDES += external/libselinux/include
LOCAL_SHARED_LIBRARIES += libselinux

ifeq ($(TW_INCLUDE_CRYPTO_FBE), true)
    LOCAL_STATIC_LIBRARIES += libvold libscrypt_static libasync_safe
    LOCAL_SHARED_LIBRARIES +=  \
        android.hardware.boot@1.0 \
        android.hardware.boot-V1-ndk \
        android.hardware.confirmationui@1.0 \
        lib_android_keymaster_keymint_utils \
        android.hardware.gatekeeper-V1-ndk \
        android.hardware.gatekeeper@1.0 \
        android.hardware.keymaster@3.0 \
        android.hardware.keymaster@4.0 \
        android.hardware.keymaster@4.1 \
        android.hardware.weaver@1.0 \
        android.security.apc-ndk \
        android.system.keystore2-V4-ndk \
        android.security.authorization-ndk \
        android.security.maintenance-ndk \
        libselinux \
        libbinder_ndk \
        libboot_control_client \
        libext4_utils \
        libbase \
        libcrypto \
        libcutils \
        libf2fs_sparseblock \
        libkeymaster_messages \
        libkeymint_support \
        libkeystoreinfo \
        libkeystore-attestation-application-id \
        libhardware \
        libprotobuf-cpp-lite \
        libfscrypt \
        libhidlbase \
        libutils \
        libbinder \
        libfs_mgr \
        libkeymaster4support \
        libkeymaster4_1support \
        libf2fs_sparseblock \
        libkeyutils \
        liblog \
        libhwbinder \
        libchrome \
        libbootloader_message \
        libgatekeeper_aidl \
        libsysutils

    LOCAL_CFLAGS += -DUSE_FSCRYPT
    ifeq ($(TW_USE_FSCRYPT_POLICY), 1)
        LOCAL_CFLAGS += -DUSE_FSCRYPT_POLICY_V1
    else
        LOCAL_CFLAGS += -DUSE_FSCRYPT_POLICY_V2
    endif
    LOCAL_C_INCLUDES += system/vold
endif

ifeq ($(TW_LIBTAR_DEBUG),true)
    LOCAL_CFLAGS += -DTW_LIBTAR_DEBUG
endif

include $(BUILD_SHARED_LIBRARY)

# Build static library
include $(CLEAR_VARS)

LOCAL_MODULE := libtar_static
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := append.c block.c decode.c encode.c extract.c handle.c output.c util.c wrapper.c basename.c strmode.c libtar_hash.c libtar_list.c dirname.c android_utils.c
LOCAL_C_INCLUDES += $(LOCAL_PATH) \
                    external/zlib
LOCAL_STATIC_LIBRARIES += libz libc

LOCAL_C_INCLUDES += external/libselinux/include
LOCAL_STATIC_LIBRARIES += libselinux

ifeq ($(TW_INCLUDE_CRYPTO_FBE), true)
    LOCAL_CFLAGS += -DUSE_FSCRYPT
    LOCAL_C_INCLUDES += system/vold
endif

ifeq ($(TW_LIBTAR_DEBUG),true)
    LOCAL_CFLAGS += -DTW_LIBTAR_DEBUG
endif

include $(BUILD_STATIC_LIBRARY)
