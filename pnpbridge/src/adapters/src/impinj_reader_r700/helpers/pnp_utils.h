// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#ifndef R700_UTILS_H
#define R700_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <parson.h>
#include "azure_c_shared_utility/xlogging.h"
#include "../impinj_reader_r700.h"

void
LogJsonPretty(
    const char* MsgFormat,
    JSON_Value* JsonValue, ...);

void
LogJsonPrettyStr(
    const char* MsgFormat,
    char* JsonString, ...);

JSON_Value*
Remove_JSON_Array(
    char* Payload,
    const char* ArrayName);

JSON_Value*
JSONArray2DtdlMap(
    char* Payload,
    const char* ArrayName,
    const char* MapName);

char*
DtdlMap2JSONArray(
    const char* Payload,
    const char* ArrayName);

const char*
GetStringFromPayload(
    JSON_Value* CommandValue,
    const char* ParamName);

const char*
GetObjectStringFromPayload(
    JSON_Value* Payload,
    const char* ParamName);

bool
CleanAntennaConfig(
    JSON_Object* jsonObj_AntennaConfig);

void
GetFirmwareVersion(
    PIMPINJ_READER Reader);

void
CheckRfidInterfaceType(
    PIMPINJ_READER Reader);

PUPGRADE_DATA
DownloadFile(
    const char* Url,
    int IsAutoReboot);

int
RebootWorker(
    void* context);

bool
ScheduleRebootWorker(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle);

bool
ScheduleUpgradeWorker(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle);

int
ProcessReboot(
    PNPBRIDGE_COMPONENT_HANDLE PnpComponentHandle,
    unsigned char** CommandResponse,
    size_t* CommandResponseSize);

#ifdef __cplusplus
}
#endif

#endif /* R700_UTILS_H */