; =============================================================================
; SAMOS / AtlasOS — kernel/arch/i386/isr_stubs.asm
; -----------------------------------------------------------------------------
; Points d'entrée des 48 premiers vecteurs d'interruption (K0.4) :
;   0..31  exceptions processeur
;   32..47 réservés aux IRQ matérielles remappées (utilisés en K0.5)
;
; Le CPU, en entrant par une porte d'interruption (interrupt gate) de même
; privilège, empile : EFLAGS, CS, EIP, puis un CODE D'ERREUR pour certaines
; exceptions seulement (8, 10-14, 17, 21, 29, 30 ; SDM vol. 3A §6.13).
; Pour obtenir un cadre UNIFORME, chaque stub sans code d'erreur empile un 0
; factice, puis le numéro de vecteur, et saute vers isr_common.
; =============================================================================
bits 32

KERNEL_DATA_SEL equ 0x10

extern isr_dispatch

section .text

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push dword 0            ; code d'erreur factice
    push dword %1           ; numéro de vecteur
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    ; le CPU a déjà empilé le code d'erreur
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8                 ; #DF (en pratique géré par une porte de tâche)
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
%assign i 32
%rep 16
ISR_NOERR i                 ; IRQ 0..15 remappées (K0.5)
%assign i i + 1
%endrep

; -----------------------------------------------------------------------------
; isr_common : sauvegarde complète, appel C conforme à l'ABI, restauration.
; -----------------------------------------------------------------------------
isr_common:
    pushad                  ; EAX ECX EDX EBX ESP(avant pushad) EBP ESI EDI
    push ds
    push es
    push fs
    push gs

    ; Segments de données du noyau (indispensable dès qu'une interruption
    ; pourra survenir en ring 3 avec des segments utilisateur, K1).
    mov ax, KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; L'ABI exige DF = 0 à l'appel d'une fonction C ; une interruption ne
    ; modifie PAS DF : le code interrompu a pu exécuter STD.
    cld

    ; Alignement de pile : la taille du cadre dépend du vecteur et du code
    ; interrompu, l'alignement de ESP est donc inconnu ici. On mémorise le
    ; pointeur de cadre dans EBX (registre préservé par l'appelé en cdecl),
    ; on aligne ESP sur 16, puis on empile l'argument de sorte que ESP soit
    ; multiple de 16 juste avant CALL.
    mov ebx, esp            ; EBX = &interrupt_frame
    and esp, 0xFFFFFFF0
    sub esp, 12
    push ebx                ; argument : struct interrupt_frame *
    call isr_dispatch
    mov esp, ebx            ; annule l'alignement et l'argument

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8              ; retirer vecteur + code d'erreur
    iretd                   ; restaure EIP, CS, EFLAGS (dont IF et DF d'origine)

; Table des adresses des stubs, utilisée par idt.c pour remplir l'IDT.
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dd isr_stub_ %+ i
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
