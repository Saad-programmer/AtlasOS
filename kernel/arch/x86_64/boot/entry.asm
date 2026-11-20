; =============================================================================
; SAMOS / AtlasOS — kernel/arch/x86_64/boot/entry.asm        (K1.1-M64-1)
; -----------------------------------------------------------------------------
; SQUELETTE DE CONSTRUCTION, NON AMORÇABLE.
;
; Rôle : fournir le symbole d'entrée `_start` exigé par l'éditeur de liens
; pour produire un exécutable ELF64 complet, que tools/check_elf64.sh peut
; inspecter statiquement (classe, machine, sections, segments, symboles).
;
; Ce fichier ne contient VOLONTAIREMENT :
;   - aucun en-tête Multiboot (1 ou 2) : le format d'image et le mécanisme de
;     chargement relèvent de la décision A-1, prise après les expériences V2/V3
;     (docs/review/K1.1-M64-1-design.md §4.3, §12.1). check_elf64.sh REFUSE
;     toute image qui contiendrait un magic Multiboot ;
;   - aucun code de transition (CPUID, PAE, EFER, CR0/CR3/CR4, GDT, IDT) :
;     c'est M64-2, non autorisé. check_elf64.sh refuse ces instructions.
;
; Aucun chargeur ne sait démarrer cette image : `_start` n'est jamais exécuté.
; Son contenu (cli ; hlt en boucle) est seulement un code 64 bits valide et
; inerte, assemblé en `bits 64`, pour que la section .text existe et que le
; point d'entrée désigne une instruction réelle.
; =============================================================================
bits 64

section .text
global _start
_start:
    cli                     ; jamais exécuté (voir l'en-tête)
.halt:
    hlt
    jmp .halt

; Pile non exécutable (cohérent avec -z noexecstack de l'éditeur de liens).
section .note.GNU-stack noalloc noexec nowrite progbits
