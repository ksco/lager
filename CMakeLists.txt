cmake_minimum_required(VERSION 3.16)

project(lager C)

add_executable(lager
    lager.c
    misc/log.c
    host/host.c
    host/launcher.c
    host/initramfs.c
    host/modules.c
    guest/config.c
    guest/guest.c
    guest/exec.c
    guest/services.c
    misc/config.c
    features/audio.c
    features/box64.c
    features/common.c
    features/dbus.c
    features/gpu.c
    features/gpu_compat.c
    features/network.c
    features/notifications.c
    features/openbox.c
    features/registry.c
    features/wayland.c
    features/x11.c
    misc/utils.c
)
target_compile_features(lager PRIVATE c_std_11)
target_compile_options(lager PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(lager PRIVATE -static)

install(TARGETS lager RUNTIME DESTINATION bin)
