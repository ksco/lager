cmake_minimum_required(VERSION 3.16)

project(lager C)

add_executable(lager
    lager.c
    log.c
    host.c
    launcher.c
    guest_config.c
    initramfs.c
    guest.c
    guest_exec.c
    guest_net.c
    guest_services.c
    config.c
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
    features/x11.c
    modules.c
    utils.c
)
target_compile_features(lager PRIVATE c_std_11)
target_compile_options(lager PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(lager PRIVATE -static)

install(TARGETS lager RUNTIME DESTINATION bin)
