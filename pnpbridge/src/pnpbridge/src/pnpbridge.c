// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "pnpbridge_common.h"
#include "configuration_parser.h"
#include "pnpadapter_api.h"
#include "pnpadapter_manager.h"
#include "iothub_comms.h"

#include <iothub_client.h>

// Globals Pnp bridge instance
PPNP_BRIDGE g_PnpBridge = NULL;
PNP_BRIDGE_STATE g_PnpBridgeState = PNP_BRIDGE_UNINITIALIZED;
bool g_PnpBridgeShutdown = false;


IOTHUB_CLIENT_RESULT
PnpBridge_InitializePnpModuleConfig(PNP_DEVICE_CONFIGURATION * PnpModuleConfig)
{
    PnpModuleConfig->deviceMethodCallback = (IOTHUB_CLIENT_DEVICE_METHOD_CALLBACK_ASYNC) PnpAdapterManager_DeviceMethodCallback;
    PnpModuleConfig->deviceTwinCallback = (IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK) PnpAdapterManager_DeviceTwinCallback;
    PnpModuleConfig->enableTracing = (strcmp(getenv(g_hubClientTraceEnabled), "true") == 0);
    PnpModuleConfig->modelId = getenv(g_pnpBridgeModuleRootModelId);
    // Note: User Agent String should not be changed
    PnpModuleConfig->UserAgentString = g_pnpBridgeUserAgentString;
    PnpModuleConfig->securityType = PNP_CONNECTION_SECURITY_TYPE_CONNECTION_STRING;
    PnpModuleConfig->u.connectionString = getenv(g_connectionStringEnvironmentVariable);
    return IOTHUB_CLIENT_OK;
}

IOTHUB_CLIENT_RESULT
PnpBridge_InitializePnpDeviceConfig(PNPBRIDGE_CONFIGURATION * Configuration)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    Configuration->ConnParams->PnpDeviceConfiguration.deviceMethodCallback = (IOTHUB_CLIENT_DEVICE_METHOD_CALLBACK_ASYNC) PnpAdapterManager_DeviceMethodCallback;
    Configuration->ConnParams->PnpDeviceConfiguration.deviceTwinCallback = (IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK) PnpAdapterManager_DeviceTwinCallback;
    Configuration->ConnParams->PnpDeviceConfiguration.enableTracing = Configuration->TraceOn;
    Configuration->ConnParams->PnpDeviceConfiguration.modelId = Configuration->ConnParams->RootInterfaceModelId;
    // Note: User Agent String should not be changed
    Configuration->ConnParams->PnpDeviceConfiguration.UserAgentString = g_pnpBridgeUserAgentString;

    if (Configuration->ConnParams != NULL)
    {
        if (Configuration->ConnParams->ConnectionType == CONNECTION_TYPE_CONNECTION_STRING)
        {
            Configuration->ConnParams->PnpDeviceConfiguration.securityType = PNP_CONNECTION_SECURITY_TYPE_CONNECTION_STRING;
            if (AUTH_TYPE_SYMMETRIC_KEY == Configuration->ConnParams->AuthParameters.AuthType)
            {
                char* format = NULL;

                if (NULL != strstr(Configuration->ConnParams->u1.ConnectionString, "SharedAccessKey="))
                {
                    LogInfo("WARNING: SharedAccessKey is included in connection string. Ignoring "
                        PNP_CONFIG_CONNECTION_AUTH_TYPE_DEVICE_SYMM_KEY " in config file.");
                    Configuration->ConnParams->PnpDeviceConfiguration.u.connectionString = (char*)Configuration->ConnParams->u1.ConnectionString;
                }
                else
                {
                    format = "%s;SharedAccessKey=%s";
                    Configuration->ConnParams->PnpDeviceConfiguration.u.connectionString = (char*)malloc(strlen(Configuration->ConnParams->AuthParameters.u1.DeviceKey) +
                        strlen(Configuration->ConnParams->u1.ConnectionString) + strlen(format) + 1);
                    sprintf(Configuration->ConnParams->PnpDeviceConfiguration.u.connectionString, format, Configuration->ConnParams->u1.ConnectionString,
                        Configuration->ConnParams->AuthParameters.u1.DeviceKey);
                }
            }
            else
            {
                LogError("Auth type (%d) is not supported for symmetric key", Configuration->ConnParams->AuthParameters.AuthType);
                goto exit;
            }
        }
        else if (Configuration->ConnParams->ConnectionType == CONNECTION_TYPE_DPS)
        {
            Configuration->ConnParams->PnpDeviceConfiguration.securityType = PNP_CONNECTION_SECURITY_TYPE_DPS;
            if (AUTH_TYPE_SYMMETRIC_KEY == Configuration->ConnParams->AuthParameters.AuthType)
            {
                Configuration->ConnParams->PnpDeviceConfiguration.u.dpsConnectionAuth.endpoint =  Configuration->ConnParams->u1.Dps.GlobalProvUri;
                Configuration->ConnParams->PnpDeviceConfiguration.u.dpsConnectionAuth.idScope = Configuration->ConnParams->u1.Dps.IdScope;
                Configuration->ConnParams->PnpDeviceConfiguration.u.dpsConnectionAuth.deviceId = Configuration->ConnParams->u1.Dps.DeviceId;
                Configuration->ConnParams->PnpDeviceConfiguration.u.dpsConnectionAuth.deviceKey = Configuration->ConnParams->AuthParameters.u1.DeviceKey;
            }
            else
            {
                LogError("Auth type (%d) is not supported for DPS", Configuration->ConnParams->AuthParameters.AuthType);
                goto exit;
            }
        }
        else
        {
            LogError("Connection type (%d) is not supported", Configuration->ConnParams->ConnectionType);
            goto exit;
        }
    }
exit:
    return result;
}

IOTHUB_CLIENT_RESULT
PnpBridge_InitializeEdgeModuleConfig(PNPBRIDGE_CONFIGURATION * Configuration)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    Configuration->Features.ModuleClient = 1;
    Configuration->TraceOn = (strcmp(getenv(g_hubClientTraceEnabled), "true") == 0);
    Configuration->ConnParams = (PCONNECTION_PARAMETERS)calloc(1, sizeof(CONNECTION_PARAMETERS));
    if (NULL == Configuration->ConnParams)
    {
        LogError("Failed to allocate CONNECTION_PARAMETERS for edge module.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }
    Configuration->ConnParams->ConnectionType = CONNECTION_TYPE_EDGE_MODULE;
    Configuration->ConnParams->RootInterfaceModelId = getenv(g_pnpBridgeModuleRootModelId);
    Configuration->ConnParams->u1.ConnectionString = getenv(g_connectionStringEnvironmentVariable);
    result = PnpBridge_InitializePnpModuleConfig(&Configuration->ConnParams->PnpDeviceConfiguration);
    if (IOTHUB_CLIENT_OK != result)
    {
        LogError("Failed to initialize PnP module configuration for edge module.");
        goto exit;
    }

exit:
    return result;
}

IOTHUB_CLIENT_RESULT
PnpBridge_InitializeDeviceConfig(PNPBRIDGE_CONFIGURATION * Configuration, const char * ConfigFilePath)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    JSON_Value* config = NULL;

    // Get the Json value from configuration file
    result = PnpBridgeConfig_GetJsonValueFromConfigFile(ConfigFilePath, &config);
    if (IOTHUB_CLIENT_OK != result)
    {
        LogError("Failed to retrieve the bridge configuration from specified file location.");
        goto exit;
    }

    // Check if config file has required parameters, and if so retrieve them
    // This call also internally allocated memory for and initializes Json config
    // and Connection Parameters, if all the validation succeeds
    result = PnpBridgeConfig_RetrieveConfiguration(config, Configuration);
    if (IOTHUB_CLIENT_OK != result)
    {
        LogError("Config file is invalid");
        goto exit;
    }

    result = PnpBridge_InitializePnpDeviceConfig(Configuration);
    if (IOTHUB_CLIENT_OK != result)
    {
        LogError("Pnp device configuration failed to initialize.");
        goto exit;
    }

exit:
    return result;
}

bool
PnpBridge_IsRunningInEdgeRuntime()
{
    bool dockerizedModule = false;
    const char * iotEdgeWorkLoadUri = getenv(g_workloadURIEnvironmentVariable);
    dockerizedModule = (iotEdgeWorkLoadUri != NULL);
    LogInfo("%s", ((dockerizedModule) ? "Pnp Bridge is running on an IoT edge module." : "Pnp Bridge is running on an IoT device."));
    return dockerizedModule;
}


IOTHUB_CLIENT_RESULT 
PnpBridge_Create(
    PPNP_BRIDGE* PnpBridge
    )
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;
    PPNP_BRIDGE pbridge = NULL;
    bool lockAcquired = false;

    {
        g_PnpBridgeState = PNP_BRIDGE_UNINITIALIZED;

        // Allocate memory for the PNP_BRIDGE structure
        pbridge = (PPNP_BRIDGE) calloc(1, sizeof(PNP_BRIDGE));
        if (NULL == pbridge) {
            LogError("Failed to allocate memory for PnpBridge global");
            result = IOTHUB_CLIENT_ERROR;
            goto exit;
        }

        pbridge->ExitCondition = Condition_Init();
        if (NULL == pbridge->ExitCondition) {
            LogError("Failed to init ExitCondition");
            result = IOTHUB_CLIENT_ERROR;
            goto exit;
        }

        pbridge->ExitLock = Lock_Init();
        if (NULL == pbridge->ExitLock) {
            LogError("Failed to init ExitLock lock");
            result = IOTHUB_CLIENT_ERROR;
            goto exit;
        }
        Lock(pbridge->ExitLock);
        lockAcquired = true;

        {
            if (PnpBridge_IsRunningInEdgeRuntime())
            {
                pbridge->IoTClientType = PNP_BRIDGE_IOT_TYPE_RUNTIME_MODULE;
            }
            else
            {
                pbridge->IoTClientType = PNP_BRIDGE_IOT_TYPE_DEVICE;
            }
        }

        LogInfo("Pnp Bridge creation succeeded.");
        *PnpBridge = pbridge;
    }
exit:
    {
        if (IOTHUB_CLIENT_OK != result) {
            if (lockAcquired) {
                Unlock(pbridge->ExitLock);
            }
            PnpBridge_Release(pbridge);
            *PnpBridge = NULL;
        }
    }
   
    return result;
}

IOTHUB_CLIENT_RESULT 
PnpBridge_Initialize(
    PPNP_BRIDGE PnpBridge,
    const char * ConfigFilePath
    )
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    {
        if (PnpBridge->IoTClientType == PNP_BRIDGE_IOT_TYPE_RUNTIME_MODULE)
        {
            result = PnpBridge_InitializeEdgeModuleConfig(&PnpBridge->Configuration);
            if (IOTHUB_CLIENT_OK != result) {
                LogError("Failed to initilize IoT Edge Runtime module configuration for module client handle");
                goto exit;
            }
            LogInfo("IoT Edge Module configuration initialized successfully");
        }
        else
        {
            result = PnpBridge_InitializeDeviceConfig(&PnpBridge->Configuration, ConfigFilePath);
            if (IOTHUB_CLIENT_OK != result) {
                LogError("Failed to initialize Pnp Bridge device configuration");
                goto exit;
            }
            LogInfo("IoT Edge Device configuration initialized successfully");
        }
    }
exit:
    {
        if (IOTHUB_CLIENT_OK != result) {
            PnpBridge_Release(PnpBridge);
            PnpBridge = NULL;
        }
    }
   
    return result;
}

IOTHUB_CLIENT_RESULT 
PnpBridge_RegisterIoTHubHandle()
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    result = IotComms_InitializeIotHandle(&g_PnpBridge->IotHandle, g_PnpBridge->Configuration.ConnParams);
    if (IOTHUB_CLIENT_OK != result) {
        LogError("IotComms_InitializeIotHandle failed.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

exit:
    return result;
}

IOTHUB_CLIENT_RESULT 
PnpBridge_UnregisterIoTHubHandle()
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    result = IotComms_DeinitializeIotHandle(&g_PnpBridge->IotHandle, g_PnpBridge->Configuration.ConnParams);
    if (IOTHUB_CLIENT_OK != result) {
        LogError("IotComms_DeinitializeIotHandle failed.");
        result = IOTHUB_CLIENT_ERROR;
        goto exit;
    }

exit:
    return result;
}

void 
PnpBridge_Release(
    PPNP_BRIDGE pnpBridge
    )
{
    LogInfo("Cleaning Pnp Bridge resources");

    assert((PNP_BRIDGE_UNINITIALIZED == g_PnpBridgeState) ||
           (PNP_BRIDGE_INITIALIZED == g_PnpBridgeState) ||
           (PNP_BRIDGE_TEARING_DOWN == g_PnpBridgeState));

    g_PnpBridgeState = PNP_BRIDGE_DESTROYED;

    if (pnpBridge->PnpMgr)
    {
        // Free resources used by components
        PnpAdapterManager_DestroyComponents(pnpBridge->PnpMgr);
        // Free resources used by adapters and adapter manager
        PnpAdapterManager_ReleaseManager(pnpBridge->PnpMgr);
        pnpBridge->PnpMgr = NULL;
    }

    if (NULL != pnpBridge->ExitCondition) {
        Condition_Deinit(pnpBridge->ExitCondition);
    }

    if (NULL != pnpBridge->ExitLock) {
        Lock_Deinit(pnpBridge->ExitLock);
    }

    if (pnpBridge) {
        free(pnpBridge);
    }
}

int
PnpBridge_Main(const char * ConfigurationFilePath)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_OK;

    {
            LogInfo("Starting Azure PnpBridge");

            result = PnpBridge_Create(&g_PnpBridge);
            if (IOTHUB_CLIENT_OK != result) {
                LogError("PnpBridge_Create failed: %d", result);
                goto exit;
            }

            result = PnpBridge_Initialize(g_PnpBridge, ConfigurationFilePath);
            if (IOTHUB_CLIENT_OK != result) {
                LogError("PnpBridge_Initialize failed: %d", result);
                goto exit;
            }

            if (g_PnpBridge->IoTClientType == PNP_BRIDGE_IOT_TYPE_DEVICE)
            {
                // Build adapter manager, required adapters and components with adapter configuration from local file
                result = PnpAdapterManager_BuildAdaptersAndComponents(&g_PnpBridge->PnpMgr, g_PnpBridge->Configuration.JsonConfig, PNP_BRIDGE_IOT_TYPE_DEVICE);
                if (IOTHUB_CLIENT_OK != result)
                {
                    LogError("PnpAdapterManager_BuildAdaptersAndComponents failed: %d", result);
                    goto exit;
                }

                // After all Pnp Bridge Components have been built, state is initiaized
                g_PnpBridgeState = PNP_BRIDGE_INITIALIZED;

                // Register IoT Hub device client handle
                result = PnpBridge_RegisterIoTHubHandle();
                if (IOTHUB_CLIENT_OK != result) {
                    LogError("PnpBridge_RegisterIoTHubHandle failed: %d", result);
                    goto exit;
                }

                LogInfo("Connected to Azure IoT Hub");

                PnpAdapterManager_SendPnpBridgeStateTelemetry(PnpBridge_ConfigurationComplete);

                // Start Pnp Components

                if (IOTHUB_CLIENT_OK != PnpAdapterManager_StartComponents(g_PnpBridge->PnpMgr))
                {
                    LogError("PnpAdapterManager_StartComponents failed");
                    goto exit;
                }

                LogInfo("Pnp components started successfully.");
            }
            else
            {
                // Register IoT Hub module client handle (This is required at this point to receive Bridge configuration property update)
                result = PnpBridge_RegisterIoTHubHandle();
                if (IOTHUB_CLIENT_OK != result) {
                    LogError("PnpBridge_RegisterIoTHubHandle failed: %d", result);
                    goto exit;
                }

                // Bridge waits for adapter and component configuration to be sent down as an edge module desired property
                LogInfo("Pnp Bridge adapter and component initialization pending while running as an edge module");

                // Send telemetry indicating Bridge State is currently waiting for confuration initialization
                if (g_PnpBridgeState != PNP_BRIDGE_INITIALIZED && g_PnpBridge->IotHandle.ClientHandleInitialized)
                {
                    PnpAdapterManager_SendPnpBridgeStateTelemetry(PnpBridge_WaitingForConfig);
                }

            }

            // Prevent main thread from returning by waiting for the
            // exit condition to be set. This condition will be set when
            // the bridge has received a stop signal
            // ExitLock was taken in call to PnpBridge_Initialize so does not need to be reacquired.
            Condition_Wait(g_PnpBridge->ExitCondition, g_PnpBridge->ExitLock, 0);
            Unlock(g_PnpBridge->ExitLock);

    } 
exit:
    {
        if (g_PnpBridge != NULL && g_PnpBridgeState != PNP_BRIDGE_DESTROYED)
        {
            PnpAdapterManager_StopComponents(g_PnpBridge->PnpMgr);
            PnpBridge_UnregisterIoTHubHandle();
            PnpBridge_Release(g_PnpBridge);
        }
        g_PnpBridge = NULL;
    }

    return result;
}

void 
PnpBridge_Stop()
{
    if (!g_PnpBridgeShutdown)
    {
        g_PnpBridgeShutdown = true;
        assert(g_PnpBridge != NULL);
        assert(g_PnpBridgeState == PNP_BRIDGE_INITIALIZED);

        if (PNP_BRIDGE_TEARING_DOWN != g_PnpBridgeState) {
            g_PnpBridgeState = PNP_BRIDGE_TEARING_DOWN;
            Lock(g_PnpBridge->ExitLock);
            Condition_Post(g_PnpBridge->ExitCondition);
            Unlock(g_PnpBridge->ExitLock);
        }
    }
}

// Note: PnpBridge_UploadToBlobAsync method is not synchronized 
// with the g_PnpBridge cleanup path

int
PnpBridge_UploadToBlobAsync(
    const char* pszDestination,
    const unsigned char* pbData,
    size_t cbData,
    IOTHUB_CLIENT_FILE_UPLOAD_CALLBACK iotHubClientFileUploadCallback,
    void* context
    )
{
    IOTHUB_CLIENT_RESULT    iotResult = IOTHUB_CLIENT_OK;
    IOTHUB_CLIENT_HANDLE handle = NULL;

    if (!g_PnpBridge->IotHandle.ClientHandleInitialized)
    {
        return IOTHUB_CLIENT_ERROR;
    }

    handle = g_PnpBridge->IotHandle.IsModule ? g_PnpBridge->IotHandle.u1.IotModule.moduleHandle :
                                               g_PnpBridge->IotHandle.u1.IotDevice.deviceHandle;

    if (NULL == pszDestination || (NULL == pbData && cbData > 0) ||
        (NULL != pbData && cbData == 0) ||
        NULL == iotHubClientFileUploadCallback)
    {
        return IOTHUB_CLIENT_INVALID_ARG;
    }

    iotResult = IoTHubClient_UploadToBlobAsync(handle,
        pszDestination,
        pbData,
        cbData,
        iotHubClientFileUploadCallback,
        context);
    switch (iotResult)
    {
    case IOTHUB_CLIENT_OK:
        return IOTHUB_CLIENT_OK;
        break;
    case IOTHUB_CLIENT_INVALID_ARG:
    case IOTHUB_CLIENT_INVALID_SIZE:
        return IOTHUB_CLIENT_INVALID_ARG;
        break;
    case IOTHUB_CLIENT_INDEFINITE_TIME:
    case IOTHUB_CLIENT_ERROR:
    default:
        return IOTHUB_CLIENT_ERROR;
        break;
    }
}