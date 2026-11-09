/* The Linux release command line has no unwinder (src/cli/no_unwind.cpp), so nothing reads the
   unwind tables and exception tables that libstdc++'s, libgcc's and libc.a's objects bring:
   dropped. */
SECTIONS
{
  /DISCARD/ : { *(.eh_frame) *(.eh_frame.*) *(.gcc_except_table) *(.gcc_except_table.*) }
}
INSERT AFTER .rodata;
