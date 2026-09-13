// 离线内核结构偏移提取器（开发机工具，不随产品分发）。
//
// 为什么需要它：注入检查的 VAD 视图、映像节对象比较与 VAD 断链判定都要按目标 build
// 验证过的结构偏移，而 `ksword_pdb_profile_generator.py` 的后端是 llvm-pdbutil，
// 这台机器上没有。DbgHelp 在，而且仓库里 ArkRuntimeDynData.cpp 已经用它读 PDB 类型
// 读了很久 —— 这里复用同一套 TI_FINDCHILDREN 配方，只是跑在离线侧。
//
// **只读本地符号库，绝不联网**：符号路径写死成给定目录，不加 `srv*` 前缀，
// 所以 DbgHelp 只会在本地符号库的标准布局里找 <pdb>\<GUID><Age>\<pdb>。
// 产品侧"不得在目标电脑下载 PDB"的策略不受影响 —— 这个工具不进产品。
//
// 用法：
//   ksword_kernel_struct_offsets.exe <pe-path> <symbol-store> [<type>!<member> ...]
//   不给 type!member 时输出内置的注入检查所需字段清单。
//
// 输出是 JSON，便于喂给后续步骤；解析不到的字段明确列在 "missing" 里，
// **不猜、不取相近 build 的值**。

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "DbgHelp.lib")

namespace
{
    // 注入检查这条线要用到的全部内核结构字段。
    // VAD 树遍历只要 EPROCESS.VadRoot；断链判定要 MMVAD_SHORT 的树指针；
    // 映像节对象比较要从 VAD 走到 ControlArea/Segment 的那条链。
    const char* const kWantedFields[] = {
        "_EPROCESS!VadRoot",
        "_EPROCESS!VadHint",
        "_EPROCESS!VadCount",
        "_EPROCESS!UniqueProcessId",
        "_MMVAD_SHORT!VadNode",
        "_MMVAD_SHORT!StartingVpn",
        "_MMVAD_SHORT!EndingVpn",
        "_MMVAD_SHORT!StartingVpnHigh",
        "_MMVAD_SHORT!EndingVpnHigh",
        "_MMVAD_SHORT!u",
        "_MMVAD_SHORT!VadFlags",
        "_MMVAD!Subsection",
        "_MMVAD!FirstPrototypePte",
        "_MMVAD!LastContiguousPte",
        "_RTL_BALANCED_NODE!Left",
        "_RTL_BALANCED_NODE!Right",
        "_RTL_BALANCED_NODE!ParentValue",
        "_RTL_AVL_TREE!Root",
        "_SUBSECTION!ControlArea",
        "_SUBSECTION!SubsectionBase",
        "_SUBSECTION!PtesInSubsection",
        "_SUBSECTION!NextSubsection",
        "_SUBSECTION!StartingSector",
        "_CONTROL_AREA!Segment",
        "_CONTROL_AREA!FilePointer",
        "_CONTROL_AREA!NumberOfSectionReferences",
        "_SEGMENT!ControlArea",
        "_SEGMENT!TotalNumberOfPtes",
        "_SEGMENT!SegmentFlags",
        "_SEGMENT!PrototypePte",
    };

    struct Session final
    {
        HANDLE key = nullptr;
        DWORD64 base = 0U;
        ~Session()
        {
            if (key != nullptr)
            {
                ::SymCleanup(key);
            }
        }
    };

    std::string JsonEscape(const std::string& text)
    {
        std::string out;
        for (const char character : text)
        {
            switch (character)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out.push_back(character); break;
            }
        }
        return out;
    }

    std::string Narrow(const wchar_t* const wide)
    {
        if (wide == nullptr)
        {
            return std::string();
        }
        const int needed =
            ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1)
        {
            return std::string();
        }
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    struct MemberInfo final
    {
        DWORD offset = 0U;
        DWORD bitPosition = 0U;
        ULONG64 bitLength = 0U;
        bool isBitfield = false;
    };

    // 一个类型的全部成员：名字 -> 偏移。
    bool LoadTypeMembers(const Session& session, const std::string& typeName,
                         std::map<std::string, MemberInfo>& membersOut,
                         ULONG64& sizeOut)
    {
        std::vector<std::uint8_t> storage(sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char), 0U);
        auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (::SymGetTypeFromName(session.key, session.base, typeName.c_str(), symbol) == FALSE)
        {
            return false;
        }
        sizeOut = symbol->Size;
        const ULONG typeIndex = symbol->TypeIndex;

        DWORD childCount = 0U;
        if (::SymGetTypeInfo(session.key, session.base, typeIndex, TI_GET_CHILDRENCOUNT,
                             &childCount) == FALSE)
        {
            return false;
        }
        if (childCount == 0U)
        {
            return true;
        }

        const std::size_t bytes = sizeof(TI_FINDCHILDREN_PARAMS) +
                                  (static_cast<std::size_t>(childCount) - 1U) * sizeof(ULONG);
        std::vector<std::uint8_t> childStorage(bytes, 0U);
        auto* const children = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(childStorage.data());
        children->Count = childCount;
        children->Start = 0U;
        if (::SymGetTypeInfo(session.key, session.base, typeIndex, TI_FINDCHILDREN,
                             children) == FALSE)
        {
            return false;
        }

        for (ULONG index = 0U; index < childCount; ++index)
        {
            const ULONG childId = children->ChildId[index];
            wchar_t* rawName = nullptr;
            if (::SymGetTypeInfo(session.key, session.base, childId, TI_GET_SYMNAME,
                                 &rawName) == FALSE ||
                rawName == nullptr)
            {
                continue;
            }
            const std::unique_ptr<wchar_t, decltype(&::LocalFree)> name(rawName, &::LocalFree);

            DWORD offset = 0U;
            if (::SymGetTypeInfo(session.key, session.base, childId, TI_GET_OFFSET,
                                 &offset) == FALSE)
            {
                continue;
            }
            MemberInfo info;
            info.offset = offset;
            DWORD bitPosition = 0U;
            if (::SymGetTypeInfo(session.key, session.base, childId, TI_GET_BITPOSITION,
                                 &bitPosition) != FALSE)
            {
                ULONG64 bitLength = 0U;
                if (::SymGetTypeInfo(session.key, session.base, childId, TI_GET_LENGTH,
                                     &bitLength) != FALSE)
                {
                    info.isBitfield = true;
                    info.bitPosition = bitPosition;
                    info.bitLength = bitLength;
                }
            }
            membersOut.emplace(Narrow(name.get()), info);
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s <pe-path> <symbol-store> [<type>!<member> ...]\n", argv[0]);
        return 2;
    }
    const std::string pePath = argv[1];
    const std::string symbolStore = argv[2];

    std::vector<std::string> wanted;
    for (int index = 3; index < argc; ++index)
    {
        wanted.emplace_back(argv[index]);
    }
    if (wanted.empty())
    {
        for (const char* const field : kWantedFields)
        {
            wanted.emplace_back(field);
        }
    }

    // SYMOPT_EXACT_SYMBOLS：GUID/Age 对不上就失败，绝不接受相邻版本的 PDB。
    // 不设 SYMOPT_DEBUG，不用 srv* 前缀 —— 搜索路径只有给定的本地库。
    ::SymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS |
                    SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_INCLUDE_32BIT_MODULES);

    Session session;
    session.key = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x4B53574FU));
    if (::SymInitialize(session.key, symbolStore.c_str(), FALSE) == FALSE)
    {
        std::printf("{\"error\":\"SymInitialize failed\",\"win32\":%lu}\n", ::GetLastError());
        session.key = nullptr;
        return 3;
    }

    session.base = ::SymLoadModuleEx(session.key, nullptr, pePath.c_str(), nullptr,
                                     0x10000000ULL, 0U, nullptr, 0U);
    if (session.base == 0U)
    {
        std::printf("{\"error\":\"SymLoadModuleEx failed\",\"win32\":%lu}\n", ::GetLastError());
        return 4;
    }

    IMAGEHLP_MODULEW64 moduleInfo{};
    moduleInfo.SizeOfStruct = sizeof(moduleInfo);
    if (::SymGetModuleInfoW64(session.key, session.base, &moduleInfo) == FALSE)
    {
        std::printf("{\"error\":\"SymGetModuleInfoW64 failed\",\"win32\":%lu}\n", ::GetLastError());
        return 5;
    }
    // SymNone / SymDeferred 表示符号根本没加载成功，这时任何"偏移"都是假的。
    if (moduleInfo.SymType != SymPdb)
    {
        std::printf("{\"error\":\"no PDB loaded\",\"symType\":%d}\n",
                    static_cast<int>(moduleInfo.SymType));
        return 6;
    }

    std::printf("{\n");
    std::printf("  \"pe\": \"%s\",\n", JsonEscape(pePath).c_str());
    std::printf("  \"pdb\": \"%s\",\n", JsonEscape(Narrow(moduleInfo.LoadedPdbName)).c_str());
    std::printf("  \"pdbGuid\": \"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X\",\n",
                moduleInfo.PdbSig70.Data1, moduleInfo.PdbSig70.Data2, moduleInfo.PdbSig70.Data3,
                moduleInfo.PdbSig70.Data4[0], moduleInfo.PdbSig70.Data4[1],
                moduleInfo.PdbSig70.Data4[2], moduleInfo.PdbSig70.Data4[3],
                moduleInfo.PdbSig70.Data4[4], moduleInfo.PdbSig70.Data4[5],
                moduleInfo.PdbSig70.Data4[6], moduleInfo.PdbSig70.Data4[7]);
    std::printf("  \"pdbAge\": %lu,\n", moduleInfo.PdbAge);
    std::printf("  \"timeDateStamp\": \"0x%08lX\",\n", moduleInfo.TimeDateStamp);
    std::printf("  \"sizeOfImage\": \"0x%08lX\",\n", moduleInfo.ImageSize);

    std::map<std::string, std::map<std::string, MemberInfo>> cache;
    std::map<std::string, ULONG64> typeSizes;
    std::vector<std::string> missing;

    std::printf("  \"fields\": {\n");
    bool first = true;
    for (const std::string& entry : wanted)
    {
        const std::size_t bang = entry.find('!');
        if (bang == std::string::npos)
        {
            missing.push_back(entry);
            continue;
        }
        const std::string typeName = entry.substr(0U, bang);
        const std::string memberName = entry.substr(bang + 1U);

        if (cache.find(typeName) == cache.end())
        {
            std::map<std::string, MemberInfo> members;
            ULONG64 size = 0U;
            if (!LoadTypeMembers(session, typeName, members, size))
            {
                cache.emplace(typeName, std::map<std::string, MemberInfo>{});
                typeSizes.emplace(typeName, 0U);
            }
            else
            {
                cache.emplace(typeName, std::move(members));
                typeSizes.emplace(typeName, size);
            }
        }
        const auto& members = cache[typeName];
        const auto hit = members.find(memberName);
        if (hit == members.end())
        {
            missing.push_back(entry);
            continue;
        }
        if (!first)
        {
            std::printf(",\n");
        }
        first = false;
        std::printf("    \"%s\": { \"offset\": %lu, \"offsetHex\": \"0x%lX\"",
                    JsonEscape(entry).c_str(), hit->second.offset, hit->second.offset);
        if (hit->second.isBitfield)
        {
            std::printf(", \"bitPosition\": %lu, \"bitLength\": %llu",
                        hit->second.bitPosition,
                        static_cast<unsigned long long>(hit->second.bitLength));
        }
        std::printf(" }");
    }
    std::printf("\n  },\n");

    std::printf("  \"typeSizes\": {\n");
    first = true;
    for (const auto& [typeName, size] : typeSizes)
    {
        if (!first)
        {
            std::printf(",\n");
        }
        first = false;
        std::printf("    \"%s\": %llu", JsonEscape(typeName).c_str(),
                    static_cast<unsigned long long>(size));
    }
    std::printf("\n  },\n");

    std::printf("  \"missing\": [");
    for (std::size_t index = 0U; index < missing.size(); ++index)
    {
        std::printf("%s\"%s\"", index == 0U ? "" : ", ", JsonEscape(missing[index]).c_str());
    }
    std::printf("]\n}\n");
    return 0;
}
