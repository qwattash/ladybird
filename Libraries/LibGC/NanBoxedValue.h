/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Cheri.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>

namespace GC {

namespace Detail {

static_assert(sizeof(double) == 8);
#ifndef AK_ARCH_CHERI
static_assert(sizeof(void*) == sizeof(double) || sizeof(void*) == sizeof(u32));
// To make our Value representation compact we can use the fact that IEEE
// doubles have a lot (2^52 - 2) of NaN bit patterns. The canonical form being
// just 0x7FF8000000000000 i.e. sign = 0 exponent is all ones and the top most
// bit of the mantissa set.
static constexpr u64 CANON_NAN_BITS = bit_cast<u64>(__builtin_nan(""));
static_assert(CANON_NAN_BITS == 0x7FF8000000000000);
// (Unfortunately all the other values are valid so we have to convert any
// incoming NaNs to this pattern although in practice it seems only the negative
// version of these CANON_NAN_BITS)
// +/- Infinity are represented by a full exponent but without any bits of the
// mantissa set.
static constexpr u64 POSITIVE_INFINITY_BITS = bit_cast<u64>(__builtin_huge_val());
static constexpr u64 NEGATIVE_INFINITY_BITS = bit_cast<u64>(-__builtin_huge_val());
static_assert(POSITIVE_INFINITY_BITS == 0x7FF0000000000000);
static_assert(NEGATIVE_INFINITY_BITS == 0xFFF0000000000000);
// However as long as any bit is set in the mantissa with the exponent of all
// ones this value is a NaN, and it even ignores the sign bit.
// (NOTE: we have to use __builtin_isnan here since some isnan implementations are not constexpr)
static_assert(__builtin_isnan(bit_cast<double>(0x7FF0000000000001)));
static_assert(__builtin_isnan(bit_cast<double>(0xFFF0000000040000)));
// This means we can use all of these NaNs to store all other options for Value.
// To make sure all of these other representations we use 0x7FF8 as the base top
// 2 bytes which ensures the value is always a NaN.
static constexpr u64 BASE_TAG = 0x7FF8;
// This leaves the sign bit and the three lower bits for tagging a value and then
// 48 bits of potential payload.
// First the pointer backed types (Object, String etc.), to signify this category
// and make stack scanning easier we use the sign bit (top most bit) of 1 to
// signify that it is a pointer backed type.
static constexpr u64 IS_CELL_BIT = 0x8000 | BASE_TAG;
// On all current 64-bit systems this code runs pointer actually only use the
// lowest 6 bytes which fits neatly into our NaN payload with the top two bytes
// left over for marking it as a NaN and tagging the type.
// Note that we do need to take care when extracting the pointer value but this
// is explained in the extract_pointer method.

static constexpr u64 IS_CELL_PATTERN = 0xFFF8ULL;
static constexpr u64 TAG_SHIFT = 48;
static constexpr u64 TAG_EXTRACTION = 0xFFFF000000000000;
static constexpr u64 SHIFTED_IS_CELL_PATTERN = IS_CELL_PATTERN << TAG_SHIFT;
#endif /* ! AK_ARCH_CHERI */

// In CHERI all these assumptions break down, because you can't use the
// BASE_TAG for the top bytes of a capability and expect it to survive.
// Similary, capabilities have more than 48 bits of payload and the top
// u64 word can not be mangled.
// We use a CHERI-specific boxed value instead, which also gives us more
// bytes to work with for non-pointer values.
//
// We use the CHERI tag to distinguish between pointers and non-pointers.
// Tagging pointers is more complicated, because we would need TBI
// (Top Byte Ignore) for AArch64 or Pointer Masking in RISC-V.
// It is unclear how much we can rely on object alignment, but to use 3
// bits of flags we must assume that cell pointers are 16-bytes aligned.
// This is probably ok on 128bit CHERI, but more problematic on 64bit.
//
// Non-pointers can use the top 64bit word to encode the type flags, this
// means we don't have to play any tricks with the NaN encoding.

}; /* namespace Detail */

// There are three spare box tag bits.
// This representation is independent of the location of the bits in
// the encoded layout.
// Use the get_flags and set_flags accessors to handle flag bits.
// Use the is_cell() and set_cell() accessors to handle cell values.
static constexpr u8 TAG_FLAGS_MASK = 0x07;

class GC_API NanBoxedValue {
public:
    // Opaque value that wraps the encoded conttent of a NanBoxedValue.
    // This should not be manipulated arbitrarily, because the bit pattern
    // is thightly controlled here.
#ifdef AK_ARCH_CHERI
    using EncodedType = uintptr_t;
#else
    using EncodedType = u64;
#endif

    bool is_cell() const {
#ifdef AK_ARCH_CHERI
        return cheri_tag_get(m_value.encoded);
#else
        return (m_value.tag & Detail::IS_CELL_PATTERN) == Detail::IS_CELL_PATTERN;
#endif
    }

    static constexpr FlatPtr extract_pointer_bits(EncodedType encoded)
    {
#ifdef AK_ARCH_CHERI
        // In CHERI there are no flags for pointers, just return the entire value.
        return static_cast<FlatPtr>(encoded);
#elif defined(AK_ARCH_32_BIT)
        // For 32-bit system the pointer fully fits so we can just return it directly.
        static_assert(sizeof(void*) == sizeof(u32));
        return static_cast<FlatPtr>(encoded & 0xffff'ffff);
#elif ARCH(X86_64) || ARCH(RISCV64)
        // For x86_64 and riscv64 the top 16 bits should be sign extending the "real" top bit (47th).
        // So first shift the top 16 bits away then using the right shift it sign extends the top 16 bits.
        return static_cast<FlatPtr>((static_cast<i64>(encoded << 16)) >> 16);
#elif ARCH(AARCH64) || ARCH(PPC64) || ARCH(PPC64LE)
        // For AArch64 the top 16 bits of the pointer should be zero.
        // For PPC64: all 64 bits can be used for pointers, however on Linux only
        //            the lower 43 bits are used for user-space addresses, so
        //            masking off the top 16 bits should match the rest of LibGC.
        return static_cast<FlatPtr>(encoded & 0xffff'ffff'ffffULL);
#else
#    error "Unknown architecture. Don't know whether pointers need to be sign-extended."
#endif
    }

    template<typename PointerType>
    PointerType* extract_pointer() const
    {
        VERIFY(is_cell());
        return reinterpret_cast<PointerType*>(extract_pointer_bits(m_value.encoded));
    }

    Cell& as_cell()
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    Cell& as_cell() const
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    bool is_nan() const
    {
#ifdef AK_ARCH_CHERI
        return cheri_tag_get(m_value.encoded) == 0 && m_value.encoded == Detail::CANON_NAN_BITS;
#else
        return m_value.encoded == Detail::CANON_NAN_BITS;
#endif
    }

protected:
    // The flags accessor returns a 3 bit value that encodes the boxed type flags.
    // This abstracts away the exact placement of the flags in the encoded layout.
    u8 get_flags() const
    {
#ifdef AK_ARCH_CHERI
        if (cheri_tag_get(m_value.encoded)) {
            return m_value.encoded & TAG_FLAGS_MASK;
        } else {
            return m_value.tag;
        }
#else
        return m_value.tag & TAG_FLAGS_MASK;
#endif
    }

    void set_data(u64 value, u8 flags)
    {
        VERIFY((flags & ~TAG_FLAGS_MASK) == 0);
        flags = flags & TAG_FLAGS_MASK;
#ifdef AK_ARCH_CHERI
        m_value.tag = flags;
        m_value.payload = value;
#else
        VERIFY((value & Detail::TAG_EXTRACTION) == 0);
        m_value.encoded = ((Detail::BASE_TAG | flags) << TAG_SHIFT) | value;
#endif
    }

    // Set the embedded cell pointer, abstracting away the cell layout encoding
    void set_cell(Cell *cell, u8 flags)
    {
        VERIFY((flags & ~TAG_FLAGS_MASK) == 0);
#ifdef AK_ARCH_CHERI
        // Only allow valid pointers
        VERIFY(cheri_tag_get(cell));
        VERIFY(AK::is_aligned_to(cell, TAG_FLAGS_MASK + 1));
        m_value.encoded = reinterpret_cast<EncodedType>(cell) | flags;
#else
        EncodedType tag = (Detail::IS_CELL_BIT | flags) << TAG_SHIFT;
        m_value.encoded = tag | reinterpret_cast<EncodedType>(cell);
#endif
    }

    union {
#ifdef AK_ARCH_CHERI
#    ifndef AK_ARCH_64_BIT
#        error "32-bit CHERI is not supported yet"
#    endif
        // We will pretend big endian architectures do not exist.
        // Note: the tag here is just for non-pointer values.
        // Pointer values will be tagged in the low pointer bits.
        u64 tag;
        union {
            double as_double;
            u64 payload;
        };
#else
        double as_double;
        struct {
            u64 payload : 48;
            u64 tag : 16;
        };
#endif
        EncodedType encoded;
    } m_value { .encoded = 0 };
};

static_assert(sizeof(NanBoxedValue) == max(sizeof(double), sizeof(uintptr_t)));

}
