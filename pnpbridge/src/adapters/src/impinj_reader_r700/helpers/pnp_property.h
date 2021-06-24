// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#ifndef R700_PROPERTY_H
#define R700_PROPERTY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <parson.h>
#include <pnpadapter_api.h>
#include "restapi.h"
#include "pnp_utils.h"

static const char g_IoTHubTwinDesiredVersion[]    = "$version";
static const char g_IoTHubTwinDesiredObjectName[] = "desired";

bool
OnPropertyCompleteCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    JSON_Value* JsonVal_Payload,
    void* UserContextCallback);

void
OnPropertyPatchCallback(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* PropertyName,
    JSON_Value* JsonVal_Property,
    int Version,
    void* ClientHandle);

IOTHUB_CLIENT_RESULT
UpdateReadOnlyReportPropertyEx(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* ComponentName,
    char* PropertyName,
    JSON_Value* JsonVal_Property,
    bool Verbose);

IOTHUB_CLIENT_RESULT
UpdateReadOnlyReportProperty(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    const char* ComponentName,
    char* PropertyName,
    JSON_Value* JsonVal_Property);

IOTHUB_CLIENT_RESULT
UpdateWritableProperty(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    PIMPINJ_R700_REST RestRequest,
    JSON_Value* JsonVal_Property,
    int Ac,
    char* Ad,
    int Av);

#ifdef __cplusplus
}
#endif

#endif /* R700_PROPERTY_H */