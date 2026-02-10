/*
 * SPDX-License-Identifier: MIT
 * ----------------------------------------------------------------------------
 * Copyright (c) 2024-2025 Arm Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * ----------------------------------------------------------------------------
 */

#include <vulkan/utility/vk_struct_helper.hpp>

#include "device.hpp"
#include "framework/manual_functions.hpp"
#include "framework/utils.hpp"
#include "instance.hpp"
#include "generated_feature_overrides.hpp"
#include "utils/misc.hpp"

/**
 * @brief The dispatch lookup for all of the created Vulkan instances.
 */
static std::unordered_map<void*, std::unique_ptr<Device>> g_devices;

/* Predeclare custom DeviceCreatePatch functions */
static void modifyDeviceRobustBufferAccess(Instance& instance,
                                           VkPhysicalDevice physicalDevice,
                                           vku::safe_VkDeviceCreateInfo& createInfo,
                                           std::vector<std::string>& supported);
static void overrideDeviceFeatures(Instance& instance,
                                   VkPhysicalDevice physicalDevice,
                                   vku::safe_VkDeviceCreateInfo& createInfo,
                                   std::vector<std::string>& supported);
static void overrideDeviceExtensions(Instance& instance,
                                     VkPhysicalDevice physicalDevice,
                                     vku::safe_VkDeviceCreateInfo& createInfo,
                                     std::vector<std::string>& supported);

/* See header for documentation. */
const std::vector<DeviceCreatePatchPtr> Device::createInfoPatches {
    enableDeviceVkKhrTimelineSemaphore,
    enableDeviceVkExtImageCompressionControl,
    modifyDeviceRobustBufferAccess,
    overrideDeviceFeatures,
    overrideDeviceExtensions
};

/* See header for documentation. */
void Device::store(VkDevice handle, std::unique_ptr<Device> device)
{
    void* key = getDispatchKey(handle);
    g_devices.insert({key, std::move(device)});
}

/* See header for documentation. */
Device* Device::retrieve(VkDevice handle)
{
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
Device* Device::retrieve(VkQueue handle)
{
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
Device* Device::retrieve(VkCommandBuffer handle)
{
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
std::unique_ptr<Device> Device::destroy(
    VkDevice handle
) {
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));

    auto device = std::move(g_devices.at(key));
    g_devices.erase(key);
    return device;
}

/* See header for documentation. */
Device::Device(Instance* _instance,
               VkPhysicalDevice _physicalDevice,
               VkDevice _device,
               PFN_vkGetDeviceProcAddr nlayerGetProcAddress,
               const VkDeviceCreateInfo& createInfo)
    : instance(_instance),
      physicalDevice(_physicalDevice),
      device(_device)
{
    UNUSED(createInfo);

    initDriverDeviceDispatchTable(device, nlayerGetProcAddress, driver);

    VkSemaphoreTypeCreateInfo timelineCreateInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = queueSerializationTimelineSemCount,
    };

    VkSemaphoreCreateInfo semCreateInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineCreateInfo,
        .flags = 0,
    };

    auto result = driver.vkCreateSemaphore(device, &semCreateInfo, nullptr, &queueSerializationTimelineSem);
    if (result != VK_SUCCESS)
    {
        LAYER_ERR("Failed vkCreateSemaphore() for queue serialization");
        queueSerializationTimelineSem = nullptr;
    }
}

/**
 * Allow a force enable/disable of robustBufferAccess feature.
 *
 * @param instance         The layer instance we are running within.
 * @param physicalDevice   The physical device we are creating a device for.
 * @param createInfo       The createInfo we can search to find user config.
 * @param supported        The list of supported extensions.
 */
static void modifyDeviceRobustBufferAccess(Instance& instance,
                                           VkPhysicalDevice physicalDevice,
                                           vku::safe_VkDeviceCreateInfo& createInfo,
                                           std::vector<std::string>& supported)
{
    UNUSED(supported);

    // Only one of these can be set as an override
    // If neither are set then don't change the app settings
    bool enableRobustness = instance.config.feature_enable_robustBufferAccess();
    bool disableRobustness = instance.config.feature_disable_robustBufferAccess();

    // No patch to apply
    if (!enableRobustness && !disableRobustness)
    {
        return;
    }

    // Query if robustness is supported if we need to force enable
    VkPhysicalDeviceFeatures supportedFeatures;
    instance.driver.vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
    if (enableRobustness && !supportedFeatures.robustBufferAccess)
    {
        LAYER_LOG("Device feature not available: robustBufferAccess");
        return;
    }

    // We know we can const_cast here because createInfo is a safe-struct clone
    // Option1 = legacy-style enable
    auto* config = const_cast<VkPhysicalDeviceFeatures*>(createInfo.pEnabledFeatures);

    // Option2 = modern-style enable
    void* pNextBase = const_cast<void*>(createInfo.pNext);
    auto* configNext = vku::FindStructInPNextChain<VkPhysicalDeviceFeatures2>(pNextBase);

    // Pick the feature struct from either of the valid options
    if (!config && configNext)
    {
        config = &configNext->features;
    }

    // User provided feature enable struct, so just change that directly
    if (config)
    {
        if (enableRobustness)
        {
            if (config->robustBufferAccess)
            {
                LAYER_LOG("Device feature already enabled: robustBufferAccess");
            }
            else
            {
                LAYER_LOG("Device feature enabled: robustBufferAccess");
                config->robustBufferAccess = VK_TRUE;
            }
        }

        if (disableRobustness)
        {
            if (!config->robustBufferAccess)
            {
                LAYER_LOG("Device feature already disabled: robustBufferAccess");
            }

            LAYER_LOG("Device feature disabled: robustBufferAccess");
            config->robustBufferAccess = VK_FALSE;
        }
    }
    // User provided no feature enables, so provide our own structure
    else if (enableRobustness)
    {
        LAYER_LOG("Device feature enabled: robustBufferAccess");

        // Create a dynamic copy and transfer ownership to the safe-struct
        auto* newFeatures = new VkPhysicalDeviceFeatures;
        memset(newFeatures, 0, sizeof(VkPhysicalDeviceFeatures));
        createInfo.pEnabledFeatures = newFeatures;
    }
    // User provided no feature enables, but we don't need one
    else if (disableRobustness)
    {
        LAYER_LOG("Device feature already disabled: robustBufferAccess");
    }
}

/**
 * Apply feature overrides to the device feature list.
 *
 * @param instance         The layer instance we are running within.
 * @param physicalDevice   The physical device we are creating a device for.
 * @param createInfo       The createInfo we can search to find user config.
 * @param supported        The list of supported extensions.
 */
static void overrideDeviceFeatures(Instance& instance,
                                   VkPhysicalDevice physicalDevice,
                                   vku::safe_VkDeviceCreateInfo& createInfo,
                                   std::vector<std::string>& supported)
{
    UNUSED(supported);

    const auto& overrides = instance.config.feature_overrides();
    if (overrides.empty())
    {
        return;
    }

    struct PendingOverride
    {
        const FeatureOverrideEntry* entry;
        const std::string* action;
    };

    std::vector<PendingOverride> pending;
    pending.reserve(overrides.size());

    std::vector<FeatureStructId> extensionSupportStructs;
    for (const auto& entry : overrides)
    {
        if (entry.second == "do_not_override")
        {
            continue;
        }

        const FeatureOverrideEntry* featureEntry = nullptr;
        for (size_t i = 0; i < featureOverrideEntryCount; i++)
        {
            if (entry.first == featureOverrideEntries[i].name)
            {
                featureEntry = &featureOverrideEntries[i];
                break;
            }
        }

        if (!featureEntry)
        {
            if (entry.first.find('.') == std::string::npos)
            {
                LAYER_ERR("Invalid feature override key (expected StructName.field): %s",
                          entry.first.c_str());
            }
            else
            {
                LAYER_ERR("Unknown feature override: %s", entry.first.c_str());
            }
            continue;
        }

        pending.push_back({featureEntry, &entry.second});
        if ((entry.second == "insert") &&
            (featureEntry->struct_id != FeatureStructId::Id_VkPhysicalDeviceFeatures) &&
            !isIn(featureEntry->struct_id, extensionSupportStructs))
        {
            extensionSupportStructs.push_back(featureEntry->struct_id);
        }
    }

    if (pending.empty())
    {
        return;
    }

    VkPhysicalDeviceFeatures supportedFeatures {};
    bool needCoreSupportQuery = false;
    for (const auto& entry : pending)
    {
        if ((*entry.action == "insert") &&
            (entry.entry->struct_id == FeatureStructId::Id_VkPhysicalDeviceFeatures))
        {
            needCoreSupportQuery = true;
            break;
        }
    }

    if (needCoreSupportQuery)
    {
        instance.driver.vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
    }

    VkPhysicalDeviceFeatures2 supportedFeatures2 = vku::InitStructHelper();
    std::vector<std::pair<FeatureStructId, VkBaseOutStructure*>> supportedStructs;
    if (!extensionSupportStructs.empty())
    {
        supportedStructs.reserve(extensionSupportStructs.size());
        for (const auto structId : extensionSupportStructs)
        {
            auto* data = allocateFeatureStruct(structId);
            if (!data)
            {
                continue;
            }

            data->pNext = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(supportedFeatures2.pNext));
            supportedFeatures2.pNext = data;
            supportedStructs.emplace_back(structId, data);
        }

        instance.driver.vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures2);
    }

    VkPhysicalDeviceFeatures* config = const_cast<VkPhysicalDeviceFeatures*>(createInfo.pEnabledFeatures);
    void* pNextBase = const_cast<void*>(createInfo.pNext);
    auto* configNext = vku::FindStructInPNextChain<VkPhysicalDeviceFeatures2>(pNextBase);
    if (!config && configNext)
    {
        config = &configNext->features;
    }

    auto* supportedCoreBase = reinterpret_cast<const uint8_t*>(&supportedFeatures);
    auto* configCoreBase = reinterpret_cast<uint8_t*>(config);

    for (const auto& entry : pending)
    {
        const std::string& action = *entry.action;
        const FeatureOverrideEntry* featureEntry = entry.entry;
        const char* featureName = featureEntry->name;

        if (action == "insert")
        {
            if (featureEntry->struct_id == FeatureStructId::Id_VkPhysicalDeviceFeatures)
            {
                auto* supportedField = reinterpret_cast<const VkBool32*>(supportedCoreBase + featureEntry->offset);
                if (!(*supportedField))
                {
                    LAYER_LOG("Device feature not available: %s", featureName);
                    continue;
                }

                if (!config)
                {
                    auto* newFeatures = new VkPhysicalDeviceFeatures;
                    memset(newFeatures, 0, sizeof(VkPhysicalDeviceFeatures));
                    createInfo.pEnabledFeatures = newFeatures;
                    config = newFeatures;
                    configCoreBase = reinterpret_cast<uint8_t*>(config);
                }

                auto* configField = reinterpret_cast<VkBool32*>(configCoreBase + featureEntry->offset);
                if (*configField)
                {
                    LAYER_LOG("Device feature already enabled: %s", featureName);
                }
                else
                {
                    LAYER_LOG("Device feature enabled: %s", featureName);
                    *configField = VK_TRUE;
                }
            }
            else
            {
                const FeatureStructInfo* info = nullptr;
                for (size_t i = 0; i < featureStructInfoCount; i++)
                {
                    if (featureStructInfos[i].struct_id == featureEntry->struct_id)
                    {
                        info = &featureStructInfos[i];
                        break;
                    }
                }

                if (!info)
                {
                    LAYER_ERR("Unknown feature override: %s", featureName);
                    continue;
                }

                VkBaseOutStructure* supportedStruct = reinterpret_cast<VkBaseOutStructure*>(supportedFeatures2.pNext);
                while (supportedStruct && supportedStruct->sType != info->sType)
                {
                    supportedStruct = supportedStruct->pNext;
                }

                if (!supportedStruct)
                {
                    LAYER_LOG("Device feature not available: %s", featureName);
                    continue;
                }

                auto* supportedField = reinterpret_cast<const VkBool32*>(
                    reinterpret_cast<const uint8_t*>(supportedStruct) + featureEntry->offset);
                if (!(*supportedField))
                {
                    LAYER_LOG("Device feature not available: %s", featureName);
                    continue;
                }

                const VkBaseOutStructure* existingStructConst =
                    reinterpret_cast<const VkBaseOutStructure*>(createInfo.pNext);
                while (existingStructConst && existingStructConst->sType != info->sType)
                {
                    existingStructConst = existingStructConst->pNext;
                }

                if (existingStructConst)
                {
                    auto* existingStruct = const_cast<VkBaseOutStructure*>(existingStructConst);
                    auto* configField = reinterpret_cast<VkBool32*>(
                        reinterpret_cast<uint8_t*>(existingStruct) + featureEntry->offset);
                    if (*configField)
                    {
                        LAYER_LOG("Device feature already enabled: %s", featureName);
                    }
                    else
                    {
                        LAYER_LOG("Device feature enabled: %s", featureName);
                        *configField = VK_TRUE;
                    }
                }
                else
                {
                    if (addFeatureStructToPnext(createInfo, featureEntry->struct_id, featureEntry->offset, VK_TRUE))
                    {
                        LAYER_LOG("Device feature enabled: %s", featureName);
                    }
                    else
                    {
                        LAYER_LOG("Device feature already enabled: %s", featureName);
                    }
                }
            }
        }
        else if (action == "remove")
        {
            if (featureEntry->struct_id == FeatureStructId::Id_VkPhysicalDeviceFeatures)
            {
                if (!config)
                {
                    LAYER_LOG("Device feature already disabled: %s", featureName);
                    continue;
                }

                auto* configField = reinterpret_cast<VkBool32*>(configCoreBase + featureEntry->offset);
                if (!(*configField))
                {
                    LAYER_LOG("Device feature already disabled: %s", featureName);
                }

                LAYER_LOG("Device feature disabled: %s", featureName);
                *configField = VK_FALSE;
            }
            else
            {
                const FeatureStructInfo* info = nullptr;
                for (size_t i = 0; i < featureStructInfoCount; i++)
                {
                    if (featureStructInfos[i].struct_id == featureEntry->struct_id)
                    {
                        info = &featureStructInfos[i];
                        break;
                    }
                }

                if (!info)
                {
                    LAYER_ERR("Unknown feature override: %s", featureName);
                    continue;
                }

                const VkBaseOutStructure* existingStructConst =
                    reinterpret_cast<const VkBaseOutStructure*>(createInfo.pNext);
                while (existingStructConst && existingStructConst->sType != info->sType)
                {
                    existingStructConst = existingStructConst->pNext;
                }

                if (!existingStructConst)
                {
                    LAYER_LOG("Device feature already disabled: %s", featureName);
                    continue;
                }

                auto* existingStruct = const_cast<VkBaseOutStructure*>(existingStructConst);
                auto* configField = reinterpret_cast<VkBool32*>(
                    reinterpret_cast<uint8_t*>(existingStruct) + featureEntry->offset);
                if (!(*configField))
                {
                    LAYER_LOG("Device feature already disabled: %s", featureName);
                }

                LAYER_LOG("Device feature disabled: %s", featureName);
                *configField = VK_FALSE;
            }
        }
    }

    for (const auto& entry : supportedStructs)
    {
        freeFeatureStruct(entry.first, entry.second);
    }
}

/**
 * Apply extension overrides to the device extension list.
 *
 * @param instance         The layer instance we are running within.
 * @param physicalDevice   The physical device we are creating a device for.
 * @param createInfo       The createInfo we can search to find user config.
 * @param supported        The list of supported extensions.
 */
static void overrideDeviceExtensions(Instance& instance,
                                     VkPhysicalDevice physicalDevice,
                                     vku::safe_VkDeviceCreateInfo& createInfo,
                                     std::vector<std::string>& supported)
{
    UNUSED(physicalDevice);

    const auto& overrides = instance.config.extension_device_overrides();
    if (overrides.empty())
    {
        return;
    }

    for (const auto& entry : overrides)
    {
        const std::string& extension = entry.first;
        const std::string& action = entry.second;

        if (action == "do_not_override")
        {
            continue;
        }
        else if (action == "insert")
        {
            if (supported.size() && !isIn(extension, supported))
            {
                LAYER_LOG("Device extension not available: %s", extension.c_str());
                continue;
            }

            if (vku::AddExtension(createInfo, extension.c_str()))
            {
                LAYER_LOG("Device extension added: %s", extension.c_str());
            }
            else
            {
                LAYER_LOG("Device extension already enabled: %s", extension.c_str());
            }
        }
        else if (action == "remove")
        {
            if (vku::RemoveExtension(createInfo, extension.c_str()))
            {
                LAYER_LOG("Device extension removed: %s", extension.c_str());
            }
            else
            {
                LAYER_LOG("Device extension not enabled: %s", extension.c_str());
            }
        }
    }
}
