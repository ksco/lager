![logo](logo.png)
# lager

`lager` is a launcher for running Box64 workloads on LoongArch systems whose
native userland uses 16 KiB pages. It primarily targets LoongArch AOSC OS,
which provides a 4 KiB page-size kernel by default.

Other LoongArch distributions may also work, provided that a 4 KiB page-size
kernel and matching kernel modules are available.

## How It Works

`lager` starts a compact 4 KiB page-size LoongArch guest with QEMU-KVM. The
same `lager` binary runs as the guest init program, mounts the host filesystem
through virtiofsd, prepares the runtime environment, and starts the requested
command.

Graphics are provided through virtio-gpu Venus and Zink, allowing applications
inside the guest to use Vulkan and OpenGL.

## Dependencies

Required:

- A LoongArch64 system with KVM access
- A 4 KiB page-size LoongArch kernel and matching modules
- QEMU and virtiofsd
- passt
- Box64 in `PATH`
- dbus
- PulseAudio
- Xorg
- Openbox
- dunst
- zstd

Optional:

- polkit
- upower

The invoking user must be able to access `/dev/kvm`.

### AOSC OS

Install the common runtime dependencies:

```bash
sudo oma install qemu passt virtiofsd dbus pulseaudio xorg-server openbox dunst zstd
```

Install optional power-management integration:

```bash
sudo oma install polkit upower
```

Add your user to the `kvm` group:

```bash
sudo usermod -aG kvm "$USER"
sudo chown $USER /dev/kvm
```

Log out and back in for the group change to take effect.

## Compile

```bash
cmake -S . -B build
cmake --build build
```

Install:

```bash
sudo cmake --install build
```

## Use

Run a command:

```bash
lager COMMAND [ARGS...]
```

Run a command without the graphical session:

```bash
lager -headless -- COMMAND [ARGS...]
```

Examples:

```bash
lager xeyes
lager -headless -- ls
lager box64 wine game.exe
```

Show configuration:

```bash
lager -config
```
