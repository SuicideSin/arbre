/*
 * arbre
 *
 * (c) 2011-2012, Alexis Sellier
 *
 * vm.h
 *
 */
typedef struct {
    unsigned long    pc;
    Path            *path;
    Path           **paths;
    unsigned         pathc; /* TODO: Rename to `npaths` */
    ModuleList      *modules[];
} VM;

struct version {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

VM   *vm       (void);
void  vm_load  (VM *vm, const char *module, Path *paths[]);
void  vm_open  (VM *vm, const char *module, uint8_t *code);
void  vm_run   (VM *vm,  const char *module, const char *path);
