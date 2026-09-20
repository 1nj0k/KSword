// 数值文本解析（shared/evidence/NumericTextParse.h）的离线测试。
//
// 为什么这个解析器值得一整套穷举断言：它属于**算错了不会报错**的那一类。
// 解析成另一个数字之后，界面照样显示、驱动照样去读，只是读的是另一个地址。
// 真实发生过的那次是：地址框输入纯数字串被按十进制解释，`1233` 变成 0x4D1，
// 落在进程的空指针保护区里，最终表现为"读取失败，错误码 299"——一个看起来
// 与解析毫无关系的症状，排查时先怀疑的是内存读取而不是输入解析。
//
// 断言原则与 DdmaPlanTests.cpp 一致：
//   * 期望值独立手算写死，绝不从被测函数反算；
//   * 边界两侧都测（进制回退的分界、uint64 溢出的分界）；
//   * 该被拒绝的输入必须被显式拒绝，不能只测通过路径；
//   * 两种默认进制各测一遍——同一个串在两种模式下必须得到不同结果，这正是
//     这个模块存在的理由，只测一种等于没测。

#include "TestSupport.h"

#include "../shared/evidence/NumericTextParse.h"

#include <cstdint>

namespace {

using ksword::evidence::NumericTextDefaultRadix;
using ksword::evidence::NumericTextRadix;
using ksword::evidence::ParseNumericText;

// 地址语义：无前缀按十六进制。
ksword::evidence::NumericTextParseResult Addr(const char* text) {
    return ParseNumericText(text, NumericTextDefaultRadix::Hexadecimal);
}

// 数量语义：无前缀按十进制（扇区 LBA、字节值、长度走这条）。
ksword::evidence::NumericTextParseResult Num(const char* text) {
    return ParseNumericText(text, NumericTextDefaultRadix::Decimal);
}

// ------------------------------------------------------------
// 一、缺陷本身：纯数字串在两种语义下必须分道扬镳。
// ------------------------------------------------------------
void TestBareDigitsSplitBySemantics(KswordTests::Suite& suite) {
    // 实际报障的那两个值，手算写死：
    //   0x1233 = 4659，十进制 1233 = 0x4D1；
    //   0x222222 = 2236962，十进制 222222 = 0x3640E。
    const auto addr1233 = Addr("1233");
    suite.expect(addr1233.ok && addr1233.value == 0x1233ULL,
        L"numeric text: bare 1233 is hex 0x1233 in address semantics");
    suite.expect(addr1233.radix == NumericTextRadix::Hexadecimal,
        L"numeric text: address semantics reports hexadecimal for bare digits");
    suite.expect(!addr1233.hadHexPrefix,
        L"numeric text: bare digits are not reported as prefixed");

    const auto num1233 = Num("1233");
    suite.expect(num1233.ok && num1233.value == 1233ULL,
        L"numeric text: bare 1233 stays decimal 1233 in quantity semantics");
    suite.expect(num1233.radix == NumericTextRadix::Decimal,
        L"numeric text: quantity semantics reports decimal for bare digits");

    // 两种语义对同一个串必须给出不同的值，否则这个模块白写了。
    suite.expect(addr1233.value != num1233.value,
        L"numeric text: the two semantics disagree on bare digits, which is the point");

    const auto addr222222 = Addr("222222");
    suite.expect(addr222222.ok && addr222222.value == 0x222222ULL,
        L"numeric text: bare 222222 is hex 0x222222 in address semantics");
    const auto num222222 = Num("222222");
    suite.expect(num222222.ok && num222222.value == 222222ULL,
        L"numeric text: bare 222222 stays decimal 222222 in quantity semantics");
    // 手算：222222 = 0x3640E，正是报障里出现的那个地址。
    suite.expect(num222222.value == 0x3640EULL,
        L"numeric text: decimal 222222 equals 0x3640E, the address that was actually reported");
}

// ------------------------------------------------------------
// 二、0x 前缀恒为十六进制，且优先于默认进制。
// ------------------------------------------------------------
void TestHexPrefixWinsOverDefault(KswordTests::Suite& suite) {
    for (const char* text : { "0x1233", "0X1233" }) {
        const auto asAddress = ParseNumericText(text, NumericTextDefaultRadix::Hexadecimal);
        const auto asQuantity = ParseNumericText(text, NumericTextDefaultRadix::Decimal);
        suite.expect(asAddress.ok && asAddress.value == 0x1233ULL,
            L"numeric text: 0x prefix parses as hex under address semantics");
        suite.expect(asQuantity.ok && asQuantity.value == 0x1233ULL,
            L"numeric text: 0x prefix parses as hex under quantity semantics too");
        suite.expect(asAddress.hadHexPrefix && asQuantity.hadHexPrefix,
            L"numeric text: the 0x prefix is reported back to the caller");
        suite.expect(asAddress.radix == NumericTextRadix::Hexadecimal
            && asQuantity.radix == NumericTextRadix::Hexadecimal,
            L"numeric text: a prefixed value reports hexadecimal in both semantics");
    }

    // 大小写混排的数位同样成立。
    const auto mixedCase = Addr("0xAbCdEf");
    suite.expect(mixedCase.ok && mixedCase.value == 0xABCDEFULL,
        L"numeric text: hex digits are case-insensitive");

    // 只有前缀没有数位必须被拒绝，绝不能退化成 0。0 是一个合法地址，
    // 也是 DDMA 里的 MBR 扇区号——退化成它的代价是实打实的。
    suite.expect(!Addr("0x").ok, L"numeric text: a bare 0x prefix is rejected, not treated as 0");
    suite.expect(!Num("0x").ok, L"numeric text: a bare 0x prefix is rejected in quantity semantics");
    suite.expect(!Addr("0X").ok, L"numeric text: a bare 0X prefix is rejected");
}

// ------------------------------------------------------------
// 三、十进制语义下的十六进制回退：分界正是"串里有没有 a–f"。
// ------------------------------------------------------------
void TestDecimalFallbackBoundary(KswordTests::Suite& suite) {
    // 含 a–f -> 十进制不成立 -> 回退十六进制。
    const auto withLetter = Num("1a");
    suite.expect(withLetter.ok && withLetter.value == 0x1AULL,
        L"numeric text: quantity semantics falls back to hex when a digit is a-f");
    suite.expect(withLetter.radix == NumericTextRadix::Hexadecimal,
        L"numeric text: the fallback reports hexadecimal, not decimal");

    // 不含 a–f -> 十进制成立 -> 回退一次也走不到。这条就是原缺陷的成因，
    // 在数量语义下它是**正确行为**，必须钉住，免得连带被"修"掉。
    const auto withoutLetter = Num("19");
    suite.expect(withoutLetter.ok && withoutLetter.value == 19ULL,
        L"numeric text: quantity semantics keeps decimal when every digit is 0-9");
    suite.expect(withoutLetter.radix == NumericTextRadix::Decimal,
        L"numeric text: no fallback happens when the decimal parse succeeds");

    // 地址语义没有回退可言：十六进制字符集是十进制的超集，两条路是同一条。
    const auto addrWithLetter = Addr("1a");
    const auto addrWithoutLetter = Addr("19");
    suite.expect(addrWithLetter.ok && addrWithLetter.value == 0x1AULL,
        L"numeric text: address semantics reads a-f directly");
    suite.expect(addrWithoutLetter.ok && addrWithoutLetter.value == 0x19ULL,
        L"numeric text: address semantics reads 19 as 0x19, never as decimal 19");
    suite.expect(addrWithoutLetter.value != 19ULL,
        L"numeric text: 0x19 and decimal 19 are different values and must not be conflated");
}

// ------------------------------------------------------------
// 四、溢出：必须判失败，绝不回绕。
// ------------------------------------------------------------
void TestOverflowIsRejected(KswordTests::Suite& suite) {
    // uint64 上限 = 0xFFFFFFFFFFFFFFFF = 18446744073709551615。两侧都测。
    const auto hexMax = Addr("FFFFFFFFFFFFFFFF");
    suite.expect(hexMax.ok && hexMax.value == 0xFFFFFFFFFFFFFFFFULL,
        L"numeric text: the largest representable hex value is accepted");
    suite.expect(!Addr("10000000000000000").ok,
        L"numeric text: one past the largest hex value is rejected, not wrapped");
    suite.expect(!Addr("0x10000000000000000").ok,
        L"numeric text: a prefixed value one past the limit is rejected too");

    const auto decimalMax = Num("18446744073709551615");
    suite.expect(decimalMax.ok && decimalMax.value == 0xFFFFFFFFFFFFFFFFULL,
        L"numeric text: the largest representable decimal value is accepted");
    suite.expect(!Num("18446744073709551616").ok,
        L"numeric text: one past the largest decimal value is rejected");

    // 十进制溢出后不许"回退到十六进制"把它救回来：同一串数字按十六进制读只会
    // 更大，救回来的一定是个错的数。
    suite.expect(!Num("99999999999999999999").ok,
        L"numeric text: a decimal overflow does not get rescued by the hex fallback");

    // 前导零不算溢出。
    const auto padded = Addr("0000000000000000000000001233");
    suite.expect(padded.ok && padded.value == 0x1233ULL,
        L"numeric text: leading zeros do not overflow");
}

// ------------------------------------------------------------
// 五、非法输入必须被显式拒绝，而不是解析出半截。
// ------------------------------------------------------------
void TestRejectedInputs(KswordTests::Suite& suite) {
    const char* const rejected[] = {
        "",            // 空串
        "   ",         // 全空白
        "g",           // 越出十六进制字符集
        "12g4",        // 尾部越界
        "0x12g4",      // 前缀之后越界
        "-1",          // 负号：地址没有负数，接受它等于接受一次回绕
        "+1",          // 正号：同样不接受，宁可让用户去掉
        "12 34",       // 内部空格：这是输入错误，不是 1234
        "1,234",       // 千分位
        "1_234",       // 下划线分隔
        "0x1.8",       // 小数点
        "1233h",       // 汇编风格的后缀，本工具不支持
        "#1233",       // 其它前缀
    };
    for (const char* text : rejected) {
        suite.expect(!Addr(text).ok, L"numeric text: malformed input is rejected in address semantics");
        suite.expect(!Num(text).ok, L"numeric text: malformed input is rejected in quantity semantics");
    }

    // 失败时输出参数必须保持在安全的初值上，调用方哪怕忘了看返回值也不会拿到
    // 上一次的残留。
    const auto failed = Addr("g");
    suite.expect(failed.value == 0ULL, L"numeric text: a failed parse leaves the value at 0");
    suite.expect(failed.radix == NumericTextRadix::None,
        L"numeric text: a failed parse reports no radix");
    suite.expect(!failed.hadHexPrefix, L"numeric text: a failed parse reports no prefix");
}

// ------------------------------------------------------------
// 六、只有 0x 是前缀，别的"看起来像前缀"的写法一律当普通数位。
// ------------------------------------------------------------
void TestOnlyHexPrefixIsSpecialCased(KswordTests::Suite& suite) {
    // `0b1010` 常被当成二进制写法，但本工具从未承诺过 0b。它在十六进制字符集里
    // 每一位都合法，于是整串按十六进制读 = 0x0B1010。这里把这个行为**钉死**，
    // 是因为它容易被后来人当成缺陷顺手"修"成拒绝或修成二进制，而那两种改法都会
    // 让 `0b1010` 这个输入的含义再变一次。
    const auto binaryLooking = Addr("0b1010");
    suite.expect(binaryLooking.ok && binaryLooking.value == 0x0B1010ULL,
        L"numeric text: 0b1010 is read as the hex digits 0B1010, with no binary prefix special case");
    suite.expect(!binaryLooking.hadHexPrefix,
        L"numeric text: 0b is not reported as a prefix");

    // 数量语义下同一个串走的是"十进制失败 -> 十六进制回退"，结果相同。
    const auto asQuantity = Num("0b1010");
    suite.expect(asQuantity.ok && asQuantity.value == 0x0B1010ULL,
        L"numeric text: 0b1010 falls back to the same hex reading in quantity semantics");

    // 单个 0 必须仍然是 0，不能被"前缀检测"顺手吃掉。
    const auto zero = Addr("0");
    suite.expect(zero.ok && zero.value == 0ULL,
        L"numeric text: a lone 0 parses as 0 and is not mistaken for a prefix");
    const auto zeroQuantity = Num("0");
    suite.expect(zeroQuantity.ok && zeroQuantity.value == 0ULL,
        L"numeric text: a lone 0 parses as 0 in quantity semantics");
}

} // namespace

int RunNumericTextParseTests() {
    KswordTests::Suite suite(L"ADDR numeric text");
    TestBareDigitsSplitBySemantics(suite);
    TestHexPrefixWinsOverDefault(suite);
    TestDecimalFallbackBoundary(suite);
    TestOverflowIsRejected(suite);
    TestRejectedInputs(suite);
    TestOnlyHexPrefixIsSpecialCased(suite);
    suite.report();
    return suite.failures();
}
