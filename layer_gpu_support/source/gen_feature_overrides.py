#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# ----------------------------------------------------------------------------
# Copyright (c) 2025 Arm Limited
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
# ----------------------------------------------------------------------------

import sys
import xml.etree.ElementTree as ET
from typing import Optional


class FeatureStruct:
    def __init__(self, name: str, s_type: Optional[str], fields: list[str], guard: Optional[str]) -> None:
        self.name = name
        self.s_type = s_type
        self.fields = fields
        self.guard = guard


def is_feature_struct(name: str) -> bool:
    if name == "VkPhysicalDeviceFeatures":
        return True
    if not name.startswith("VkPhysicalDevice"):
        return False
    return "Features" in name


def parse_guard(struct: ET.Element) -> Optional[str]:
    guards: list[str] = []
    protect = struct.get("protect")
    requires = struct.get("requires")

    for value in (protect, requires):
        if not value:
            continue
        for token in value.split(","):
            token = token.strip()
            if token == "VK_ENABLE_BETA_EXTENSIONS" or token.startswith("VK_USE_PLATFORM_"):
                guards.append(token)

    if not guards:
        return None

    return " && ".join([f"defined({g})" for g in guards])


def load_feature_structs(vk_xml_path: str) -> list[FeatureStruct]:
    tree = ET.parse(vk_xml_path)
    root = tree.getroot()

    structs = []
    for struct in root.findall("./types/type[@category='struct']"):
        name = struct.get("name")
        if name is None or not is_feature_struct(name):
            continue

        fields = []
        s_type = None
        for member in struct.findall("member"):
            type_node = member.find("type")
            name_node = member.find("name")
            enum_node = member.find("enum")
            if type_node is None or name_node is None:
                continue
            if name_node.text == "sType" and enum_node is not None:
                s_type = enum_node.text
            if type_node.text == "VkBool32":
                fields.append(name_node.text)

        if fields:
            guard = parse_guard(struct)
            if name != "VkPhysicalDeviceFeatures" and s_type is None:
                continue
            structs.append(FeatureStruct(name, s_type, fields, guard))

    if not structs:
        raise RuntimeError("No feature structs found in vk.xml")

    return structs


def write_header(out_path: str, structs: list[FeatureStruct]) -> None:
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("/*\n")
        out.write(" * SPDX-License-Identifier: MIT\n")
        out.write(" * ----------------------------------------------------------------------------\n")
        out.write(" * Copyright (c) 2025 Arm Limited\n")
        out.write(" *\n")
        out.write(" * Permission is hereby granted, free of charge, to any person obtaining a copy\n")
        out.write(" * of this software and associated documentation files (the \"Software\"), to\n")
        out.write(" * deal in the Software without restriction, including without limitation the\n")
        out.write(" * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or\n")
        out.write(" * sell copies of the Software, and to permit persons to whom the Software is\n")
        out.write(" * furnished to do so, subject to the following conditions:\n")
        out.write(" *\n")
        out.write(" * The above copyright notice and this permission notice shall be included in\n")
        out.write(" * all copies or substantial portions of the Software.\n")
        out.write(" *\n")
        out.write(" * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n")
        out.write(" * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n")
        out.write(" * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n")
        out.write(" * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n")
        out.write(" * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING\n")
        out.write(" * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS\n")
        out.write(" * IN THE SOFTWARE.\n")
        out.write(" * ----------------------------------------------------------------------------\n")
        out.write(" */\n\n")
        out.write("#pragma once\n\n")
        out.write("#include <cstddef>\n")
        out.write("#include <cstdint>\n")
        out.write("#include <cstring>\n")
        out.write("#include <vulkan/vulkan.h>\n")
        out.write("#include <vulkan/utility/vk_safe_struct_utils.hpp>\n\n")
        out.write("enum class FeatureStructId\n")
        out.write("{\n")
        for struct in structs:
            if struct.guard:
                out.write(f"#if {struct.guard}\n")
            out.write(f"    Id_{struct.name},\n")
            if struct.guard:
                out.write("#endif\n")
        out.write("};\n\n")
        out.write("struct FeatureOverrideEntry\n")
        out.write("{\n")
        out.write("    const char* name;\n")
        out.write("    FeatureStructId struct_id;\n")
        out.write("    size_t offset;\n")
        out.write("};\n\n")
        out.write("struct FeatureStructInfo\n")
        out.write("{\n")
        out.write("    FeatureStructId struct_id;\n")
        out.write("    VkStructureType sType;\n")
        out.write("    size_t size;\n")
        out.write("    const char* name;\n")
        out.write("};\n\n")
        out.write("static const FeatureStructInfo featureStructInfos[] = {\n")
        for struct in structs:
            if struct.guard:
                out.write(f"#if {struct.guard}\n")
            s_type = struct.s_type if struct.s_type else "VK_STRUCTURE_TYPE_MAX_ENUM"
            out.write(f"    {{FeatureStructId::Id_{struct.name}, {s_type}, sizeof({struct.name}), \"{struct.name}\"}},\n")
            if struct.guard:
                out.write("#endif\n")
        out.write("};\n\n")
        out.write("static constexpr size_t featureStructInfoCount =\n")
        out.write("    sizeof(featureStructInfos) / sizeof(featureStructInfos[0]);\n\n")
        out.write("static const FeatureOverrideEntry featureOverrideEntries[] = {\n")
        for struct in structs:
            for field in struct.fields:
                key = f"{struct.name}.{field}"
                if struct.guard:
                    out.write(f"#if {struct.guard}\n")
                out.write(
                    f"    {{\"{key}\", FeatureStructId::Id_{struct.name}, offsetof({struct.name}, {field})}},\n")
                if struct.guard:
                    out.write("#endif\n")
        out.write("};\n\n")
        out.write("static constexpr size_t featureOverrideEntryCount =\n")
        out.write("    sizeof(featureOverrideEntries) / sizeof(featureOverrideEntries[0]);\n\n")
        out.write("static inline VkBaseOutStructure* allocateFeatureStruct(FeatureStructId id)\n")
        out.write("{\n")
        out.write("    switch (id)\n")
        out.write("    {\n")
        for struct in structs:
            if struct.name == "VkPhysicalDeviceFeatures":
                continue
            if struct.guard:
                out.write(f"#if {struct.guard}\n")
            out.write(f"        case FeatureStructId::Id_{struct.name}:\n")
            out.write("        {\n")
            out.write(f"            auto* data = new {struct.name};\n")
            out.write(f"            std::memset(data, 0, sizeof({struct.name}));\n")
            if struct.s_type:
                out.write(f"            data->sType = {struct.s_type};\n")
            out.write("            data->pNext = nullptr;\n")
            out.write("            return reinterpret_cast<VkBaseOutStructure*>(data);\n")
            out.write("        }\n")
            if struct.guard:
                out.write("#endif\n")
        out.write("        default:\n")
        out.write("            return nullptr;\n")
        out.write("    }\n")
        out.write("}\n\n")
        out.write("static inline void freeFeatureStruct(FeatureStructId id, VkBaseOutStructure* ptr)\n")
        out.write("{\n")
        out.write("    if (!ptr)\n")
        out.write("    {\n")
        out.write("        return;\n")
        out.write("    }\n")
        out.write("\n")
        out.write("    switch (id)\n")
        out.write("    {\n")
        for struct in structs:
            if struct.name == "VkPhysicalDeviceFeatures":
                continue
            if struct.guard:
                out.write(f"#if {struct.guard}\n")
            out.write(f"        case FeatureStructId::Id_{struct.name}:\n")
            out.write(f"            delete reinterpret_cast<{struct.name}*>(ptr);\n")
            out.write("            return;\n")
            if struct.guard:
                out.write("#endif\n")
        out.write("        default:\n")
        out.write("            return;\n")
        out.write("    }\n")
        out.write("}\n\n")
        out.write("static inline bool addFeatureStructToPnext(vku::safe_VkDeviceCreateInfo& createInfo,\n")
        out.write("                                           FeatureStructId id,\n")
        out.write("                                           size_t offset,\n")
        out.write("                                           VkBool32 value)\n")
        out.write("{\n")
        out.write("    (void)createInfo;\n")
        out.write("    (void)offset;\n")
        out.write("    (void)value;\n")
        out.write("    switch (id)\n")
        out.write("    {\n")
        for struct in structs:
            if struct.name == "VkPhysicalDeviceFeatures":
                continue
            if struct.guard:
                out.write(f"#if {struct.guard}\n")
            out.write(f"        case FeatureStructId::Id_{struct.name}:\n")
            out.write("        {\n")
            out.write(f"            {struct.name} data;\n")
            out.write(f"            std::memset(&data, 0, sizeof({struct.name}));\n")
            if struct.s_type:
                out.write(f"            data.sType = {struct.s_type};\n")
            out.write("            auto* field = reinterpret_cast<VkBool32*>(\n")
            out.write("                reinterpret_cast<uint8_t*>(&data) + offset);\n")
            out.write("            *field = value;\n")
            out.write("            return vku::AddToPnext(createInfo, data);\n")
            out.write("        }\n")
            if struct.guard:
                out.write("#endif\n")
        out.write("        default:\n")
        out.write("            return false;\n")
        out.write("    }\n")
        out.write("}\n")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gen_feature_overrides.py <vk.xml> <output.hpp>", file=sys.stderr)
        return 1

    vk_xml_path = sys.argv[1]
    out_path = sys.argv[2]

    structs = load_feature_structs(vk_xml_path)
    write_header(out_path, structs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
