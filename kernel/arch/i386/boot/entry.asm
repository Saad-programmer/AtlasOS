; =============================================================================
; SAMOS / AtlasOS — kernel/arch/i386/boot/entry.asm
; -----------------------------------------------------------------------------
; Point d'entrée du noyau (jalon K0.1).
;
; Rôle :
;   1. Fournir l'en-tête Multiboot 1 qui permet à un chargeur conforme (GRUB,
;      ou le chargeur intégré de QEMU via `-kernel`) de reconnaître et charger
;      ce fichier ELF.
;   2. Recevoir le contrôle du chargeur (symbole `_start`) dans l'état machine
;      défini par la spécification Multiboot 0.6.96, section 3.2.
;   3. Établir un environnement minimal conforme à l'ABI C i386 System V :
;      une pile valide et alignée, EFLAGS.DF = 0, .bss à zéro.
;   4. Appeler `kmain(magic, mbi_phys)` en C.
;   5. Ne jamais revenir : boucle d'arrêt sûre si kmain retournait.
;
; État CPU garanti par Multiboot à l'entrée (spéc. §3.2) :
;   EAX = 0x2BADB002 (magic de preuve du chargeur)
;   EBX = adresse PHYSIQUE de la structure multiboot_info
;   CS  = segment code 32 bits, base 0, limite 0xFFFFFFFF
;   DS/ES/FS/GS/SS = segment données 32 bits, base 0, limite 0xFFFFFFFF
;   CR0.PE = 1 (mode protégé), CR0.PG = 0 (pas de pagination)
;   EFLAGS.IF = 0 (interruptions masquées), EFLAGS.VM = 0
;   A20 activée
; NON garanti :
;   ESP (indéfini  -> nous devons créer notre pile)
;   GDTR (peut être invalide -> ne pas recharger de registre segment avant K0.4)
;   IDTR (indéfini -> aucune interruption ne doit survenir avant K0.4)
;   Les autres bits d'EFLAGS, notamment DF (-> remise à zéro explicite)
; =============================================================================

bits 32

; ---------------------------------------------------------------------------
; Constantes de l'en-tête Multiboot 1 (spéc. §3.1.1)
; ---------------------------------------------------------------------------
MB_HEADER_MAGIC     equ 0x1BADB002
MB_FLAG_PAGE_ALIGN  equ 1 << 0      ; modules chargés alignés sur 4 KiB
MB_FLAG_MEMORY_INFO equ 1 << 1      ; demander mem_lower/mem_upper et la carte mémoire
MB_HEADER_FLAGS     equ MB_FLAG_PAGE_ALIGN | MB_FLAG_MEMORY_INFO
; La somme magic + flags + checksum doit valoir 0 modulo 2^32.
MB_HEADER_CHECKSUM  equ -(MB_HEADER_MAGIC + MB_HEADER_FLAGS)

; Taille de la pile de démarrage du noyau. 16 KiB est largement suffisant pour
; K0 (aucune récursion profonde, aucun gros tableau local). Doit rester un
; multiple de 16 pour conserver l'alignement de stack_top.
KERNEL_STACK_SIZE   equ 16384

; ---------------------------------------------------------------------------
; En-tête Multiboot.
; Section dédiée, placée en TOUT PREMIER par le script de liaison afin d'être
; dans les 8192 premiers octets du fichier et alignée sur 4 octets (§3.1).
; ---------------------------------------------------------------------------
section .multiboot progbits alloc noexec nowrite align=4
global multiboot_header
multiboot_header:
    dd MB_HEADER_MAGIC
    dd MB_HEADER_FLAGS
    dd MB_HEADER_CHECKSUM

; ---------------------------------------------------------------------------
; Pile de démarrage, dans .bss (n'occupe pas de place dans le fichier ELF).
; La pile x86 croît vers les adresses basses : ESP démarre à stack_top.
; ---------------------------------------------------------------------------
section .bss nobits alloc noexec write align=16
global boot_stack_bottom
global boot_stack_top
boot_stack_bottom:
    resb KERNEL_STACK_SIZE
boot_stack_top:

; ---------------------------------------------------------------------------
; Code d'entrée
; ---------------------------------------------------------------------------
section .text
global _start
extern kmain
extern __bss_start                  ; définis par linker.ld
extern __bss_end

_start:
    ; Les interruptions sont déjà masquées (IF=0) selon Multiboot, mais aucune
    ; IDT n'existe encore : on le réaffirme de façon défensive.
    cli

    ; 1) Pile : ESP est indéfini à l'entrée. Toute instruction push/call
    ;    avant cette ligne écrirait à une adresse arbitraire.
    mov esp, boot_stack_top

    ; 2) EFLAGS : seul IF et VM sont garantis. L'ABI exige DF = 0 à chaque
    ;    appel de fonction (les instructions de chaîne avancent alors vers
    ;    les adresses croissantes). On charge 0 dans EFLAGS entier :
    ;    DF=0, IF=0, AC=0, NT=0, TF=0...  (le bit 1 réservé est forcé à 1
    ;    par le matériel).
    push dword 0
    popfd

    ; 3) Mise à zéro de .bss.
    ;    Un chargeur ELF doit déjà remplir de zéros la partie p_memsz-p_filesz
    ;    des segments ; nous ne dépendons pas de ce comportement et
    ;    l'imposons nous-mêmes (C exige que les objets statiques non
    ;    initialisés valent 0). rep stosd utilise EAX, ECX, EDI : on sauve
    ;    d'abord le magic Multiboot (EAX) dans ESI. EBX n'est pas touché.
    ;    La pile se trouve elle-même dans .bss : c'est sans danger car elle
    ;    est vide à cet instant (le push/popfd ci-dessus est terminé).
    mov esi, eax                    ; ESI <- magic Multiboot
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi                    ; taille en octets (multiple de 4, garanti par linker.ld)
    shr ecx, 2                      ; nombre de mots de 32 bits
    xor eax, eax
    rep stosd                       ; DF=0 : remplissage croissant

    ; 4) Préparer l'appel cdecl : kmain(uint32_t magic, uint32_t mbi_phys)
    ;    Les arguments sont empilés de droite à gauche.
    ;    Règle d'alignement (ABI i386 System V, appliquée par -mstack-alignment=16) :
    ;    ESP doit être multiple de 16 JUSTE AVANT l'instruction call, de sorte
    ;    qu'à l'entrée de la fonction (ESP + 4) soit multiple de 16.
    ;    ESP = boot_stack_top (aligné 16)
    ;    sub 8   -> ESP ≡ 8  (mod 16)
    ;    push x2 -> ESP ≡ 0  (mod 16)   => alignement correct au call.
    mov esp, boot_stack_top
    sub esp, 8
    push ebx                        ; 2e argument : adresse physique multiboot_info
    push esi                        ; 1er argument : magic

    ; Vérification d'invariant à l'exécution : si une modification future
    ; casse l'alignement, on s'arrête immédiatement plutôt que d'exécuter du
    ; C dans un état non conforme à l'ABI.
    test esp, 0xF
    jnz .halt

    call kmain

    ; kmain est déclarée _Noreturn. Si elle revenait malgré tout, on tombe ici.
.halt:
    cli                             ; masquer les interruptions masquables
.halt_loop:
    hlt                             ; arrêter le CPU jusqu'à la prochaine interruption
    jmp .halt_loop                  ; un NMI ou SMI peut réveiller le CPU : on reboucle

; Indique à l'éditeur de liens que ce fichier n'a pas besoin de pile exécutable.
section .note.GNU-stack noalloc noexec nowrite progbits
