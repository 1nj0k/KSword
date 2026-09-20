#pragma once

// ============================================================
// NumericTextParse.h
// 作用：
// - 把用户输入的数值文本解析成 uint64，并且**在类型层面区分"地址"与"数量"**，
//   因为这两者的默认进制本来就不一样。
//
// 为什么要有这个模块：
// - 原先地址框与数量框共用同一个"先试十进制、失败再试十六进制"的解析器。那条
//   十六进制回退**只对含 a–f 的串生效**：纯数字串的十进制解析永远成立，回退
//   一次也走不到。于是在一个所有地址都以 0x 回显的工具里，输入 `1233` 会被解释
//   成十进制 1233（= 0x4D1），**安静地跳到另一个地址**——不报错、不提示，
//   只是读到了别处。地址越小越像"读取失败"，越不容易被认成解析问题。
// - 这类"算错了不会报错、只会去读写另一个位置"的逻辑，在本项目里必须被穷举
//   测试覆盖（同族的还有 DDMA 的 LBA 编码与 CDB 编码）。所以规则抽到这里：
//   不碰 Qt、不碰 Win32，可以在离线套件里直接跑。
//
// 规则：
// - `0x` / `0X` 前缀恒为十六进制，与默认进制无关。
// - 无前缀时按调用方声明的默认进制解析。地址用十六进制；数量（扇区 LBA、字节
//   值、长度）用十进制——数量本来就是人按十进制念的，那里不能跟着改。
// - 默认十进制时保留"无前缀十六进制"的回退（含 a–f 的串）；默认十六进制时
//   没有回退，因为十六进制的字符集是十进制的超集，回退无从谈起。
// - 溢出一律判失败，**绝不回绕**：回绕出来的值同样是个合法地址，会被照单读下去。
// - 不接受符号、千分位、内部空格。两端空白先剪掉。
// ============================================================

#include <cstdint>
#include <string_view>

namespace ksword::evidence
{
    // NumericTextRadix：实际采用的进制。返回它是为了让界面能回显"我把它当成了
    // 什么"，而不是只给一个数字让用户自己猜。
    enum class NumericTextRadix : int
    {
        None = 0,
        Decimal = 10,
        Hexadecimal = 16,
    };

    // NumericTextDefaultRadix：无前缀时按哪种进制解释。调用方必须显式选，
    // 没有默认值——这道题选错不会报错，不能靠默认参数糊过去。
    enum class NumericTextDefaultRadix : int
    {
        Hexadecimal = 0,
        Decimal = 1,
    };

    struct NumericTextParseResult
    {
        bool ok = false;
        std::uint64_t value = 0;
        NumericTextRadix radix = NumericTextRadix::None;
        bool hadHexPrefix = false;
    };

    // NumericTextDigitValue：单个字符在给定进制下的数位值；不是合法数位返回 -1。
    inline int NumericTextDigitValue(const char character, const int radix)
    {
        int digit = -1;
        if (character >= '0' && character <= '9')
        {
            digit = character - '0';
        }
        else if (character >= 'a' && character <= 'f')
        {
            digit = character - 'a' + 10;
        }
        else if (character >= 'A' && character <= 'F')
        {
            digit = character - 'A' + 10;
        }
        else
        {
            return -1;
        }
        return (digit < radix) ? digit : -1;
    }

    // NumericTextTrim：剪掉两端空白。内部空白不剪——"12 34"是输入错误，
    // 不是"1234"。
    inline std::string_view NumericTextTrim(std::string_view text)
    {
        const auto isSpace = [](const char character) {
            return character == ' ' || character == '\t' || character == '\r'
                || character == '\n' || character == '\f' || character == '\v';
        };
        while (!text.empty() && isSpace(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && isSpace(text.back()))
        {
            text.remove_suffix(1);
        }
        return text;
    }

    // NumericTextParseDigits：整串按给定进制解析，任何一个字符不合法都判失败。
    // 允许前导零；不允许空串。
    inline bool NumericTextParseDigits(
        const std::string_view text,
        const int radix,
        std::uint64_t& valueOut)
    {
        if (text.empty())
        {
            return false;
        }
        constexpr std::uint64_t kMaxValue = ~0ULL;
        const std::uint64_t radixValue = static_cast<std::uint64_t>(radix);
        std::uint64_t accumulated = 0;
        for (const char character : text)
        {
            const int digit = NumericTextDigitValue(character, radix);
            if (digit < 0)
            {
                return false;
            }
            // 先判溢出再乘加。回绕之后的值落地就再也看不出问题了：它是个合法
            // 地址，界面会照样显示、驱动会照样去读。
            if (accumulated > kMaxValue / radixValue)
            {
                return false;
            }
            accumulated *= radixValue;
            if (accumulated > kMaxValue - static_cast<std::uint64_t>(digit))
            {
                return false;
            }
            accumulated += static_cast<std::uint64_t>(digit);
        }
        valueOut = accumulated;
        return true;
    }

    // ParseNumericText：按上面的规则解析一段数值文本。
    inline NumericTextParseResult ParseNumericText(
        const std::string_view rawText,
        const NumericTextDefaultRadix defaultRadix)
    {
        NumericTextParseResult result;
        const std::string_view text = NumericTextTrim(rawText);
        if (text.empty())
        {
            return result;
        }

        // 显式前缀优先，且优先于默认进制：写了 0x 就是十六进制，没有例外。
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            std::uint64_t value = 0;
            if (!NumericTextParseDigits(text.substr(2), 16, value))
            {
                return result;
            }
            result.ok = true;
            result.value = value;
            result.radix = NumericTextRadix::Hexadecimal;
            result.hadHexPrefix = true;
            return result;
        }

        if (defaultRadix == NumericTextDefaultRadix::Hexadecimal)
        {
            std::uint64_t value = 0;
            if (!NumericTextParseDigits(text, 16, value))
            {
                return result;
            }
            result.ok = true;
            result.value = value;
            result.radix = NumericTextRadix::Hexadecimal;
            return result;
        }

        std::uint64_t value = 0;
        if (NumericTextParseDigits(text, 10, value))
        {
            result.ok = true;
            result.value = value;
            result.radix = NumericTextRadix::Decimal;
            return result;
        }
        // 十进制不成立才轮到十六进制，保留"无前缀十六进制"的输入习惯。
        if (NumericTextParseDigits(text, 16, value))
        {
            result.ok = true;
            result.value = value;
            result.radix = NumericTextRadix::Hexadecimal;
            return result;
        }
        return result;
    }
}
