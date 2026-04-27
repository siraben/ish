#ifndef ASBESTOS_PROFILE_H
#define ASBESTOS_PROFILE_H

#include <stdint.h>
#include "misc.h"

void asbestos_profile_record_opcode(unsigned key, uint64_t code_words, uint64_t guest_bytes);
void asbestos_profile_record_block(uint64_t code_words, uint64_t guest_bytes);
void asbestos_profile_record_interrupt(int interrupt);
void asbestos_profile_record_syscall(unsigned syscall);
bool asbestos_profile_is_enabled(void);

#endif
