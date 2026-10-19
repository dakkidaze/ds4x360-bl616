SDK_DEMO_PATH ?= $(abspath .)
BL_SDK_BASE ?= $(abspath third_party/bouffalo_sdk)

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk

include $(BL_SDK_BASE)/project.build
