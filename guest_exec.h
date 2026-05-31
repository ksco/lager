#ifndef LAGER_GUEST_EXEC_H
#define LAGER_GUEST_EXEC_H

#include "guest_config.h"

void guest_exec_signal(int signal_number);
int run_guest_command(const struct guest_config *cfg);

#endif
