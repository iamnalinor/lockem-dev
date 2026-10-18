# Build both target ABIs: arm64-v8a (Pixel 9a) and x86_64 (emulator).
APP_ABI      := arm64-v8a x86_64
# API 26: covers Android 16 target and leaves room for older roots.
APP_PLATFORM := android-26
# No C++ runtime — the daemon is pure C with no external deps.
APP_STL      := none
# 16 KB max page size, required by newer images (e.g. the ps16k emulator).
APP_LDFLAGS  += -Wl,-z,max-page-size=16384
