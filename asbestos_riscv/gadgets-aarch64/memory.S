#include "gadgets.h"

.rv_gadget load_u8
    ldrb _tmp, [_addr]
    rv_gret

.rv_gadget load_u16
    ldrh _tmp, [_addr]
    rv_gret

.rv_gadget load_u32
    ldr w0, [_addr]
    rv_gret

.rv_gadget load_u64
    ldr _tmp, [_addr]
    rv_gret

.rv_gadget store8
    strb w8, [_addr]
    rv_gret

.rv_gadget store16
    strh w8, [_addr]
    rv_gret

.rv_gadget store32
    str w8, [_addr]
    rv_gret

.rv_gadget store64
    str x8, [_addr]
    rv_gret

# vim: ft=gas
