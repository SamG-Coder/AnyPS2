#include "protocol.hpp"

extern "C" Word spilled_switch(Word index);
extern "C" Word hoisted_switch(Word index);
// Original assembly fixes the compiler scheduling pattern under test. It is
// assembled into a real PS2 ELF and then compiled through the ordinary AOT path.
asm(R"(
    .set push
    .set noreorder
    .text
    .align 3
    .globl spilled_switch
    .ent spilled_switch
spilled_switch:
    addiu $sp,$sp,-32
    lui $t0,%hi(spilled_targets)
    addiu $t0,$t0,%lo(spilled_targets)
    sw $t0,16($sp)
    move $t1,$sp
    b spilled_dispatch
    sw $zero,20($t1)
spilled_dispatch:
    sltiu $a1,$a0,3
    beq $a1,$zero,spilled_default
    nop
    sll $v0,$a0,2
    lw $v1,16($t1)
    addu $v0,$v1,$v0
    lw $v0,0($v0)
    jr $v0
    nop
spilled_zero:
    addiu $v0,$zero,11
    jr $ra
    addiu $sp,$sp,32
spilled_one:
    addiu $v0,$zero,23
    jr $ra
    addiu $sp,$sp,32
spilled_two:
    addiu $v0,$zero,37
    jr $ra
    addiu $sp,$sp,32
spilled_default:
    addiu $v0,$zero,99
    jr $ra
    addiu $sp,$sp,32
    .end spilled_switch
    .align 3
    .globl hoisted_switch
    .ent hoisted_switch
hoisted_switch:
    addiu $sp,$sp,-32
    lui $v1,%hi(spilled_targets)
    addiu $v1,$v1,%lo(spilled_targets)
    sltiu $a1,$a0,3
    beq $a1,$zero,hoisted_default
    nop
    sll $v0,$a0,2
    addu $v0,$v1,$v0
    lw $v0,0($v0)
    jr $v0
    nop
hoisted_default:
    addiu $v0,$zero,99
    jr $ra
    addiu $sp,$sp,32
    .end hoisted_switch
    .section .rodata
    .align 2
spilled_targets:
    .word spilled_zero,spilled_one,spilled_two
    .text
    .set pop
)");

void test_body() {
    for (Word i=0;i!=4;++i) record(spilled_switch(i));
    for (Word i=0;i!=4;++i) record(hoisted_switch(i));
}
