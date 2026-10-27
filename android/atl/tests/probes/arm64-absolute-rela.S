/* ARM64 RELA must ignore the nonzero bytes originally stored in each slot. */
.text
.global fixture_target
.type fixture_target, %function
fixture_target:
    mov w0, #42
    ret
.size fixture_target, .-fixture_target

.data
.balign 8
.global fixture_slot
.type fixture_slot, %object
fixture_slot:
    .xword 0x26e950
    .reloc fixture_slot, R_AARCH64_ABS64, fixture_target
.size fixture_slot, 8

.global fixture_data
.type fixture_data, %object
fixture_data:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
.size fixture_data, 16

.balign 8
.global fixture_data_slot
.type fixture_data_slot, %object
fixture_data_slot:
    .xword 0x1234
    .reloc fixture_data_slot, R_AARCH64_ABS64, fixture_data+7
.size fixture_data_slot, 8

/* This unresolved Android-only name forces the translation loader instead
 * of glibc dlopen. The probe exports its bionic_ hook. */
.global fixture_marker_slot
fixture_marker_slot:
    .xword fixture_android_marker
.section .note.GNU-stack,"",%progbits
