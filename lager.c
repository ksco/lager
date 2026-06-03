#include "guest/guest.h"
#include "host/host.h"

#include <unistd.h>

int main(int argc, char **argv)
{
    if (getpid() == 1)
        return guest_init();
    return host_main(argc, argv);
}
