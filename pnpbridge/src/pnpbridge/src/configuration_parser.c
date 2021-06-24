// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "pnpbridge_common.h"
#include "azure_c_shared_utility/hmacsha256.h"
#include "azure_c_shared_utility/azure_base64.h"

char* getcwd(char* buf, size_t size);

PCONNECTION_PARAMETERS PnpBridgeConfig_GetConnectionDetails(JSON_Object* ConnectionParams)
{
    PCONNECTION_PARAMETERS connParams = NULL;
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    {
        connParams = (PCONNECTION_PARAMETERS)calloc(1, sizeof(CONNECTION_PARAMETERS));
        if (NULL == connParams) {
            LogError("Failed to allocate CONNECTION_PARAMETERS");
            result = IOTHUB_CLIENT_ERROR;
            goto exit;
        }

        // Evaluate the connection_type
        {
            const char* connectionTypeStr = json_object_get_string(ConnectionParams, PNP_CONFIG_CONNECTION_TYPE);
            if (NULL == connectionTypeStr) {
                LogError("%s is not specified in config", PNP_CONFIG_CONNECTION_TYPE);
                result = IOTHUB_CLIENT_INVALID_ARG;
                goto exit;
            }

            if (0 == strcmp(connectionTypeStr, PNP_CONFIG_CONNECTION_TYPE_STRING)) {
                connParams->ConnectionType = CONNECTION_TYPE_CONNECTION_STRING;
            }
            else if (0 == strcmp(connectionTypeStr, PNP_CONFIG_CONNECTION_TYPE_DPS)) {
                connParams->ConnectionType = CONNECTION_TYPE_DPS;
            }
            else if (0 == strcmp(connectionTypeStr, PNP_CONFIG_CONNECTION_TYPE_EDGE_MODULE)) {
                connParams->ConnectionType = CONNECTION_TYPE_EDGE_MODULE;
            }
            else {
                LogError("ConnectionType (%s) is not valid", connectionTypeStr);
                result = IOTHUB_CLIENT_INVALID_ARG;
                goto exit;
            }

            LogInfo("Connection_type is [%s]", connectionTypeStr);
        }

        // Get device capability model URI
        connParams->RootInterfaceModelId = json_object_get_string(ConnectionParams, PNP_CONFIG_CONNECTION_ROOT_INTERFACE_MODEL_ID);
        if (NULL == connParams->RootInterfaceModelId) {
            LogError("%s is missing in config", PNP_CONFIG_CONNECTION_ROOT_INTERFACE_MODEL_ID);
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        // Get connection string parameters
        {
            if (CONNECTION_TYPE_CONNECTION_STRING == connParams->ConnectionType) {
                connParams->u1.ConnectionString = json_object_get_string(ConnectionParams, PNP_CONFIG_CONNECTION_TYPE_CONFIG_STRING);
                if (NULL == connParams->u1.ConnectionString) {
                    LogError("%s is missing", PNP_CONFIG_CONNECTION_TYPE_CONFIG_STRING);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }
            }
        }

        // Get dps parameters
        {
            if (CONNECTION_TYPE_DPS == connParams->ConnectionType) {
                JSON_Object* dpsSettings = json_object_get_object(ConnectionParams, PNP_CONFIG_CONNECTION_TYPE_CONFIG_DPS);
                if (NULL == dpsSettings) {
                    LogError("%s are missing in config", PNP_CONFIG_CONNECTION_TYPE_CONFIG_DPS);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }

                DPS_PARAMETERS* dpsParams = &connParams->u1.Dps;

                dpsParams->GlobalProvUri = json_object_get_string(dpsSettings, PNP_CONFIG_CONNECTION_DPS_GLOBAL_PROV_URI);
                if (NULL == dpsParams->GlobalProvUri) {
                    LogError("%s is missing in config", PNP_CONFIG_CONNECTION_DPS_GLOBAL_PROV_URI);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }

                dpsParams->IdScope = json_object_get_string(dpsSettings, PNP_CONFIG_CONNECTION_DPS_ID_SCOPE);
                if (NULL == dpsParams->IdScope) {
                    LogError("%s is missing in config", PNP_CONFIG_CONNECTION_DPS_ID_SCOPE);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }

                dpsParams->DeviceId = json_object_get_string(dpsSettings, PNP_CONFIG_CONNECTION_DPS_DEVICE_ID);
                if (NULL == dpsParams->DeviceId) {
                    LogError("%s is missing in config", PNP_CONFIG_CONNECTION_DPS_DEVICE_ID);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }

                dpsParams->RootInterfaceModelId = connParams->RootInterfaceModelId;
            }
        }

        // Evaluate the auth_type
        {
            // Get auth_parameters object
            JSON_Object* authParameters = json_object_get_object(ConnectionParams, PNP_CONFIG_CONNECTION_AUTH_PARAMETERS);
            if (NULL == authParameters) {
                LogError("%s is missing in config", PNP_CONFIG_CONNECTION_AUTH_PARAMETERS);
                result = IOTHUB_CLIENT_INVALID_ARG;
                goto exit;
            }

            const char* authType = json_object_get_string(authParameters, PNP_CONFIG_CONNECTION_AUTH_TYPE);
            if (NULL == authType) {
                LogError("%s is not specified in config", PNP_CONFIG_CONNECTION_AUTH_TYPE);
                result = IOTHUB_CLIENT_INVALID_ARG;
                goto exit;
            }

            if (0 == strcmp(authType, PNP_CONFIG_CONNECTION_AUTH_TYPE_SYMM)) {
                connParams->AuthParameters.AuthType = AUTH_TYPE_SYMMETRIC_KEY;
            }
            else if (0 == strcmp(authType, PNP_CONFIG_CONNECTION_AUTH_TYPE_X509)) {
                connParams->AuthParameters.AuthType = AUTH_TYPE_X509;
            }
            else {
                LogError("auth_type (%s) is not valid", authType);
                result = IOTHUB_CLIENT_INVALID_ARG;
                goto exit;
            }

            // Evaluate auth parameters
            {
                if (AUTH_TYPE_SYMMETRIC_KEY == connParams->AuthParameters.AuthType) {

                    const char *deviceKey = json_object_get_string(authParameters, PNP_CONFIG_CONNECTION_AUTH_TYPE_DEVICE_SYMM_KEY);

                    if (connParams->ConnectionType == CONNECTION_TYPE_DPS)
                    {
                        // check if Group Symmetric Key is specified
                        const char *groupKey = json_object_get_string(authParameters, PNP_CONFIG_CONNECTION_AUTH_TYPE_GROUP_SYMM_KEY);

                        if (NULL != groupKey)
                        {
                            BUFFER_HANDLE decode_key;
                            BUFFER_HANDLE hash;

                            // LogInfo("Group key Found %s", groupKey);

                            if ((decode_key = Azure_Base64_Decode(groupKey)) == NULL)
                            {
                                LogError("Failed to decode Group Key\r\n");
                            }
                            else if ((hash = BUFFER_new()) == NULL)
                            {
                                LogError("Failed to allocate buffer for hash");
                                BUFFER_delete(decode_key);
                            }
                            else
                            {
                                if ((HMACSHA256_ComputeHash(BUFFER_u_char(decode_key), 
                                                            BUFFER_length(decode_key),
                                                            (const unsigned char *)connParams->u1.Dps.DeviceId,
                                                            strlen(connParams->u1.Dps.DeviceId), hash)) != HMACSHA256_OK)
                                {
                                    LogError("Failed to compute hash for device id");
                                }
                                else
                                {
                                    STRING_HANDLE device_key = NULL;
                                    device_key = Azure_Base64_Encode(hash);
                                    deviceKey = STRING_c_str(device_key);
                                    // LogInfo("Device Key Generated : %s", deviceKey);
                                }
                                BUFFER_delete(hash);
                                BUFFER_delete(decode_key);
                            }
                        }
                    }

                    if (deviceKey == NULL)
                    {
                        deviceKey = json_object_get_string(authParameters, PNP_CONFIG_CONNECTION_AUTH_TYPE_DEVICE_SYMM_KEY);
                    }

                    if (NULL == deviceKey) {
                        LogError("%s is missing in config", PNP_CONFIG_CONNECTION_AUTH_TYPE_DEVICE_SYMM_KEY);
                        result = IOTHUB_CLIENT_INVALID_ARG;
                        goto exit;
                    }

                    connParams->AuthParameters.u1.DeviceKey = deviceKey;
                }
            }
        }
    }
exit:
    {
        if (IOTHUB_CLIENT_OK != result) {
            free(connParams);
            connParams = NULL;
        }
    }

    return connParams;
}

IOTHUB_CLIENT_RESULT PnpBridgeConfig_GetJsonValueFromConfigFile(const char* filename, JSON_Value** config)
{
    if (NULL == filename) {
        LogError("filename is NULL");
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    if (NULL == config) {
        LogError("config is NULL");
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    // Check if file exists
    {
        FILE* file;
        file = fopen(filename, "r");
        if (NULL != file) {
            fclose(file);
        }
        else {
            char* workingDir;
            workingDir = getcwd(NULL, 0);
            LogError("Cannot find the config file %s. Current working directory %s", filename, workingDir);
            if (NULL != workingDir) {
                free(workingDir);
            }
            return IOTHUB_CLIENT_ERROR;
        }
    }

    // Serialize the json file using parson
    *config = json_parse_file(filename);
    if (*config == NULL) {
        LogError("Failed to parse the config file. Please validate the JSON file in a JSON vailidator");
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT PnpBridgeConfig_RetrieveConfiguration(JSON_Value* JsonConfig, PNPBRIDGE_CONFIGURATION* BridgeConfig)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    PCONNECTION_PARAMETERS connParams = NULL;

    // Check for mandatory parameters
    {
        if (NULL == JsonConfig) {
            LogError("JsonConfig is NULL");
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        if (NULL == BridgeConfig) {
            LogError("BridgeConfig is NULL");
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        // TODO: Validate json schema
        // json_validate(JsonConfig, SCHEMA);

        JSON_Object* jsonObject = json_value_get_object(JsonConfig);
        JSON_Object* pnpBridgeConnectionParameters = json_object_get_object(jsonObject, PNP_CONFIG_CONNECTION_PARAMETERS);
        if (NULL == pnpBridgeConnectionParameters) {
            LogError("%s is missing", PNP_CONFIG_CONNECTION_PARAMETERS);
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        connParams = PnpBridgeConfig_GetConnectionDetails(pnpBridgeConnectionParameters);
        if (NULL == connParams) {
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        // Read the trace flag option
        int traceOnBool = json_object_get_boolean(jsonObject, PNP_CONFIG_TRACE_ON);
        BridgeConfig->TraceOn = (1 == traceOnBool);
        LogInfo("Tracing is %s", BridgeConfig->TraceOn ? "enabled" : "disabled");

        // Check for interface instance list
        JSON_Array* devices = Configuration_GetDevices(JsonConfig);
        if (NULL == devices) {
            LogError("No configured devices in the pnpbridge config");
            result = IOTHUB_CLIENT_INVALID_ARG;
            goto exit;
        }

        for (size_t i = 0; i < json_array_get_count(devices); i++) {
            JSON_Object* device = json_array_get_object(devices, i);

            // Every interface instance should specify a component name and associated pnp adapter identity
            {
                const char* componentName = json_object_dotget_string(device, PNP_CONFIG_COMPONENT_NAME);
                if (NULL == componentName) {
                    LogError("Device at index %zu is missing %s", i, PNP_CONFIG_COMPONENT_NAME);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }

                const char* adapterId = json_object_dotget_string(device, PNP_CONFIG_ADAPTER_ID);
                if (NULL == adapterId) {
                    LogError("Device at index %zu is missing %s", i, PNP_CONFIG_ADAPTER_ID);
                    result = IOTHUB_CLIENT_INVALID_ARG;
                    goto exit;
                }
            }
        }

        // Assign output values
        BridgeConfig->JsonConfig = JsonConfig;
        BridgeConfig->ConnParams = connParams;

    }
exit:
    {
        if (!PNPBRIDGE_SUCCESS(result)) {
            if (connParams) {
                free(connParams);
            }
        }
    }

    return result;
}

IOTHUB_CLIENT_RESULT PnpBridgeConfig_GetJsonValueFromString(const char* configString, JSON_Value** config)
{
    if (NULL == configString || NULL == config) {
        LogError("PnpBridgeConfig_GetJsonValueFromString: Invalid parameters");
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    *config = json_parse_string(configString);
    if (*config == NULL) {
        LogError("PnpBridgeConfig_GetJsonValueFromString: Failed to parse config string");
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    return IOTHUB_CLIENT_OK;
}

JSON_Array* Configuration_GetDevices(JSON_Value* config) {
    JSON_Object* jsonObject = json_value_get_object(config);
    JSON_Array* devices = json_object_dotget_array(jsonObject, PNP_CONFIG_DEVICES);

    return devices;
}

JSON_Object* Configuration_GetPnpParametersForDevice(JSON_Object* device) {

    if (device == NULL) {
        return NULL;
    }

    JSON_Object* pnpParams = json_object_dotget_object(device, PNP_CONFIG_PNP_PARAMETERS);
    return pnpParams;
}

JSON_Object* Configuration_GetMatchParametersForDevice(JSON_Object* device)
{
    if (device == NULL) {
        return NULL;
    }

    JSON_Object* filterParams = json_object_dotget_object(device, PNP_CONFIG_MATCH_FILTERS);
    JSON_Object* matchParams = json_object_dotget_object(filterParams, PNP_CONFIG_MATCH_PARAMETERS);
    return matchParams;
}

JSON_Object* Configuration_GetAdapterParameters(JSON_Value* config, const char* identity, const char* adapterType) {
    JSON_Object* jsonObject = json_value_get_object(config);
    JSON_Object* adapter = json_object_dotget_object(jsonObject, adapterType);
    if (NULL == adapter) {
        return NULL;
    }

    JSON_Array* params = json_object_dotget_array(adapter, PNP_CONFIG_PARAMETERS);
    if (NULL == params) {
        return NULL;
    }

    for (size_t j = 0; j < json_array_get_count(params); j++) {
        JSON_Object* param = json_array_get_object(params, j);
        const char* id = json_object_dotget_string(param, PNP_CONFIG_IDENTITY);
        if (NULL != id) {
            if (strcmp(id, identity) == 0) {
                return param;
            }
        }
    }

    return NULL;
}

JSON_Object* Configuration_GetGlobalAdapterParameters(JSON_Value* config, const char* identity) {
    JSON_Object* globalAdapterParams = NULL;
    JSON_Object* jsonObject = json_value_get_object(config);
    JSON_Object* globalParams = json_object_get_object(jsonObject, PNP_CONFIG_ADAPTER_GLOBAL);
    if (globalParams != NULL)
    {
        for (size_t i = 0; i < json_object_get_count(globalParams); i++) {
            const char* adapterType = json_object_get_name(globalParams, i);
            if (strcmp(adapterType, identity) == 0)
            {
                globalAdapterParams = json_object_dotget_object(globalParams, adapterType);
                break;
            }
        }
    }

    return globalAdapterParams;
}