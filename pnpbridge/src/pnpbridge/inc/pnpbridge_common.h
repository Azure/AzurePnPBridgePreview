// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#ifndef PNPBRIDGE_COMMON_H
#define PNPBRIDGE_COMMON_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>

#if !defined(_MSC_VER)
#include <nosal.h>
#endif

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"

#include "azure_macro_utils/macro_utils.h"
#include "umock_c/umock_c_prod.h"

#include "iothub_transport_ll.h"
#include "iothub_message.h"

#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/condition.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/map.h"
#include "azure_c_shared_utility/strings_types.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/lock.h"

#include <iothub.h>
#include <iothub_device_client.h>
#include "iothub_module_client.h"
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>

#include "parson.h"

// PnP helper utilities.
#include "pnp_device_client.h"
#include "pnp_dps.h"
#include "pnp_protocol.h"
#include "pnp_bridge_client.h"

// Pnp Bridge headers
#include "configuration_parser.h"
#include "pnpadapter_manager.h"

#include <assert.h>

#define PNPBRIDGE_CLIENT_HANDLE void*

#define PNPBRIDGE_RESULT_VALUES \
    PNPBRIDGE_OK, \
    PNPBRIDGE_INSUFFICIENT_MEMORY, \
    PNPBRIDGE_FILE_NOT_FOUND, \
    PNPBRIDGE_NOT_SUPPORTED, \
    PNPBRIDGE_INVALID_ARGS, \
    PNPBRIDGE_CONFIG_READ_FAILED, \
    PNPBRIDGE_DUPLICATE_ENTRY, \
    PNPBRIDGE_FAILED

MU_DEFINE_ENUM(PNPBRIDGE_RESULT, PNPBRIDGE_RESULT_VALUES);

#define PNPBRIDGE_SUCCESS(Result) (Result == IOTHUB_CLIENT_OK)

MAP_RESULT Map_Add_Index(MAP_HANDLE handle, const char* key, int value);

int Map_GetIndexValueFromKey(MAP_HANDLE handle, const char* key);

#define PNP_CONFIG_CONNECTION_PARAMETERS "pnp_bridge_connection_parameters"
#define PNP_CONFIG_TRACE_ON "pnp_bridge_debug_trace"

#define PNP_CONFIG_CONNECTION_TYPE "connection_type"
#define PNP_CONFIG_CONNECTION_TYPE_STRING "connection_string"
#define PNP_CONFIG_CONNECTION_TYPE_DPS "dps"
#define PNP_CONFIG_CONNECTION_TYPE_EDGE_MODULE "edge_module"

#define PNP_CONFIG_CONNECTION_TYPE_CONFIG "config"
#define PNP_CONFIG_CONNECTION_TYPE_CONFIG_STRING "connection_string"

#define PNP_CONFIG_CONNECTION_TYPE_CONFIG_DPS "dps_parameters"
#define PNP_CONFIG_CONNECTION_DPS_GLOBAL_PROV_URI "global_prov_uri"
#define PNP_CONFIG_CONNECTION_DPS_ID_SCOPE "id_scope" 
#define PNP_CONFIG_CONNECTION_DPS_DEVICE_ID "device_id"
#define PNP_CONFIG_CONNECTION_ROOT_INTERFACE_MODEL_ID "root_interface_model_id"

#define PNP_CONFIG_CONNECTION_AUTH_PARAMETERS "auth_parameters"
#define PNP_CONFIG_CONNECTION_AUTH_TYPE "auth_type"
#define PNP_CONFIG_CONNECTION_AUTH_TYPE_SYMM "symmetric_key"
#define PNP_CONFIG_CONNECTION_AUTH_TYPE_X509 "x509"
#define PNP_CONFIG_CONNECTION_AUTH_TYPE_DEVICE_SYMM_KEY "symmetric_key"
#define PNP_CONFIG_CONNECTION_AUTH_TYPE_GROUP_SYMM_KEY "group_symmetric_key"

#define PNP_CONFIG_ADAPTER_GLOBAL "pnp_bridge_adapter_global_configs"
#define PNP_CONFIG_DEVICES "pnp_bridge_interface_components"
#define PNP_CONFIG_IDENTITY "identity"
#define PNP_CONFIG_COMPONENT_NAME "pnp_bridge_component_name"
#define PNP_CONFIG_ADAPTER_ID "pnp_bridge_adapter_id"
#define PNP_CONFIG_DEVICE_ADAPTER_CONFIG "pnp_bridge_adapter_config"
#define PNP_CONFIG_PUBLISH_MODE "publish_mode"
#define PNP_CONFIG_MATCH_FILTERS "match_filters"
#define PNP_CONFIG_MATCH_TYPE "match_type"
#define PNP_CONFIG_MATCH_TYPE_EXACT "exact"
#define PNP_CONFIG_MATCH_PARAMETERS "match_parameters"
#define PNP_CONFIG_DISCOVERY_ADAPTER "discovery_adapter"
#define PNP_CONFIG_PARAMETERS "parameters"
#define PNP_CONFIG_PNP_PARAMETERS "pnp_parameters"
#define PNP_CONFIG_DISCOVERY_PARAMETERS "discovery_parameters"
#define PNP_CONFIG_PNP_ADAPTERS "pnp_adapters"
#define PNP_CONFIG_DISCOVERY_ADAPTERS "discovery_adapters"
#define PNP_CONFIG_SELF_DESCRIBING "self_describing"
#define PNP_CONFIG_PUBLISH_MODE_ALWAYS "always"
#define PNP_CONFIG_TRUE "true"
#define PNP_CONFIG_FALSE "false"

#define PNPBRIDGE_MAX_PATH 2048

// Mode agnostic iot and pnp handle
typedef struct _MX_IOT_HANDLE_TAG {
    union {
        struct IotDevice {
            // connection to iot device
            IOTHUB_DEVICE_CLIENT_HANDLE deviceHandle;

        } IotDevice;

        struct IotModule {
            // connection to iot device
            IOTHUB_MODULE_CLIENT_HANDLE moduleHandle;

        } IotModule;
    } u1;

    bool IsModule;
    bool ClientHandleInitialized;
} MX_IOT_HANDLE_TAG;

typedef enum PNP_BRIDGE_STATE {
    PNP_BRIDGE_UNINITIALIZED,
    PNP_BRIDGE_INITIALIZED,
    PNP_BRIDGE_TEARING_DOWN,
    PNP_BRIDGE_DESTROYED
} PNP_BRIDGE_STATE;

// Device aggregator context
typedef struct _PNP_BRIDGE {
    MX_IOT_HANDLE_TAG IotHandle;

    PPNP_ADAPTER_MANAGER PnpMgr;

    PNPBRIDGE_CONFIGURATION Configuration;

    PNP_BRIDGE_IOT_TYPE IoTClientType;

    COND_HANDLE ExitCondition;

    LOCK_HANDLE ExitLock;
} PNP_BRIDGE, *PPNP_BRIDGE;


void PnpBridge_Release(PPNP_BRIDGE pnpBridge);

// User Agent String for Pnp Bridge telemetry [THIS VALUE SHOULD NEVER BE CHANGED]
static const char g_pnpBridgeUserAgentString[] = "PnpBridgeUserAgentString";
// Pnp Bridge desired property for component config
static const char g_pnpBridgeConfigProperty[] = "PnpBridgeConfig";
// Root Pnp Bridge Interface model ID when running as a module
static const char g_pnpBridgeModuleRootModelId[] = "PNP_BRIDGE_ROOT_MODEL_ID";
// Environment variable used to specify this application's connection string when running as a module
static const char g_connectionStringEnvironmentVariable[] = "IOTHUB_DEVICE_CONNECTION_STRING";
// Environment variable used to specify workload URI for dockerized containers/modules
static const char g_workloadURIEnvironmentVariable[] = "IOTEDGE_WORKLOADURI";
// Hub client tracing for when Pnp Bridge runs in Edge Module
static const char g_hubClientTraceEnabled[] = "PNP_BRIDGE_HUB_TRACING_ENABLED";

#ifdef __cplusplus
}
#endif

#endif /* PNPBRIDGE_COMMON_H */
