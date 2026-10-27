/* xbox_runtime.c -- MSVC i386 helpers used by the Xbox SDK libraries.
 *
 * libxnet's DHCP code calls __alldiv: two signed 64-bit stack arguments,
 * EDX:EAX result, callee pops 16 bytes. RXDK links compiler-rt's ___divdi3,
 * which computes the same result using the i386 C calling convention.
 *
 * Keep the original frame pointer, align the outgoing C call to 16 bytes,
 * and delegate arithmetic to the compiler runtime already in the link.
 * This must be assembly: a normal C function would not have the SDK's
 * exact symbol name and stack cleanup convention.
 */
#if !defined(TARGET_XBOX) || !defined(__i386__)
#error xbox_runtime.c requires the Xbox i386 target
#endif

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl __alldiv\n"
    ".def __alldiv; .scl 2; .type 32; .endef\n"
    "__alldiv:\n"
    "pushl %ebp\n"
    "movl %esp, %ebp\n"
    "andl $-16, %esp\n"
    "pushl 20(%ebp)\n"  /* divisor, high word */
    "pushl 16(%ebp)\n"  /* divisor, low word */
    "pushl 12(%ebp)\n"  /* dividend, high word */
    "pushl 8(%ebp)\n"   /* dividend, low word */
    "calll ___divdi3\n"
    "movl %ebp, %esp\n"
    "popl %ebp\n"
    "retl $16\n"
);
