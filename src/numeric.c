#include "ternfold/numeric.h"

#include <stdbool.h>

enum {
    PREFIX_DIGITS = 768,
    DECIMAL_CLAMP = 325,
    /* The largest shifted dividend/divisor, including twice a remainder,
     * needs at most 2606 bits. See rounded_bits. */
    BIG_LIMBS = 84
};

typedef struct {
    uint32_t limb[BIG_LIMBS]; /* least significant limb first */
    int used;
} Big;

typedef struct {
    Big prefix;
    size_t leading_zeros;
    int kept;
    bool sticky;
} Decimal;

static bool digit(unsigned char byte)
{
    return byte >= 0x30 && byte <= 0x39;
}

TfNumericStatus tf_parse_int_magnitude(const unsigned char *bytes, size_t length,
                                       uint64_t *out)
{
    const uint64_t limit = UINT64_C(1) << 63;
    uint64_t value = 0;
    bool range = false;
    if (bytes == NULL || out == NULL || length == 0 ||
        (length > 1 && bytes[0] == 0x30)) {
        return TF_NUMERIC_INVALID;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!digit(bytes[i])) return TF_NUMERIC_INVALID;
        const unsigned n = (unsigned)bytes[i] - 0x30u;
        if (value > (limit - n) / 10u) range = true;
        if (!range) value = value * 10u + n;
    }
    if (range) return TF_NUMERIC_RANGE;
    *out = value;
    return TF_NUMERIC_OK;
}

static void big_mul_add(Big *value, unsigned factor, unsigned addend)
{
    uint64_t carry = addend;
    for (int i = 0; i < value->used; ++i) {
        const uint64_t next = (uint64_t)value->limb[i] * factor + carry;
        value->limb[i] = (uint32_t)next;
        carry = next >> 32;
    }
    if (carry != 0) value->limb[value->used++] = (uint32_t)carry;
}

static void append_digit(Decimal *decimal, unsigned char byte)
{
    const unsigned n = (unsigned)byte - 0x30u;
    if (decimal->kept == 0 && n == 0) {
        ++decimal->leading_zeros;
    } else if (decimal->kept < PREFIX_DIGITS) {
        big_mul_add(&decimal->prefix, 10, n);
        ++decimal->kept;
    } else if (n != 0) {
        decimal->sticky = true;
    }
}

static int big_bits(const Big *value)
{
    if (value->used == 0) return 0;
    int bits = (value->used - 1) * 32;
    for (uint32_t top = value->limb[value->used - 1]; top != 0; top >>= 1) {
        ++bits;
    }
    return bits;
}

static int big_cmp(const Big *a, const Big *b)
{
    if (a->used != b->used) return a->used > b->used ? 1 : -1;
    for (int i = a->used; i > 0; --i) {
        const uint32_t left = a->limb[i - 1];
        const uint32_t right = b->limb[i - 1];
        if (left != right) return left > right ? 1 : -1;
    }
    return 0;
}

static void big_shl(Big *value, int shift)
{
    if (value->used == 0) return;
    const int words = shift / 32;
    const int bits = shift % 32;
    if (words != 0) {
        for (int i = value->used; i > 0; --i) {
            value->limb[i - 1 + words] = value->limb[i - 1];
        }
        for (int i = 0; i < words; ++i) value->limb[i] = 0;
        value->used += words;
    }
    uint64_t carry = 0;
    for (int i = words; i < value->used; ++i) {
        const uint64_t next = ((uint64_t)value->limb[i] << bits) | carry;
        value->limb[i] = (uint32_t)next;
        carry = next >> 32;
    }
    if (carry != 0) value->limb[value->used++] = (uint32_t)carry;
}

static void big_shr_one(Big *value)
{
    uint32_t carry = 0;
    for (int i = value->used; i > 0; --i) {
        const uint32_t word = value->limb[i - 1];
        value->limb[i - 1] = (word >> 1) | carry;
        carry = (word & UINT32_C(1)) << 31;
    }
    if (value->used > 0 && value->limb[value->used - 1] == 0) --value->used;
}

/* Requires a >= b. */
static void big_sub(Big *a, const Big *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < a->used; ++i) {
        const uint64_t left = a->limb[i];
        const uint64_t right = (i < b->used ? b->limb[i] : 0u) + borrow;
        a->limb[i] = (uint32_t)(left - right);
        borrow = left < right ? 1u : 0u;
    }
    while (a->used > 0 && a->limb[a->used - 1] == 0) --a->used;
}

/* Computes floor(log2(num/den)) for positive operands by aligning top bits.
 * This temporary shift reaches at most the larger input bit length. */
static int ratio_exponent(const Big *num, const Big *den)
{
    const int delta = big_bits(num) - big_bits(den);
    Big shifted = delta < 0 ? *num : *den;
    big_shl(&shifted, delta < 0 ? -delta : delta);
    const int cmp = delta < 0 ? big_cmp(&shifted, den) : big_cmp(num, &shifted);
    return cmp < 0 ? delta - 1 : delta;
}

/* Return floor(num/den), leaving the remainder in num. This ratio is < 2^53. */
static uint64_t big_divide(Big *num, const Big *den)
{
    uint64_t quotient = 0;
    Big divisor = *den;
    int shift = big_bits(num) - big_bits(den);
    if (shift < 0) return 0;
    big_shl(&divisor, shift);
    for (; shift >= 0; --shift) {
        if (big_cmp(num, &divisor) >= 0) {
            big_sub(num, &divisor);
            quotient |= UINT64_C(1) << shift;
        }
        big_shr_one(&divisor);
    }
    return quotient;
}

/* Combine the exponent with the signed displacement of the leading digit.
 * Keep the last exponent digit separate: cancellation is possible even when
 * its magnitude exceeds SIZE_MAX. Subtract large opposite-signed terms before
 * clamping. An overflowing tens field already dwarfs any source offset. */
static int adjusted_exponent(size_t offset, bool offset_negative,
                             size_t tens, unsigned units, bool negative,
                             bool tens_overflow)
{
    if (tens_overflow) return negative ? -DECIMAL_CLAMP : DECIMAL_CLAMP;
    const size_t offset_tens = offset / 10u;
    const int offset_units = (int)(offset % 10u);
    int adjusted;
    if (negative == offset_negative) {
        if (tens > 32u || offset_tens > 32u) {
            return negative ? -DECIMAL_CLAMP : DECIMAL_CLAMP;
        }
        adjusted = ((int)tens + (int)offset_tens) * 10 + (int)units + offset_units;
    } else {
        const bool larger_tens = tens >= offset_tens;
        const size_t gap = larger_tens ? tens - offset_tens : offset_tens - tens;
        if (gap > 33u) {
            return negative == larger_tens ? -DECIMAL_CLAMP : DECIMAL_CLAMP;
        }
        adjusted = (larger_tens ? (int)gap : -(int)gap) * 10 + (int)units - offset_units;
    }
    if (negative) adjusted = -adjusted;
    if (adjusted < -DECIMAL_CLAMP) return -DECIMAL_CLAMP;
    if (adjusted > DECIMAL_CLAMP) return DECIMAL_CLAMP;
    return adjusted;
}

static TfNumericStatus rounded_bits(const Decimal *decimal, int scientific,
                                    uint64_t *out)
{
    if (scientific > 308) return TF_NUMERIC_RANGE;
    if (scientific < -324) {
        *out = 0;
        return TF_NUMERIC_OK;
    }

    /* The prefix represents (num/den) * 2^power, with powers of five in one
     * side of the ratio. The prefix has at most 2552 bits; 5^1091 has at most
     * 2534. For positive power, the whole value is below 10^309, which also
     * bounds num. In the rounding scale the ratio lies in [1/2, 2^53).
     * Only one side is shifted: the dividend can gain at most 53 bits over
     * the unshifted divisor, or the divisor at most one over the unshifted
     * dividend. Doubling the remainder fits within 2552 + 53 + 1 bits. */
    const int power = scientific - (decimal->kept - 1);
    Big num = decimal->prefix;
    Big den = {{1}, 1};
    Big *scaled = power < 0 ? &den : &num;
    for (int i = 0; i < (power < 0 ? -power : power); ++i) big_mul_add(scaled, 5, 0);

    int exponent = ratio_exponent(&num, &den) + power;
    if (exponent > 1023) return TF_NUMERIC_RANGE;
    if (exponent < -1075) {
        *out = 0;
        return TF_NUMERIC_OK;
    }

    /* Divide in units of the result's ulp. Subnormals (and zero) share the
     * fixed spacing 2^-1074. Binary search is unnecessary: the quotient has
     * at most 53 bits, and the exact remainder resolves the rounding. */
    const int unit = (exponent < -1022 ? -1022 : exponent) - 52;
    const int shift = power - unit;
    if (shift < 0) big_shl(&den, -shift);
    else big_shl(&num, shift);
    uint64_t significand = big_divide(&num, &den);
    big_shl(&num, 1);
    const int half = big_cmp(&num, &den);

    /* Any rounding midpoint is m * 2^e with m < 2^54 and e >= -1075.
     * For e < 0 its exact decimal coefficient is at most m * 5^1075
     * (at most 768 digits); for e >= 0 the whole value has <= 309 digits.
     * Thus no midpoint is strictly inside a 768-digit prefix interval.
     * Discarded nonzero digits can only change an exact tie into above-half.
     * This includes the zero and overflow midpoints. */
    if (half > 0 || (half == 0 && (decimal->sticky || (significand & 1u) != 0))) {
        ++significand;
    }
    if (exponent < -1022) {
        /* Carry out of a subnormal becomes the smallest normal bit pattern. */
        *out = significand;
        return TF_NUMERIC_OK;
    }
    if (significand == (UINT64_C(1) << 53)) {
        significand >>= 1;
        if (++exponent > 1023) return TF_NUMERIC_RANGE;
    }
    *out = ((uint64_t)(exponent + 1023) << 52) | (significand - (UINT64_C(1) << 52));
    return TF_NUMERIC_OK;
}

TfNumericStatus tf_parse_float_bits(const unsigned char *bytes, size_t length,
                                    uint64_t *out)
{
    if (bytes == NULL || out == NULL || length == 0 || !digit(bytes[0]) ||
        (length > 1 && bytes[0] == 0x30 && digit(bytes[1]))) {
        return TF_NUMERIC_INVALID;
    }
    Decimal decimal = {0};
    size_t i = 0;
    while (i < length && digit(bytes[i])) append_digit(&decimal, bytes[i++]);
    const size_t integer_digits = i;
    const bool fraction = i < length && bytes[i] == 0x2e;
    if (fraction) {
        ++i;
        if (i == length || !digit(bytes[i])) return TF_NUMERIC_INVALID;
        while (i < length && digit(bytes[i])) append_digit(&decimal, bytes[i++]);
    }

    size_t tens = 0;
    unsigned units = 0;
    bool negative = false;
    bool tens_overflow = false;
    if (i < length && (bytes[i] == 0x65 || bytes[i] == 0x45)) {
        ++i;
        if (i < length && (bytes[i] == 0x2b || bytes[i] == 0x2d)) {
            negative = bytes[i++] == 0x2d;
        }
        if (i == length) return TF_NUMERIC_INVALID;
        for (; i < length; ++i) {
            if (!digit(bytes[i])) return TF_NUMERIC_INVALID;
            const unsigned n = (unsigned)bytes[i] - 0x30u;
            if (i == length - 1) units = n;
            else if (!tens_overflow) {
                if (tens > (SIZE_MAX - n) / 10u) tens_overflow = true;
                else tens = tens * 10u + n;
            }
        }
    } else if (!fraction || i != length) {
        return TF_NUMERIC_INVALID;
    }
    if (decimal.kept == 0) {
        *out = 0;
        return TF_NUMERIC_OK;
    }
    const size_t first = decimal.leading_zeros + 1; /* nonzero digit was present */
    const bool offset_negative = first > integer_digits;
    const size_t offset = offset_negative ? first - integer_digits : integer_digits - first;
    return rounded_bits(&decimal, adjusted_exponent(offset, offset_negative,
                                                  tens, units, negative, tens_overflow), out);
}
