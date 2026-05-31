#define _GNU_SOURCE
#include "guest_net.h"

#include "log.h"
#include "utils.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct dhcp_packet {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

struct dhcp_lease {
    uint32_t address;
    uint32_t netmask;
    uint32_t router;
    uint32_t dns;
    uint32_t server;
};

static void dhcp_option(unsigned char **cursor, uint8_t type, const void *data,
                        uint8_t len)
{
    *(*cursor)++ = type;
    *(*cursor)++ = len;
    memcpy(*cursor, data, len);
    *cursor += len;
}

static bool dhcp_parse(const struct dhcp_packet *packet, ssize_t size,
                       uint32_t xid, uint8_t wanted_type,
                       struct dhcp_lease *lease)
{
    const unsigned char *cursor = packet->options;
    const unsigned char *end = (const unsigned char *)packet + size;
    uint8_t message_type = 0;

    if (size < (ssize_t)offsetof(struct dhcp_packet, options) ||
        packet->op != 2 || packet->xid != xid ||
        packet->magic != htonl(0x63825363))
        return false;
    lease->address = packet->yiaddr;
    while (cursor < end) {
        uint8_t type = *cursor++;
        uint8_t len;

        if (type == 0)
            continue;
        if (type == 255)
            break;
        if (cursor >= end)
            return false;
        len = *cursor++;
        if ((size_t)(end - cursor) < len)
            return false;
        if (type == 53 && len == 1)
            message_type = cursor[0];
        else if (type == 1 && len >= 4)
            memcpy(&lease->netmask, cursor, 4);
        else if (type == 3 && len >= 4)
            memcpy(&lease->router, cursor, 4);
        else if (type == 6 && len >= 4)
            memcpy(&lease->dns, cursor, 4);
        else if (type == 54 && len >= 4)
            memcpy(&lease->server, cursor, 4);
        cursor += len;
    }
    return message_type == wanted_type;
}

static ssize_t dhcp_send(int fd, const struct sockaddr_in *destination,
                         const unsigned char *mac, uint32_t xid,
                         uint8_t message_type, const struct dhcp_lease *lease)
{
    struct dhcp_packet packet = {0};
    unsigned char *cursor = packet.options;
    const uint8_t requested_options[] = {1, 3, 6};

    packet.op = 1;
    packet.htype = 1;
    packet.hlen = 6;
    packet.xid = xid;
    packet.flags = htons(0x8000);
    memcpy(packet.chaddr, mac, 6);
    packet.magic = htonl(0x63825363);
    dhcp_option(&cursor, 53, &message_type, 1);
    if (message_type == 3) {
        dhcp_option(&cursor, 50, &lease->address, 4);
        if (lease->server)
            dhcp_option(&cursor, 54, &lease->server, 4);
    }
    dhcp_option(&cursor, 55, requested_options, sizeof(requested_options));
    *cursor++ = 255;
    return sendto(fd, &packet,
                  offsetof(struct dhcp_packet, options) +
                      (size_t)(cursor - packet.options),
                  0, (const struct sockaddr *)destination,
                  sizeof(*destination));
}

static bool dhcp_receive(int fd, uint32_t xid, uint8_t wanted_type,
                         struct dhcp_lease *lease)
{
    struct dhcp_packet packet;
    int attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        ssize_t size = recv(fd, &packet, sizeof(packet), 0);

        if (size < 0) {
            if (errno == EINTR) {
                attempt--;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            return false;
        }
        if (dhcp_parse(&packet, size, xid, wanted_type, lease))
            return true;
    }
    return false;
}

static int interface_ioctl(int fd, unsigned long request, const char *name,
                           struct sockaddr_in *address)
{
    struct ifreq ifr = {0};

    strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
    if (address)
        memcpy(&ifr.ifr_addr, address, sizeof(*address));
    return ioctl(fd, request, &ifr);
}

static void write_guest_resolv_conf(uint32_t dns)
{
    struct in_addr address = {.s_addr = dns};
    char text[INET_ADDRSTRLEN];
    FILE *file;

    if (!dns || !inet_ntop(AF_INET, &address, text, sizeof(text)))
        return;
    mkdir_ok("/run/lager", 0755);
    file = fopen("/run/lager/resolv.conf", "w");
    if (!file) {
        warnx("cannot create guest resolv.conf: %s", strerror(errno));
        return;
    }
    fprintf(file, "nameserver %s\n", text);
    fclose(file);
    if (mount("/run/lager/resolv.conf", "/etc/resolv.conf", NULL, MS_BIND,
              NULL) < 0)
        warnx("cannot install guest resolv.conf: %s", strerror(errno));
}

static void configure_guest_lease(int fd, const char *name,
                                  const struct dhcp_lease *lease)
{
    struct sockaddr_in address = {.sin_family = AF_INET};
    struct rtentry route = {0};
    struct sockaddr_in *route_address;

    address.sin_addr.s_addr = lease->address;
    if (interface_ioctl(fd, SIOCSIFADDR, name, &address) < 0)
        warnx("cannot set %s address: %s", name, strerror(errno));
    address.sin_addr.s_addr =
        lease->netmask ? lease->netmask : htonl(0xffffff00);
    if (interface_ioctl(fd, SIOCSIFNETMASK, name, &address) < 0)
        warnx("cannot set %s netmask: %s", name, strerror(errno));
    if (lease->router) {
        route_address = (struct sockaddr_in *)&route.rt_dst;
        route_address->sin_family = AF_INET;
        route_address = (struct sockaddr_in *)&route.rt_genmask;
        route_address->sin_family = AF_INET;
        route_address = (struct sockaddr_in *)&route.rt_gateway;
        route_address->sin_family = AF_INET;
        route_address->sin_addr.s_addr = lease->router;
        route.rt_flags = RTF_UP | RTF_GATEWAY;
        route.rt_dev = (char *)name;
        if (ioctl(fd, SIOCADDRT, &route) < 0 && errno != EEXIST)
            warnx("cannot set default route: %s", strerror(errno));
    }
    write_guest_resolv_conf(lease->dns);
}

static void setup_guest_loopback(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET};
    struct ifreq ifr = {0};
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        warnx("cannot create loopback socket: %s", strerror(errno));
        return;
    }
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (interface_ioctl(fd, SIOCSIFADDR, "lo", &address) < 0)
        warnx("cannot set loopback address: %s", strerror(errno));
    address.sin_addr.s_addr = htonl(0xff000000);
    if (interface_ioctl(fd, SIOCSIFNETMASK, "lo", &address) < 0)
        warnx("cannot set loopback netmask: %s", strerror(errno));
    strncpy(ifr.ifr_name, "lo", sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
        warnx("cannot read loopback flags: %s", strerror(errno));
    else {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            warnx("cannot bring loopback up: %s", strerror(errno));
    }
    close(fd);
}

static char *guest_net_ifname(void)
{
    DIR *stream = opendir("/sys/class/net");
    struct dirent *entry;
    char *name = NULL;

    if (!stream)
        return NULL;
    while ((entry = readdir(stream))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, "lo"))
            continue;
        name = xstrdup(entry->d_name);
        break;
    }
    closedir(stream);
    return name;
}

static uint32_t dhcp_xid(void)
{
    struct timespec now = {0};
    uint32_t xid;

    clock_gettime(CLOCK_MONOTONIC, &now);
    xid = (uint32_t)now.tv_sec ^ (uint32_t)now.tv_nsec ^ (uint32_t)getpid();
    return htonl(xid ? xid : 1);
}

void setup_guest_net(void)
{
    struct sockaddr_in source = {
        .sin_family = AF_INET,
        .sin_port = htons(68),
        .sin_addr.s_addr = INADDR_ANY,
    };
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(67),
        .sin_addr.s_addr = INADDR_BROADCAST,
    };
    struct timeval timeout = {.tv_sec = 1};
    struct ifreq ifr = {0};
    struct dhcp_lease lease = {0};
    uint32_t xid = dhcp_xid();
    int broadcast = 1;
    char *name;
    int fd;
    int attempt;

    setup_guest_loopback();
    name = guest_net_ifname();
    if (!name) {
        warnx("cannot find a network interface");
        return;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0) {
        warnx("cannot create DHCP socket: %s", strerror(errno));
        free(name);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name) + 1);
    strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        warnx("cannot find %s: %s", name, strerror(errno));
        close(fd);
        free(name);
        return;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0 ||
        ioctl(fd, SIOCGIFHWADDR, &ifr) < 0 ||
        bind(fd, (struct sockaddr *)&source, sizeof(source)) < 0) {
        warnx("cannot prepare %s for DHCP: %s", name, strerror(errno));
        close(fd);
        free(name);
        return;
    }
    for (attempt = 0; attempt < 4; attempt++) {
        if (dhcp_send(fd, &destination, (unsigned char *)ifr.ifr_hwaddr.sa_data,
                      xid, 1, &lease) < 0)
            continue;
        if (!dhcp_receive(fd, xid, 2, &lease))
            continue;
        if (dhcp_send(fd, &destination, (unsigned char *)ifr.ifr_hwaddr.sa_data,
                      xid, 3, &lease) < 0)
            continue;
        if (dhcp_receive(fd, xid, 5, &lease)) {
            configure_guest_lease(fd, name, &lease);
            close(fd);
            free(name);
            return;
        }
    }
    warnx("DHCP did not configure %s", name);
    close(fd);
    free(name);
}
