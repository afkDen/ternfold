#include "scalars.h"

/* Text contains UTF-8 bytes. Use ASCII byte values independent of locale
 * and of the C execution character set. */
enum {
    ASCII_MINUS = 0x2d,
    ASCII_DOT = 0x2e,
    ASCII_ZERO = 0x30,
    ASCII_NINE = 0x39,
    ASCII_E = 0x65,
    BINARY64_FRACTION_BITS = 52,
    BINARY64_BIAS = 1023,
    BINARY64_NONFINITE = 2047,
    FLOAT_TEXT_DIGITS = 17,
    DECIMAL_LIMB_DIGITS = 9,
    /* m < 10^16, |q| <= 1074 and both scaling bases are <= 5.
     * Since 5^10 < 10^7, m * 5^1074 < 10^(16+108*7) = 10^772. */
    DECIMAL_LIMBS = 86
};

#define BINARY64_SIGN (UINT64_C(1) << 63)
#define BINARY64_HIDDEN (UINT64_C(1) << BINARY64_FRACTION_BITS)
#define DECIMAL_RADIX UINT32_C(1000000000)

typedef struct {
    uint32_t limbs[DECIMAL_LIMBS]; /* Least significant first. */
    size_t used;
} Decimal;

static int exponent_field(uint64_t bits) {
    return (int)((bits >> BINARY64_FRACTION_BITS) & UINT64_C(0x7ff));
}

static uint64_t significand(uint64_t bits, int field) {
    return (bits & (BINARY64_HIDDEN - 1)) | (field != 0 ? BINARY64_HIDDEN : 0);
}

static uint64_t int_limit(bool negative) {
    return negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
}

/* The callers have already checked magnitude against int_limit. */
static int64_t signed_magnitude(uint64_t magnitude, bool negative) {
    if (negative && magnitude == int_limit(true)) return INT64_MIN;
    return negative ? -(int64_t)magnitude : (int64_t)magnitude;
}

bool tf_exec_float_int(uint64_t bits, int64_t *out) {
    const int field = exponent_field(bits);
    const int power = field - BINARY64_BIAS;
    if (power > 63) return false; /* Includes nonfinite patterns. */
    if (power < 0) {
        *out = 0; /* Also covers all subnormals and both signed zeros. */
        return true;
    }
    const bool negative = (bits & BINARY64_SIGN) != 0;
    const uint64_t mantissa = significand(bits, field);
    /* The largest possible left shift is 11 on a 53-bit significand.
     * A right shift discards the fraction before any signed conversion. */
    const uint64_t magnitude = power >= BINARY64_FRACTION_BITS
        ? mantissa << (power - BINARY64_FRACTION_BITS)
        : mantissa >> (BINARY64_FRACTION_BITS - power);
    if (magnitude > int_limit(negative)) return false;
    *out = signed_magnitude(magnitude, negative);
    return true;
}

bool tf_exec_parse_int(const unsigned char *bytes, size_t length, int64_t *out) {
    if (length == 0 || length >= TF_EXEC_INT_TEXT_CAPACITY) return false;
    const bool negative = bytes[0] == ASCII_MINUS;
    size_t at = negative ? 1u : 0u;
    if (at == length || (bytes[at] == ASCII_ZERO && length - at != 1)) return false;
    const uint64_t limit = int_limit(negative);
    uint64_t magnitude = 0;
    for (; at < length; ++at) {
        const unsigned char byte = bytes[at];
        if (byte < ASCII_ZERO || byte > ASCII_NINE) return false;
        const uint64_t digit = (uint64_t)(byte - ASCII_ZERO);
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    *out = signed_magnitude(magnitude, negative);
    return true;
}

static size_t digit_count(uint64_t value) {
    size_t count = 1;
    while (value >= 10) {
        value /= 10;
        ++count;
    }
    return count;
}

/* Write exactly width digits, including leading zeros for lower limbs. */
static void write_digits(uint64_t value, size_t width, char *out) {
    while (width != 0) {
        out[--width] = (char)(ASCII_ZERO + value % 10);
        value /= 10;
    }
}

size_t tf_exec_int_text(int64_t value, char out[static TF_EXEC_INT_TEXT_CAPACITY]) {
    const bool negative = value < 0;
    const uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    const size_t start = negative ? 1u : 0u;
    const size_t count = digit_count(magnitude);
    if (negative) out[0] = ASCII_MINUS;
    write_digits(magnitude, count, out + start);
    out[start + count] = '\0';
    return start + count;
}

static void decimal_mul(Decimal *decimal, uint32_t factor) {
    uint64_t carry = 0;
    for (size_t i = 0; i < decimal->used; ++i) {
        /* Both factor and carry are below the radix, so the sum fits. */
        const uint64_t product = (uint64_t)decimal->limbs[i] * factor + carry;
        decimal->limbs[i] = (uint32_t)(product % DECIMAL_RADIX);
        carry = product / DECIMAL_RADIX;
    }
    if (carry != 0) decimal->limbs[decimal->used++] = (uint32_t)carry;
}

static void decimal_scale(Decimal *decimal, int power) {
    const uint32_t base = power < 0 ? 5u : 2u;
    const int chunk = power < 0 ? 12 : 29;
    const uint32_t factor = power < 0 ? UINT32_C(244140625) : UINT32_C(536870912);
    int count = power < 0 ? -power : power;
    for (; count >= chunk; count -= chunk) decimal_mul(decimal, factor);
    uint32_t tail = 1;
    for (; count > 0; --count) tail *= base;
    if (tail != 1) decimal_mul(decimal, tail);
}

static bool round_up(const char *digits, size_t count, const Decimal *decimal,
                     size_t remaining) {
    const int guard = digits[FLOAT_TEXT_DIGITS] - ASCII_ZERO;
    if (guard != 5) return guard > 5;
    if ((digits[FLOAT_TEXT_DIGITS - 1] - ASCII_ZERO) % 2 != 0) return true;
    for (size_t i = FLOAT_TEXT_DIGITS + 1; i < count; ++i)
        if (digits[i] != ASCII_ZERO) return true;
    for (size_t i = 0; i < remaining; ++i)
        if (decimal->limbs[i] != 0) return true;
    return false;
}

static size_t scientific_text(bool negative, const char *digits, size_t count,
                              int exponent, char *out) {
    size_t n = 0;
    if (negative) out[n++] = ASCII_MINUS;
    out[n++] = digits[0];
    out[n++] = ASCII_DOT;
    if (count == 1) out[n++] = ASCII_ZERO;
    for (size_t i = 1; i < count; ++i) out[n++] = digits[i];
    out[n++] = ASCII_E;
    if (exponent < 0) out[n++] = ASCII_MINUS;
    const uint64_t magnitude = (uint64_t)(exponent < 0 ? -exponent : exponent);
    const size_t width = digit_count(magnitude);
    write_digits(magnitude, width, out + n);
    n += width;
    out[n] = '\0';
    return n;
}

size_t tf_exec_float_text(uint64_t bits, char out[static TF_EXEC_FLOAT_TEXT_CAPACITY]) {
    const int field = exponent_field(bits);
    if (field == BINARY64_NONFINITE) return 0;
    uint64_t mantissa = significand(bits, field);
    char digits[FLOAT_TEXT_DIGITS + DECIMAL_LIMB_DIGITS];
    if (mantissa == 0) {
        digits[0] = ASCII_ZERO;
        return scientific_text(false, digits, 1, 0, out);
    }

    /* Binary magnitude is m * 2^q. For q < 0, compute the exact decimal
     * numerator m * 5^-q with scale -q; otherwise m * 2^q is integral. */
    const int power = (field == 0 ? 1 : field) - BINARY64_BIAS - BINARY64_FRACTION_BITS;
    Decimal decimal;
    decimal.used = 0;
    for (; mantissa != 0; mantissa /= DECIMAL_RADIX)
        decimal.limbs[decimal.used++] = (uint32_t)(mantissa % DECIMAL_RADIX);
    decimal_scale(&decimal, power);

    size_t remaining = decimal.used - 1;
    size_t count = digit_count(decimal.limbs[remaining]);
    int exponent = (int)(count + remaining * DECIMAL_LIMB_DIGITS) - 1;
    if (power < 0) exponent += power;

    /* Materialize only enough high limbs to get the guard digit, if any.
     * Lower limbs still contribute to the sticky bit in a halfway test. */
    write_digits(decimal.limbs[remaining], count, digits);
    while (count <= FLOAT_TEXT_DIGITS && remaining != 0) {
        write_digits(decimal.limbs[--remaining], DECIMAL_LIMB_DIGITS, digits + count);
        count += DECIMAL_LIMB_DIGITS;
    }
    if (count > FLOAT_TEXT_DIGITS) {
        if (round_up(digits, count, &decimal, remaining)) {
            size_t at = FLOAT_TEXT_DIGITS;
            while (at != 0 && digits[at - 1] == ASCII_NINE) digits[--at] = ASCII_ZERO;
            if (at != 0) {
                ++digits[at - 1];
            } else {
                digits[0] = ASCII_ZERO + 1;
                ++exponent;
            }
        }
        count = FLOAT_TEXT_DIGITS;
    }
    while (count > 2 && digits[count - 1] == ASCII_ZERO) --count;
    return scientific_text((bits & BINARY64_SIGN) != 0, digits, count, exponent, out);
}
