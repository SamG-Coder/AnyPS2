/* Original freestanding EE entry. The loader supplies LOAD segments; no SDK,
 * console kernel initialization, game code or BIOS routines are linked. */
.set noreorder
.text
.global _start
.ent _start
_start:
    lui $sp,0x01f0
    lui $gp,%hi(_gp)
    addiu $gp,$gp,%lo(_gp)
    addu $fp,$zero,$zero
    lui $t0,%hi(__bss_start)
    addiu $t0,$t0,%lo(__bss_start)
    lui $t1,%hi(__bss_end)
    addiu $t1,$t1,%lo(__bss_end)
1:
    beq $t0,$t1,2f
    nop
    sw $zero,0($t0)
    b 1b
    addiu $t0,$t0,4
2:
    jal test_entry
    nop
.global anyps2_test_done
anyps2_test_done:
    b anyps2_test_done
    nop
.end _start
