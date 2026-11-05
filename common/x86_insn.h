/*
 * Shared x86 (32-bit) instruction length decoding + prologue relocation
 * for inline hooks. Single implementation used by injected_runtime.c
 * and lua_api_hooks.c.
 *
 * Coverage is intentionally conservative: unknown opcodes return 0 and
 * hook installation fails instead of writing a corrupt trampoline.
 */
#ifndef COCOS_DEBUG_X86_INSN_H
#define COCOS_DEBUG_X86_INSN_H

#include <stdint.h>
#include <string.h>

static inline int x86_modrm_size(const uint8_t *p) {
    uint8_t modrm = p[0];
    int mod = modrm >> 6;
    int rm = modrm & 7;
    int n = 1;
    if (mod == 3) return 1;
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

static inline int x86_insn_len(const uint8_t *p) {
    int i = 0, n;
    uint8_t op;
    while (i < 4) {
        uint8_t b = p[i];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) {
            i++;
            continue;
        }
        break;
    }
    op = p[i++];
    if (op == 0x0F) {
        uint8_t op2;
        if (i >= 16) return 0;
        op2 = p[i++];
        if (op2 == 0x1F || op2 == 0xAE || op2 == 0x18 ||
            op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
            op2 == 0x2E || op2 == 0x2F || op2 == 0x57 || op2 == 0x58 ||
            op2 == 0x59 || op2 == 0x5E || op2 == 0x6F || op2 == 0x7F) {
            n = x86_modrm_size(p + i);
            if (n <= 0) return 0;
            return i + n;
        }
        return 0;
    }
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3) return i;
    if (op == 0x6A || op == 0xEB) return i + 1;
    if (op == 0x68 || op == 0xE8 || op == 0xE9) return i + 4;
    if (op >= 0xB8 && op <= 0xBF) return i + 4;
    if (op == 0xC2) return i + 2;
    if (op == 0x81) return i + x86_modrm_size(p + i) + 4;
    if (op == 0x83 || op == 0xC6) return i + x86_modrm_size(p + i) + 1;
    if (op == 0xC7) return i + x86_modrm_size(p + i) + 4;
    if (op == 0x8B || op == 0x89 || op == 0x8D || op == 0x85 ||
        op == 0x33 || op == 0x3B || op == 0x01 || op == 0x29 ||
        op == 0x39 || op == 0x31 || op == 0x8A || op == 0x88 || op == 0xFF) {
        n = x86_modrm_size(p + i);
        if (n <= 0) return 0;
        return i + n;
    }
    return 0;
}

/* Bytes to steal so a 5-byte E9 patch never splits an instruction. */
static inline int x86_measure_stolen(const uint8_t *p, int cap) {
    int stolen = 0;
    while (stolen < 5) {
        int n = x86_insn_len(p + stolen);
        if (n <= 0 || stolen + n > cap) return 0;
        stolen += n;
    }
    return stolen;
}

/*
 * Copy `stolen` bytes from src to dst, fixing up near call/jmp rel32 so the
 * copy stays valid at dst_va. Returns stolen on success, 0 on failure.
 * Short branches (EB, 70-7F) and far jcc (0F 8x) cannot be relocated with a
 * 1:1 copy: fail instead of silently producing a broken trampoline.
 */
static inline int x86_reloc_copy(uint8_t *dst, const uint8_t *src, int stolen,
                                 uint8_t *src_va, uint8_t *dst_va) {
    int off = 0;
    while (off < stolen) {
        int n = x86_insn_len(src + off);
        int i = 0;
        uint8_t op;
        if (n <= 0 || off + n > stolen) return 0;
        memcpy(dst + off, src + off, n);
        while (i < n) {
            uint8_t b = src[off + i];
            if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 ||
                b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) {
                i++;
                continue;
            }
            break;
        }
        op = src[off + i];
        if (op == 0xE8 || op == 0xE9) {
            int32_t oldrel;
            int32_t newrel;
            uintptr_t tgt;
            memcpy(&oldrel, src + off + i + 1, 4);
            tgt = (uintptr_t)(src_va + off + i + 5) + (uintptr_t)oldrel;
            newrel = (int32_t)(tgt - (uintptr_t)(dst_va + off + i + 5));
            memcpy(dst + off + i + 1, &newrel, 4);
        } else if (op == 0xEB || (op >= 0x70 && op <= 0x7F) ||
                   (op == 0x0F && i + 1 < n && src[off + i + 1] >= 0x80 &&
                    src[off + i + 1] <= 0x8F)) {
            return 0;
        }
        off += n;
    }
    return stolen;
}

#endif
