#include "protocol.hpp"

extern "C" Word spilled_switch(Word index);
extern "C" Word hoisted_switch(Word index);
extern "C" Word reordered_switch(Word index);
extern "C" Word hoisted_guard_switch(Word index);
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
    .align 3
    .globl reordered_switch
    .ent reordered_switch
reordered_switch:
    addiu $sp,$sp,-32
    lui $t0,%hi(reordered_targets)
    addiu $t0,$t0,%lo(reordered_targets)
    sw $t0,16($sp)
    move $t1,$sp
    b reordered_dispatch
    sw $zero,20($t1)
reordered_dispatch:
    sltiu $a1,$a0,3
    beq $a1,$zero,reordered_default
    sll $t2,$a2,1
    lw $v1,16($t1)
    sll $v0,$a0,2
    addu $v0,$v1,$v0
    lw $v0,0($v0)
    jr $v0
    nop
reordered_zero:
    addiu $v0,$zero,41
    jr $ra
    addiu $sp,$sp,32
reordered_one:
    addiu $v0,$zero,53
    jr $ra
    addiu $sp,$sp,32
reordered_two:
    addiu $v0,$zero,67
    jr $ra
    addiu $sp,$sp,32
reordered_default:
    addiu $v0,$zero,101
    jr $ra
    addiu $sp,$sp,32
    .end reordered_switch
    .align 3
    .globl hoisted_guard_switch
    .ent hoisted_guard_switch
hoisted_guard_switch:
    addiu $sp,$sp,-32
    lui $t0,%hi(hoisted_guard_targets)
    addiu $t0,$t0,%lo(hoisted_guard_targets)
    sw $t0,16($sp)
    move $t1,$sp
    b hoisted_guard_dispatch
    sw $zero,20($t1)
hoisted_guard_dispatch:
    sltiu $a1,$a0,3
    addiu $t3,$zero,2
hoisted_guard_loop:
    addiu $t3,$t3,-1
    bne $t3,$zero,hoisted_guard_loop
    nop
    beq $a2,$zero,hoisted_guard_other
    nop
    move $a7,$a3
    b hoisted_guard_join
    nop
hoisted_guard_other:
    move $a7,$a2
hoisted_guard_join:
    beq $a1,$zero,hoisted_guard_default
    sll $t2,$a2,1
    lw $v1,16($t1)
    sll $v0,$a0,2
    addu $v0,$v1,$v0
    lw $v0,0($v0)
    jr $v0
    nop
hoisted_guard_zero:
    addiu $v0,$zero,71
    jr $ra
    addiu $sp,$sp,32
hoisted_guard_one:
    addiu $v0,$zero,83
    jr $ra
    addiu $sp,$sp,32
hoisted_guard_two:
    addiu $v0,$zero,97
    jr $ra
    addiu $sp,$sp,32
hoisted_guard_default:
    addiu $v0,$zero,103
    jr $ra
    addiu $sp,$sp,32
    .end hoisted_guard_switch
    .section .rodata
    .align 2
spilled_targets:
    .word spilled_zero,spilled_one,spilled_two
reordered_targets:
    .word reordered_zero,reordered_one,reordered_two
hoisted_guard_targets:
    .word hoisted_guard_zero,hoisted_guard_one,hoisted_guard_two
    .text
    .set pop
)");

void test_body() {
    for (Word i=0;i!=4;++i) record(spilled_switch(i));
    for (Word i=0;i!=4;++i) record(hoisted_switch(i));
    for (Word i=0;i!=4;++i) record(reordered_switch(i));
    for (Word i=0;i!=4;++i) record(hoisted_guard_switch(i));
}
