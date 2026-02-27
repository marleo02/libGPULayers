/*
 * SPDX-License-Identifier: MIT
 * ----------------------------------------------------------------------------
 * Copyright (c) 2025 Arm Limited
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
#include <vulkan/vk_layer.h>

#include "framework/manual_functions.hpp"
#include "layer_instance_functions.hpp"
#include "utils/misc.hpp"

/**
 * @brief Shared globals.
 */
extern std::mutex g_vulkanLock;

/**
 * Enable VK_EXT_debug_utils if not enabled.
 *
 * Enabling this requires passing the extension string to vkCreateInstance().
 *
 * @param createInfo   The createInfo we can search to find user config.
 * @param supported    The list of supported extension, or empty if unknown.
 */
static void enableInstanceVkExtDebugUtils(vku::safe_VkInstanceCreateInfo& createInfo,
                                          const std::vector<std::string>& supported)
{
    static const std::string target {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
    };

    // Test if the desired extension is supported. If supported list is
    // empty then we didn't query and assume extension is supported.
    if (supported.size() && !isIn(target, supported))
    {
        LAYER_ERR("Instance extension not available: %s", target.c_str());
        return;
    }

    // Enable the extension - this will skip adding if already enabled
    if (vku::AddExtension(createInfo, target.c_str()))
    {
        LAYER_LOG("Instance extension added: %s", target.c_str());
    }
    else
    {
        LAYER_LOG("Instance extension already enabled: %s", target.c_str());
    }
}

/* See Vulkan API for documentation. */
template <>
VKAPI_ATTR VkResult VKAPI_CALL layer_vkCreateInstance<user_tag>(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    LAYER_TRACE(__func__);

    // We cannot reliably query the available instance extensions on Android if multiple layers are
    // installed, so we disable this by default. This occurs because Android only implements the v0
    // specification between the loader and the interceptor, and does not implement chainable
    // intercepts for vkEnumerateInstanceExtensionProperties().
    //
    // On Android if you call chainInfo getInstanceProcAddr() to get the function in the next layer
    // you will get the layer implementation of the function, and layer implementations of this
    // function will return the additional extensions that the layer itself provides. It does not
    // forward to the driver, and any query for anything other than the layer will just return
    // VK_ERROR_LAYER_NOT_PRESENT.
    //
    // If you are running with a single layer you can set this to true, and use proper queries.
    constexpr bool queryExtensions = false;

    std::vector<std::string> supportedExtensions;
    if (queryExtensions)
    {
        supportedExtensions = getInstanceExtensionList(pCreateInfo);
    }

    LayerConfig config;
    if (config.has_fatal_errors())
    {
        LAYER_ERR("Invalid layer config; aborting vkCreateInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto* chainInfo = getChainInfo(pCreateInfo);
    auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    // Work out what version we should use, promoting to meet layer requirement
    APIVersion appVersion = getApplicationAPIVersion(pCreateInfo);
    APIVersion maxVersion = getInstanceAPIVersion(fpGetInstanceProcAddr);
    APIVersion reqVersion = Instance::minAPIVersion;
    APIVersion newVersion = increaseAPIVersion(appVersion, maxVersion, reqVersion);

    auto fpCreateInstanceRaw = fpGetInstanceProcAddr(nullptr, "vkCreateInstance");
    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpCreateInstanceRaw);
    if (!fpCreateInstance)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Create modifiable structures we can patch
    vku::safe_VkInstanceCreateInfo safeCreateInfo(pCreateInfo);
    auto* newCreateInfo = reinterpret_cast<VkInstanceCreateInfo*>(&safeCreateInfo);

    // Patch updated application info
    safeCreateInfo.pApplicationInfo->apiVersion = VK_MAKE_API_VERSION(0, newVersion.first, newVersion.second, 0);

    // Enable extra extensions
    for (const auto& newExt : Instance::requiredDriverExtensions)
    {
        if (newExt == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
        {
            enableInstanceVkExtDebugUtils(safeCreateInfo, supportedExtensions);
        }
        else
        {
            LAYER_ERR("Unknown instance extension: %s", newExt.c_str());
        }
    }

    // Apply instance extension overrides
    const auto& overrides = config.extension_instance_overrides();
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
            if (supportedExtensions.size() && !isIn(extension, supportedExtensions))
            {
                LAYER_ERR("Instance extension not available: %s", extension.c_str());
                continue;
            }

            if (vku::AddExtension(safeCreateInfo, extension.c_str()))
            {
                LAYER_LOG("Instance extension added: %s", extension.c_str());
            }
            else
            {
                LAYER_LOG("Instance extension already enabled: %s", extension.c_str());
            }
        }
        else if (action == "remove")
        {
            if (vku::RemoveExtension(safeCreateInfo, extension.c_str()))
            {
                LAYER_LOG("Instance extension removed: %s", extension.c_str());
            }
            else
            {
                LAYER_LOG("Instance extension not enabled: %s", extension.c_str());
            }
        }
    }

    // Log extensions for debug purposes
    for (uint32_t i = 0; i < newCreateInfo->enabledExtensionCount; i++)
    {
        LAYER_LOG("Requested instance extension list: [%u] = %s", i, newCreateInfo->ppEnabledExtensionNames[i]);
    }

    // Get the new chain info so we modify our safe copy, not the original
    chainInfo = getChainInfo(newCreateInfo);

    // Advance the link info for the next element on the chain
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
    auto result = fpCreateInstance(newCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
    {
        return result;
    }

    // Retake the lock to access layer-wide global store
    auto instance = std::make_unique<Instance>(*pInstance, fpGetInstanceProcAddr);

    {
        std::lock_guard<std::mutex> lock {g_vulkanLock};
        Instance::store(*pInstance, instance);
    }

    return VK_SUCCESS;
}
