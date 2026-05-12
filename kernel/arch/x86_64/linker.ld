ENTRY(boot_entry)

PHDRS
{
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    
    __kernel_start = .;
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .early_initcall : {
        __early_initcall_start = .;
        QUAD(0);
        KEEP(*(SORT_BY_NAME(.early_initcall.*)))
        QUAD(0);
        __early_initcall_end = .;
    } :rodata

    .late_initcall : {
        __late_initcall_start = .;
        QUAD(0);
        KEEP(*(SORT_BY_NAME(.late_initcall.*)))
        QUAD(0);
        __late_initcall_end = .;
    } :rodata

    . = ALIGN(8);

    .ap_initcall : {
        __ap_initcall_start = .;
        QUAD(0);
        KEEP(*(SORT_BY_NAME(.ap_initcall.*)))
        QUAD(0)
        __ap_initcall_end = .;
    } :rodata

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata
    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}