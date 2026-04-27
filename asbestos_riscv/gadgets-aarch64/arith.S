#include "gadgets.h"

.rv_gadget add
    add _tmp, x8, x9
    rv_gret

.rv_gadget sub
    sub _tmp, x8, x9
    rv_gret

.rv_gadget and
    and _tmp, x8, x9
    rv_gret

.rv_gadget or
    orr _tmp, x8, x9
    rv_gret

.rv_gadget xor
    eor _tmp, x8, x9
    rv_gret

.rv_gadget sll
    lsl _tmp, x8, x9
    rv_gret

.rv_gadget srl
    lsr _tmp, x8, x9
    rv_gret

.rv_gadget sra
    asr _tmp, x8, x9
    rv_gret

.rv_gadget slt
    cmp x8, x9
    cset _tmp, lt
    rv_gret

.rv_gadget sltu
    cmp x8, x9
    cset _tmp, lo
    rv_gret

# vim: ft=gas
