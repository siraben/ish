#include "gadgets.h"

.rv_gadget branch_taken
    ldr x16, [_ip], 8
    br x16

.rv_gadget branch_not_taken
    rv_gret

.rv_gadget jal
    ldr x16, [_ip], 8
    br x16

.rv_gadget jalr
    ldr x16, [_ip], 8
    br x16

# vim: ft=gas
