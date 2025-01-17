// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_set>

#include <ie_metric_helpers.hpp>
#include <threading/ie_executor_manager.hpp>
#include <ie_algorithm.hpp>
#include <ngraph/opsets/opset1.hpp>
#include <transformations/utils/utils.hpp>
#include <ie_icore.hpp>

#include "auto_plugin.hpp"
#include "ngraph_ops/convolution_ie.hpp"
#include "ngraph_ops/deconvolution_ie.hpp"

namespace AutoPlugin {
namespace {
    std::string GetNetworkPrecision(const InferenceEngine::CNNNetwork &network) {
        auto nGraphFunc = network.getFunction();
        bool isINTModel = ngraph::op::util::has_op_with_type<ngraph::op::FakeQuantize>(nGraphFunc);
        if (isINTModel) {
            return METRIC_VALUE(INT8);
        }
        for (auto & node : nGraphFunc->get_ordered_ops()) {
            if (std::dynamic_pointer_cast<ngraph::opset1::Convolution>(node) ||
                std::dynamic_pointer_cast<ngraph::opset1::GroupConvolution>(node) ||
                std::dynamic_pointer_cast<ngraph::opset1::GroupConvolutionBackpropData>(node) ||
                std::dynamic_pointer_cast<ngraph::opset1::ConvolutionBackpropData>(node) ||
                std::dynamic_pointer_cast<ngraph::op::ConvolutionIE>(node) ||
                std::dynamic_pointer_cast<ngraph::op::DeconvolutionIE>(node)) {
                auto layerType = node->input(1).get_element_type().get_type_name();
                if (layerType == "f32")
                    return METRIC_VALUE(FP32);
                if (layerType == "f16")
                    return METRIC_VALUE(FP16);
            }
        }
        return METRIC_VALUE(FP32);
    }
}  // namespace

AutoInferencePlugin::AutoInferencePlugin() {
    _pluginName = "AUTO";
}

IE::IExecutableNetworkInternal::Ptr AutoInferencePlugin::LoadNetwork(const std::string& fileName,
                                                                     const ConfigType&  config) {
    return LoadNetworkImpl(fileName, config);
}

IE::IExecutableNetworkInternal::Ptr AutoInferencePlugin::LoadExeNetworkImpl(const IE::CNNNetwork& network,
                                                                            const ConfigType&     config) {
    if (network.getFunction() == nullptr) {
        IE_THROW() << "AUTO device supports just ngraph network representation";
    }

    auto networkPrecision = GetNetworkPrecision(network);
    return LoadNetworkImpl(network, config, networkPrecision);
}

IE::QueryNetworkResult AutoInferencePlugin::QueryNetwork(const IE::CNNNetwork& network, const ConfigType& config) const {
    IE::QueryNetworkResult queryResult = {};
    if (GetCore() == nullptr) {
        IE_THROW() << "Please, work with AUTO device via InferencEngine::Core object";
    }

    if (network.getFunction() == nullptr) {
        IE_THROW() << "AUTO device supports just ngraph network representation";
    }

    auto fullConfig = mergeConfigs(_config, config);
    auto metaDevices = GetDeviceChoice(fullConfig);
    std::unordered_set<std::string> supportedLayers;
    for (auto&& value : metaDevices) {
        try {
            auto deviceQr = GetCore()->QueryNetwork(network, value.deviceName, value.config);
            std::unordered_set<std::string> deviceSupportedLayers;
            for (auto &&layerQr : deviceQr.supportedLayersMap) {
                deviceSupportedLayers.emplace(layerQr.first);
            }
            supportedLayers = supportedLayers.empty()
                            ? deviceSupportedLayers : (deviceSupportedLayers.empty()
                            ? supportedLayers : IE::details::Intersection(
                                 supportedLayers, deviceSupportedLayers));
            break;
        } catch (...) {
        }
    }

    for (auto&& supportedLayer : supportedLayers) {
        queryResult.supportedLayersMap[supportedLayer] = GetName();
    }
    return queryResult;
}

IE::Parameter AutoInferencePlugin::GetConfig(const std::string& name,
                                             const std::map<std::string, IE::Parameter> & options) const {
    auto it = _config.find(name);
    if (it == _config.end()) {
        IE_THROW() << "Unsupported config key: " << name;
    } else {
        return { it->second };
    }
}

void AutoInferencePlugin::SetConfig(const ConfigType& config) {
    for (auto && kvp : config) {
        _config[kvp.first] = kvp.second;
    }
}

IE::Parameter AutoInferencePlugin::GetMetric(const std::string& name,
                                             const std::map<std::string, IE::Parameter> & options) const {
    if (name == METRIC_KEY(SUPPORTED_METRICS)) {
        std::vector<std::string> metrics;
        metrics.emplace_back(METRIC_KEY(SUPPORTED_METRICS));
        metrics.emplace_back(METRIC_KEY(FULL_DEVICE_NAME));
        metrics.emplace_back(METRIC_KEY(SUPPORTED_CONFIG_KEYS));
        metrics.emplace_back(METRIC_KEY(OPTIMIZATION_CAPABILITIES));
        IE_SET_METRIC_RETURN(SUPPORTED_METRICS, metrics);
    } else if (name == METRIC_KEY(FULL_DEVICE_NAME)) {
        std::string device_name = {"Inference Engine AUTO device"};
        IE_SET_METRIC_RETURN(FULL_DEVICE_NAME, device_name);
    } else if (name == METRIC_KEY(SUPPORTED_CONFIG_KEYS)) {
        std::vector<std::string> configKeys;
        IE_SET_METRIC_RETURN(SUPPORTED_CONFIG_KEYS, configKeys);
    } else if (name == METRIC_KEY(OPTIMIZATION_CAPABILITIES)) {
        std::vector<std::string> capabilities = GetOptimizationCapabilities();
        IE_SET_METRIC_RETURN(OPTIMIZATION_CAPABILITIES, capabilities);
    } else {
        IE_THROW() << "Unsupported metric key " << name;
    }
}

//////////////////////////////////// private & protected functions ///////////////////
std::vector<AutoPlugin::DeviceInformation> AutoInferencePlugin::GetDeviceChoice(const ConfigType&  config) const {
    std::vector<DeviceInformation> metaDevices;
    std::vector<std::string> availableDevices;

    auto deviceListConfig = config.find(IE::KEY_AUTO_DEVICE_LIST);
    if (deviceListConfig == config.end()) {
        availableDevices = GetCore()->GetAvailableDevices();
    } else {
        availableDevices = IE::DeviceIDParser::getHeteroDevices(deviceListConfig->second);
    }

    auto getDeviceConfig = [&] (const DeviceName & deviceWithID) {
        IE::DeviceIDParser deviceParser(deviceWithID);
        std::string deviceName = deviceParser.getDeviceName();
        ConfigType tconfig = mergeConfigs(_config, config);

        // set device ID if any
        std::string deviceIDLocal = deviceParser.getDeviceID();
        if (!deviceIDLocal.empty()) {
            tconfig[IE::PluginConfigParams::KEY_DEVICE_ID] = deviceIDLocal;
        }

        return GetSupportedConfig(tconfig, deviceName);
    };

    for (auto && d : availableDevices) {
        if (d != _pluginName) {
            metaDevices.push_back({ d, getDeviceConfig(d)});
        }
    }

    if (metaDevices.empty()) {
        IE_THROW() << "Please, check environment due to no supported devices can be used";
    }

    return metaDevices;
}

std::vector<std::string> AutoInferencePlugin::GetOptimizationCapabilities() const {
    // FIXME: workaround to get devicelist.
    std::unordered_set<std::string> capabilities;
    std::vector<std::string> queryDeviceLists{"CPU", "GPU"};
    for (auto &item : queryDeviceLists) {
        try {
            std::vector<std::string> device_cap =
                GetCore()->GetMetric(item, METRIC_KEY(OPTIMIZATION_CAPABILITIES));
            for (auto &cap : device_cap) {
                // For CPU test SetBlobOfKindTest::CompareWithRefs which checks BATCHED_BLOB capability,
                // and AUTO select CPU but not GPU (GPU has this capability).
                if (cap == METRIC_VALUE(BATCHED_BLOB)) {
                    continue;
                }
                capabilities.insert(cap);
            }
        } catch (...) {
        }
    }
    return {capabilities.begin(), capabilities.end()};
}

ConfigType AutoInferencePlugin::GetSupportedConfig(const ConfigType&  config,
                                                   const std::string& deviceName) const {
    std::vector<std::string> supportedConfigKeys = GetCore()->GetMetric(deviceName, METRIC_KEY(SUPPORTED_CONFIG_KEYS));
    ConfigType supportedConfig;
    for (auto&& key : supportedConfigKeys) {
        auto itKey = config.find(key);
        if (config.end() != itKey) {
            supportedConfig[key] = itKey->second;
        }
    }
    return supportedConfig;
}

DeviceInformation AutoInferencePlugin::SelectDevice(const std::vector<DeviceInformation>& metaDevices, const std::string& networkPrecision) {
    if (metaDevices.empty()) {
        IE_THROW(NotFound) << "No available device to select in AUTO plugin";
    }
    if (metaDevices.size() == 1) {
        return metaDevices.at(0);
    }

    std::vector<DeviceInformation> CPU;
    std::vector<DeviceInformation> GPU;

    for (auto& item : metaDevices) {
        if (item.deviceName.find("CPU") == 0) {
            CPU.push_back(item);
            continue;
        }
        if (item.deviceName.find("GPU") == 0) {
            GPU.push_back(item);
            continue;
        }
    }

    if (CPU.empty() && GPU.empty()) {
        IE_THROW(NotFound) << "No available device found";
    }

    // Sort GPU by name: GPU.2 > GPU.1 > GPU.0 > GPU, so we always choose the GPU[0] as best device
    std::sort(GPU.begin(), GPU.end(), [](const DeviceInformation& a, const DeviceInformation& b)->bool{return b.deviceName < a.deviceName;});

    for (auto&& item : GPU) {
        std::vector<std::string> capability = GetCore()->GetMetric(item.deviceName, METRIC_KEY(OPTIMIZATION_CAPABILITIES));
        auto res = std::find(capability.begin(), capability.end(), networkPrecision);
        if (res != capability.end()) {
            return item;
        }
    }

    if (CPU.empty()) {
        IE_THROW() << "Cannot select any device";
    }
    return CPU[0];
}

ConfigType AutoInferencePlugin::mergeConfigs(ConfigType config, const ConfigType& local) {
    for (auto && kvp : local) {
        config[kvp.first] = kvp.second;
    }
    return config;
}

// define CreatePluginEngine to create plugin instance
static const IE::Version version = {{2, 1}, CI_BUILD_NUMBER, "AutoPlugin"};
IE_DEFINE_PLUGIN_CREATE_FUNCTION(AutoInferencePlugin, version)
}  // namespace AutoPlugin
