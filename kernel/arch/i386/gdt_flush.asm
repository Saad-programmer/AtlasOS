; =============================================================================
; SAMOS / AtlasOS — kernel/arch/i386/gdt_flush.asm
; Chargement de la GDT et du registre de tâche (K0.4).
; Convention d'appel cdecl : argument à [esp+4] ; EAX, ECX, EDX modifiables.
; =============================================================================
bits 32

KERNEL_CODE_SEL equ 0x08
KERNEL_DATA_SEL equ 0x10

section .text

; void gdt_load(const void *gdtr)
; Après LGDT, les registres de segment conservent leurs anciens descripteurs
; en cache (partie cachée). Ils doivent être RECHARGÉS pour utiliser la
; nouvelle table :
;   - CS ne peut pas être chargé par MOV : un saut lointain (far jmp)
;     charge simultanément CS et EIP ;
;   - DS, ES, FS, GS, SS sont rechargés par MOV.
global gdt_load
gdt_load:
    mov eax, [esp + 4]
    lgdt [eax]
    jmp KERNEL_CODE_SEL:.reload_cs   ; far jump : CS <- 0x08
.reload_cs:
    mov ax, KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax                       ; MOV SS inhibe les interruptions pour
                                     ; l'instruction suivante (SDM vol. 2, MOV)
    ret

; void tss_load(uint16_t selector)
; LTR charge le registre de tâche et marque le descripteur TSS « occupé ».
global tss_load
tss_load:
    mov ax, [esp + 4]
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
