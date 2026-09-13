// 把一份 v4 profile 清单打成 KswordCLI `dyn apply-profile-v4 --blob` 吃的原始包。
//
// 为什么要它：pack JSON 的解析目前只在 GUI 里（用 Qt 的 JSON），CLI 用不了。
// 于是在只有驱动和 CLI 的靶机上，**没有任何办法把 PDB profile 下进驱动** ——
// 实测后果就是 `_EPROCESS.VadRoot` 一直是 Unavailable，VAD 视图看着像"这个 build
// 没有偏移表"，其实是"偏移表在包里、只是没人 apply"。
//
// 分工刻意这样切：**二进制布局只在这里出现一次**，直接用产品头文件里的结构体填，
// 不在别处按字节手抄一遍（手抄的那份迟早和头文件走散，而且走散时不报错）。
// JSON 那一半交给 Python，它只产出纯文本清单。
//
// 清单格式（每行一条，# 开头是注释）：
//   profile      <profileName>
//   pdbName      <name>
//   pdbGuid      <32 hex>
//   pdbAge       <n>
//   moduleName   <name>
//   machine      <n>
//   timeDateStamp <n>
//   sizeOfImage  <n>
//   imageBase    <n>          （0 表示不声明）
//   classId      <n>
//   flags        <n>
//   group <groupId> <flags> <requiredItemCount> <optionalItemCount> <groupName>
//   item  <itemId> <itemKind> <flags> <capabilityGroupId> <valueLow> <valueHigh> <aux0..aux3>
//   field <fieldId> <offset>        （legacy/v1 用）
//
// 两种包：`--v4`（默认）打 APPLY_DYN_PROFILE_V4，`--v1` 打 APPLY_DYN_PROFILE。
// **两个都要发**：v4 的条目进的是独立的 v4 存储（消息是 "accepted for safe storage"），
// 它不写 `State->Kernel.*`；而 injection_vad.c 读的正是后者，只有 v1 apply 会填。
// 只发 v4 的话，apply 会报 113/113 全成功，而 `_EPROCESS.VadRoot` 依然是 Unavailable。

#include <Windows.h>

#include "../../shared/driver/KswordArkDynDataIoctl.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    void CopyNarrow(char* const destination, const std::size_t capacity, const std::string& text)
    {
        std::memset(destination, 0, capacity);
        const std::size_t count = text.size() < (capacity - 1U) ? text.size() : (capacity - 1U);
        std::memcpy(destination, text.c_str(), count);
    }

    void CopyWide(wchar_t* const destination, const std::size_t capacity, const std::string& text)
    {
        std::memset(destination, 0, capacity * sizeof(wchar_t));
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, destination,
                                                  static_cast<int>(capacity));
        if (written <= 0)
        {
            destination[0] = L'\0';
        }
        destination[capacity - 1U] = L'\0';
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s <manifest> <output-blob> [--v1|--v4]\n", argv[0]);
        std::printf("layout: v4-header=%zu item=%zu group=%zu module=%zu v1-header=%zu field=%zu\n",
                    static_cast<std::size_t>(KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE),
                    sizeof(KSW_DYN_V4_ITEM_PACKET),
                    sizeof(KSW_DYN_V4_CAPABILITY_GROUP_PACKET),
                    sizeof(KSW_DYN_V4_MODULE_IDENTITY_PACKET),
                    static_cast<std::size_t>(KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE),
                    sizeof(KSW_DYN_PROFILE_FIELD_PACKET));
        return 2;
    }
    const bool legacyMode = (argc > 3) && (std::strcmp(argv[3], "--v1") == 0);

    std::ifstream manifest(argv[1]);
    if (!manifest.is_open())
    {
        std::printf("cannot open manifest: %s\n", argv[1]);
        return 3;
    }

    std::string profileName, pdbName, pdbGuid, moduleName;
    unsigned long pdbAge = 0U, machine = 0U, timeDateStamp = 0U, sizeOfImage = 0U;
    unsigned long classId = 0U, requestFlags = 0U;
    unsigned long long imageBase = 0ULL;
    std::vector<KSW_DYN_V4_CAPABILITY_GROUP_PACKET> groups;
    std::vector<KSW_DYN_V4_ITEM_PACKET> items;
    std::vector<KSW_DYN_PROFILE_FIELD_PACKET> legacyFields;

    std::string line;
    while (std::getline(manifest, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream stream(line);
        std::string key;
        stream >> key;
        if (key == "profile") { std::getline(stream >> std::ws, profileName); }
        else if (key == "pdbName") { stream >> pdbName; }
        else if (key == "pdbGuid") { stream >> pdbGuid; }
        else if (key == "pdbAge") { stream >> pdbAge; }
        else if (key == "moduleName") { stream >> moduleName; }
        else if (key == "machine") { stream >> machine; }
        else if (key == "timeDateStamp") { stream >> timeDateStamp; }
        else if (key == "sizeOfImage") { stream >> sizeOfImage; }
        else if (key == "imageBase") { stream >> imageBase; }
        else if (key == "classId") { stream >> classId; }
        else if (key == "flags") { stream >> requestFlags; }
        else if (key == "group")
        {
            KSW_DYN_V4_CAPABILITY_GROUP_PACKET group{};
            std::string name;
            stream >> group.groupId >> group.flags >> group.requiredItemCount >>
                group.optionalItemCount;
            std::getline(stream >> std::ws, name);
            CopyNarrow(group.groupName, KSW_DYN_V4_CAPABILITY_NAME_CHARS, name);
            groups.push_back(group);
        }
        else if (key == "item")
        {
            KSW_DYN_V4_ITEM_PACKET item{};
            stream >> item.itemId >> item.itemKind >> item.flags >> item.capabilityGroupId >>
                item.valueLow >> item.valueHigh >> item.aux0 >> item.aux1 >> item.aux2 >>
                item.aux3;
            items.push_back(item);
        }
        else if (key == "field")
        {
            KSW_DYN_PROFILE_FIELD_PACKET field{};
            stream >> field.fieldId >> field.offset;
            legacyFields.push_back(field);
        }
        else
        {
            std::printf("unknown manifest key: %s\n", key.c_str());
            return 4;
        }
    }

    if (legacyMode)
    {
        if (legacyFields.empty() || legacyFields.size() > KSW_DYN_PROFILE_MAX_FIELDS)
        {
            std::printf("legacy field count invalid: %zu\n", legacyFields.size());
            return 5;
        }
        const std::size_t legacyBytes = KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE +
                                        legacyFields.size() * sizeof(KSW_DYN_PROFILE_FIELD_PACKET);
        std::vector<unsigned char> legacyBuffer(legacyBytes, 0U);
        auto* const legacy =
            reinterpret_cast<KSW_APPLY_DYN_PROFILE_REQUEST*>(legacyBuffer.data());
        legacy->size = static_cast<unsigned long>(legacyBytes);
        legacy->version = KSWORD_ARK_DYNDATA_PROTOCOL_VERSION;
        legacy->flags = requestFlags;
        legacy->fieldCount = static_cast<unsigned long>(legacyFields.size());
        legacy->ntoskrnl.present = 1UL;
        legacy->ntoskrnl.classId = classId;
        legacy->ntoskrnl.machine = machine;
        legacy->ntoskrnl.timeDateStamp = timeDateStamp;
        legacy->ntoskrnl.sizeOfImage = sizeOfImage;
        legacy->ntoskrnl.imageBase = imageBase;
        CopyWide(legacy->ntoskrnl.moduleName, KSW_DYN_MODULE_NAME_CHARS, moduleName);
        CopyNarrow(legacy->profileName, KSW_DYN_PROFILE_NAME_CHARS, profileName);
        CopyNarrow(legacy->pdbName, KSW_DYN_PDB_NAME_CHARS, pdbName);
        CopyNarrow(legacy->pdbGuid, KSW_DYN_PDB_GUID_CHARS, pdbGuid);
        legacy->pdbAge = pdbAge;
        for (std::size_t index = 0U; index < legacyFields.size(); ++index)
        {
            legacy->fields[index] = legacyFields[index];
        }
        std::ofstream legacyOutput(argv[2], std::ios::binary | std::ios::trunc);
        if (!legacyOutput.is_open())
        {
            std::printf("cannot write blob: %s\n", argv[2]);
            return 6;
        }
        legacyOutput.write(reinterpret_cast<const char*>(legacyBuffer.data()),
                           static_cast<std::streamsize>(legacyBuffer.size()));
        std::printf("wrote %zu bytes (v1): fields=%zu profile='%s'\n", legacyBytes,
                    legacyFields.size(), profileName.c_str());
        return 0;
    }

    if (items.empty() || items.size() > KSW_DYN_V4_MAX_ITEMS_PER_MODULE ||
        groups.size() > KSW_DYN_V4_MAX_CAPABILITY_GROUPS_PER_MODULE)
    {
        std::printf("item/group count invalid: items=%zu groups=%zu\n", items.size(),
                    groups.size());
        return 5;
    }

    const std::size_t bytes =
        KSW_APPLY_DYN_PROFILE_V4_REQUEST_HEADER_SIZE + items.size() * sizeof(KSW_DYN_V4_ITEM_PACKET);
    std::vector<unsigned char> buffer(bytes, 0U);
    auto* const request = reinterpret_cast<KSW_APPLY_DYN_PROFILE_V4_REQUEST*>(buffer.data());
    request->size = static_cast<unsigned long>(bytes);
    request->version = KSW_DYN_V4_PROTOCOL_VERSION;
    request->flags = requestFlags;
    request->itemCount = static_cast<unsigned long>(items.size());
    request->capabilityGroupCount = static_cast<unsigned long>(groups.size());

    request->module.image.present = 1UL;
    request->module.image.classId = classId;
    request->module.image.machine = machine;
    request->module.image.timeDateStamp = timeDateStamp;
    request->module.image.sizeOfImage = sizeOfImage;
    request->module.image.imageBase = imageBase;
    CopyWide(request->module.image.moduleName, KSW_DYN_MODULE_NAME_CHARS, moduleName);
    CopyNarrow(request->module.pdb.pdbName, KSW_DYN_PDB_NAME_CHARS, pdbName);
    CopyNarrow(request->module.pdb.pdbGuid, KSW_DYN_PDB_GUID_CHARS, pdbGuid);
    request->module.pdb.pdbAge = pdbAge;
    CopyNarrow(request->module.profileName, KSW_DYN_V4_PROFILE_NAME_CHARS, profileName);

    for (std::size_t index = 0U; index < groups.size(); ++index)
    {
        request->capabilityGroups[index] = groups[index];
    }
    for (std::size_t index = 0U; index < items.size(); ++index)
    {
        request->items[index] = items[index];
    }

    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        std::printf("cannot write blob: %s\n", argv[2]);
        return 6;
    }
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
    output.close();

    std::printf("wrote %zu bytes: items=%zu groups=%zu profile='%s'\n", bytes, items.size(),
                groups.size(), profileName.c_str());
    return 0;
}
