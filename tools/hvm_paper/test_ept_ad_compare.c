/* Test the exact pure policy included by the driver, including every bit. */
#include <assert.h>
#include <stdio.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_nested_ad_compare.h"

int main(void)
{
    const unsigned long long frame = 0x12345000ULL | 0x37ULL;
    unsigned int level, bit;
    for (level = 0; level < 4; ++level) {
        unsigned long long base = frame | ((level == 1 || level == 2) ? 0x80ULL : 0ULL);
        assert(KswordHvmEptEntryKeepsTranslation(base, base, level, 0));
        for (bit = 0; bit < 64; ++bit) {
            unsigned long long changed = base ^ (1ULL << bit);
            int expected = bit == 8 || (bit == 9 && level != 0);
            assert(KswordHvmEptEntryKeepsTranslation(base, changed, level, 1) == expected);
            assert(!KswordHvmEptEntryKeepsTranslation(base, changed, level, 0));
        }
        assert(!KswordHvmEptEntryKeepsTranslation(base | 0x300ULL, base, level, 1));
        assert(!KswordHvmEptEntryKeepsTranslation(base | 0x300ULL, base | 0x100ULL, level, 1));
        assert(!KswordHvmEptEntryKeepsTranslation(0, 0x100ULL, level, 1));
        if (level < 3) {
            assert(!KswordHvmEptEntryKeepsTranslation(frame, frame | 0x200ULL, level, 1));
        }
    }
    puts("EPT_AD_COMPARE=PASS: all 64 bits, all levels, A/D on/off, clears, invalid entries");
    return 0;
}
