"""从 v4 pack JSON 里取一个 profile，输出 ksword_dyndata_v4_blob 用的纯文本清单。

用途：靶机上只有驱动和 KswordCLI 时，把 PDB profile 下进驱动。pack 的 JSON 解析
目前只在 GUI 里（Qt JSON），所以没有 GUI 的机器上 `_EPROCESS.VadRoot` 这类
只来自 PDB profile 的字段会一直是 Unavailable —— 看着像"这个 build 没有偏移表"，
其实是"偏移表在包里、只是没人 apply"。

这里**只做 JSON 到文本**，一个字节的二进制布局都不碰：打包由
ksword_dyndata_v4_blob.cpp 用产品头文件里的结构体完成。手抄一份布局出来，
迟早和头文件走散，而且走散时不报错。

用法：
  python ksword_dyndata_pack_to_manifest.py --pack <pack.json> --pdb-guid <32hex>
         [--pdb-age N] [--image-base 0x...] [--output manifest.txt]
"""

import argparse
import io
import json
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", required=True)
    parser.add_argument("--pdb-guid", required=True,
                        help="32 位十六进制，无连字符；见 PE 的 RSDS 记录")
    parser.add_argument("--pdb-age", type=int, default=None)
    parser.add_argument("--image-base", default="0",
                        help="目标机器上该模块的实际加载基址；0 表示不声明")
    parser.add_argument("--flags", type=int, default=0)
    parser.add_argument("--output", default="-")
    args = parser.parse_args()

    wanted = args.pdb_guid.upper().replace("-", "")
    with io.open(args.pack, encoding="utf-8") as handle:
        pack = json.load(handle)

    hit = None
    for profile in pack.get("profiles", []):
        guid = str(profile.get("pdbGuid", "")).upper().replace("-", "")
        if guid != wanted:
            continue
        if args.pdb_age is not None and int(profile.get("pdbAge", -1)) != args.pdb_age:
            continue
        hit = profile
        break

    if hit is None:
        # 明确报"包里没有"，不退回相近 build —— 那正是本功能一直拒绝做的事。
        sys.stderr.write("no profile in pack for pdbGuid=%s age=%s\n"
                         % (wanted, args.pdb_age))
        return 3

    image_base = int(args.image_base, 0)
    lines = []
    lines.append("# generated from %s" % args.pack)
    lines.append("profile %s" % hit.get("profileName", ""))
    lines.append("pdbName %s" % hit.get("pdbName", ""))
    lines.append("pdbGuid %s" % wanted)
    lines.append("pdbAge %d" % int(hit.get("pdbAge", 0)))
    lines.append("moduleName %s" % ("ntoskrnl.exe" if int(hit.get("moduleClassId", 0)) == 0
                                    else hit.get("pdbName", "")))
    lines.append("machine %d" % int(hit.get("machine", 0)))
    lines.append("timeDateStamp %d" % int(hit.get("timeDateStamp", 0)))
    lines.append("sizeOfImage %d" % int(hit.get("sizeOfImage", 0)))
    lines.append("imageBase %d" % image_base)
    lines.append("classId %d" % int(hit.get("moduleClassId", 0)))
    lines.append("flags %d" % args.flags)

    for group in hit.get("capabilityGroups", []):
        lines.append("group %d %d %d %d %s" % (
            int(group.get("groupId", 0)), int(group.get("flags", 0)),
            int(group.get("requiredItemCount", 0)), int(group.get("optionalItemCount", 0)),
            group.get("groupName", "")))

    for item in hit.get("items", []):
        lines.append("item %d %d %d %d %d %d %d %d %d %d" % (
            int(item.get("itemId", 0)), int(item.get("itemKind", 0)),
            int(item.get("flags", 0)), int(item.get("capabilityGroupId", 0)),
            int(item.get("valueLow", 0)), int(item.get("valueHigh", 0)),
            int(item.get("aux0", 0)), int(item.get("aux1", 0)),
            int(item.get("aux2", 0)), int(item.get("aux3", 0))))

    # legacy(v1) 字段。`fields` 是 [字典下标, 偏移] 对，而**驱动认的是字段 id**，
    # 两者不是一回事：字典下标来自 fieldDictionary 的排列，字段 id 是协议里写死的
    # KSW_DYN_FIELD_ID_*。所以要拿名字去 v4 items 里查回 itemId。
    # 查不到 id 的条目宁可丢掉也不按下标当 id 发 —— 那会把偏移写到别的字段上。
    id_by_name = {}
    for item in hit.get("items", []):
        name = item.get("name")
        if name:
            id_by_name[name] = int(item.get("itemId", 0))

    dictionary = pack.get("fieldDictionary", [])
    emitted = 0
    unmapped = []
    for entry in hit.get("fields", []):
        if not isinstance(entry, list) or len(entry) < 2:
            continue
        index, offset = int(entry[0]), int(entry[1])
        if index < 0 or index >= len(dictionary):
            continue
        name = dictionary[index]
        field_id = id_by_name.get(name)
        if field_id is None:
            unmapped.append(name)
            continue
        lines.append("field %d %d" % (field_id, offset))
        emitted += 1

    if unmapped:
        sys.stderr.write("note: %d legacy fields have no v4 itemId and were skipped: %s\n"
                         % (len(unmapped), ", ".join(sorted(unmapped)[:8])))

    text = "\n".join(lines) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        with io.open(args.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        sys.stderr.write("wrote %s: %d v4 items, %d groups, %d legacy fields\n"
                         % (args.output, len(hit.get("items", [])),
                            len(hit.get("capabilityGroups", [])), emitted))
    return 0


if __name__ == "__main__":
    sys.exit(main())
