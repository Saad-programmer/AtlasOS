; =============================================================================
; SAMOS / AtlasOS — kernel/arch/i386/probe.asm
; -----------------------------------------------------------------------------
; Accès mémoire « sondés » (K0.6) : lecture/écriture d'un mot de 32 bits qui
; peut provoquer une faute de page SANS paniquer le noyau.
;
; Mécanisme (table de correction d'exception, cf. __ex_table de Linux) :
; l'adresse exacte de l'instruction susceptible de fauter est exportée
; (mem_probe_*_insn). Si une #PF survient avec EIP == cette adresse, le
; gestionnaire de fautes de page remplace EIP par mem_probe_fixup, qui renvoie
; -1. L'instruction fautive n'a pas modifié ESP : le RET de la correction
; revient donc normalement à l'appelant C.
;
;   int mem_probe_read(const volatile void *addr, uint32_t *out);
;   int mem_probe_write(volatile void *addr, uint32_t value);
;   retour : 0 si l'accès a réussi, -1 s'il a provoqué une faute de page.
; =============================================================================
bits 32
section .text

global mem_probe_read
global mem_probe_read_insn
global mem_probe_write
global mem_probe_write_insn
global mem_probe_fixup

mem_probe_read:
    mov ecx, [esp + 4]          ; addr
    xor eax, eax                ; valeur de retour par défaut : 0 (succès)
mem_probe_read_insn:
    mov edx, [ecx]              ; <-- peut fauter
    mov ecx, [esp + 8]          ; out (pointeur noyau valide : non sondé)
    mov [ecx], edx
    ret

mem_probe_write:
    mov ecx, [esp + 4]          ; addr
    mov edx, [esp + 8]          ; value
    xor eax, eax
mem_probe_write_insn:
    mov [ecx], edx              ; <-- peut fauter
    ret

mem_probe_fixup:
    mov eax, -1                 ; retour : faute
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
