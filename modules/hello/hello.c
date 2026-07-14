extern int printf(const char *fmt, ...);
extern void *kmalloc(unsigned long size);
extern void kfree(void *ptr);

static const char modinfo[] __attribute__((used, section(".modinfo"))) = "name=hello";

__attribute__((section(".init.text")))
int hello_init(void) {
    printf("[hello] Module loaded!\n");

    void *p = kmalloc(256);
    if (p) {
        printf("[hello] kmalloc(256) = %p\n", p);
        unsigned char *cp = (unsigned char *)p;
        for (int i = 0; i < 256; i++) cp[i] = (unsigned char)i;
        printf("[hello] data check: [0]=%d [127]=%d [255]=%d\n", cp[0], cp[127], cp[255]);
        kfree(p);
        printf("[hello] kfree done\n");
    } else {
        printf("[hello] kmalloc failed!\n");
    }

    return 0;
}

__attribute__((section(".exit.text")))
void hello_exit(void) {
    printf("[hello] Module unloaded.\n");
}
