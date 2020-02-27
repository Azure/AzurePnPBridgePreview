// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <pnpbridge.h>

#include "azure_c_shared_utility/azure_base32.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/lock.h"

// TODO: Fix this missing reference
#ifndef AZURE_UNREFERENCED_PARAMETER
#define AZURE_UNREFERENCED_PARAMETER(param)   (void)(param)
#endif

#ifdef WIN32
#include <Windows.h>
#include <cfgmgr32.h>
#include <ctype.h>
#include <winerror.h>
#else
typedef unsigned int DWORD;
typedef uint8_t byte;
typedef int HANDLE;
typedef short USHORT;
typedef uint16_t UINT16;

#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include <termios.h>
#include <unistd.h>
#endif

#include "parson.h"

#include "serial_pnp.h"

int SerialPnp_UartReceiver(void* context)
{
    int result = 0;
    PSERIAL_DEVICE_CONTEXT deviceContext = (PSERIAL_DEVICE_CONTEXT)context;

    while (result >= 0) {
        byte* desc = NULL;
        DWORD length;

        SerialPnp_RxPacket(deviceContext, &desc, &length, 0x00);
        if (desc != NULL)
        {
            SerialPnp_UnsolicitedPacket(deviceContext, desc, length);
            free(desc);
        }
    }

    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_TxPacket(PSERIAL_DEVICE_CONTEXT serialDevice, byte* OutPacket, int Length)
{
    DWORD write_size = 0;
    int error = 0;

    DWORD txLength = 1 + Length; // "+1" for start of frame byte
    // First iterate through and find out our new length
    for (int i = 0; i < Length; i++)
    {
        if ((SERIALPNP_START_OF_FRAME_BYTE == OutPacket[i]) || (SERIALPNP_ESCAPE_BYTE == OutPacket[i]))
        {
            txLength++;
        }
    }

    // Now construct outgoing buffer
    byte* SerialPnp_TxPacket = malloc(sizeof(byte) * txLength);
    if (!SerialPnp_TxPacket)
    {
        LogError("Error out of memory");
        return -1;
    }
    txLength = 1;
    SerialPnp_TxPacket[0] = 0x5A; // Start of frame
    for (int i = 0; i < Length; i++)
    {
        // Escape these bytes where necessary
        if ((SERIALPNP_START_OF_FRAME_BYTE == OutPacket[i]) || (SERIALPNP_ESCAPE_BYTE == OutPacket[i]))
        {
            SerialPnp_TxPacket[txLength++] = SERIALPNP_ESCAPE_BYTE;
            SerialPnp_TxPacket[txLength++] = (byte)(OutPacket[i] - 1);
        }
        else
        {
            SerialPnp_TxPacket[txLength++] = OutPacket[i];
        }
    }


#ifdef WIN32
    if (!WriteFile(serialDevice->hSerial, SerialPnp_TxPacket, txLength, &write_size, &serialDevice->osWriter))
    {
        // Write returned immediately, but is asynchronous
        if (ERROR_IO_PENDING != (error = GetLastError()))
        {
            // Write returned actual error and not just pending
            LogError("write failed: %d", error);
            return -1;
        }
        else
        {
            if (!GetOverlappedResult(serialDevice->hSerial, &serialDevice->osWriter, &write_size, TRUE))
            {
                error = GetLastError();
                LogError("write failed: %d", error);
                return -1;
            }
        }
    }
#else
    if ((write_size = write(serialDevice->hSerial, (const void *)SerialPnp_TxPacket, (size_t)txLength)) == -1)
    {
        LogError("write failed");
        error = -1;
    }
#endif
    // there might not be an explicit error from the above, but check that all the bytes got transmitted 
    if (write_size != txLength)
    {
        LogError("Timeout while writing");
        return -1;
    }
    free(SerialPnp_TxPacket);
    return DIGITALTWIN_CLIENT_OK;
        }

const EventDefinition* SerialPnp_LookupEvent(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, char* EventName, int InterfaceId)
{
    const InterfaceDefinition* interfaceDef;
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(interfaceDefinitions);

    for (int i = 0; i < InterfaceId - 1; i++)
    {
        if (NULL == interfaceDefHandle)
        {
            return NULL;
        }
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);

    SINGLYLINKEDLIST_HANDLE events = interfaceDef->Events;
    LIST_ITEM_HANDLE eventDef = singlylinkedlist_get_head_item(events);
    const EventDefinition* ev;
    while (NULL != eventDef)
    {
        ev = singlylinkedlist_item_get_value(eventDef);
        if (strcmp(ev->defintion.Name, EventName) == 0)
        {
            return ev;
        }
        eventDef = singlylinkedlist_get_next_item(eventDef);
    }

    return NULL;
}

byte* SerialPnp_StringSchemaToBinary(Schema schema, byte* buffer, int* length)
{
    byte* bd = NULL;
    char* data = (char*)buffer;

    if ((Float == schema) || (Int == schema))
    {
        bd = malloc(sizeof(byte) * 4);
        if (!bd)
        {
            LogError("Error out of memory");
            return NULL;
        }
        *length = 4;

        if (schema == Float)
        {
            float x = 0;
            x = (float)atof(data);
            memcpy(bd, &x, sizeof(float));
        }
        else if (schema == Int)
        {
            int x;
            x = atoi(data);
            memcpy(bd, &x, sizeof(int));
        }
    }
    else if (Boolean == schema)
    {
        bd = malloc(sizeof(byte) * 1);
        if (!bd)
        {
            LogError("Error out of memory");
            return NULL;
        }

        *length = 1;
        if (strcmp(data, "true"))
        {
            bd[0] = 1;
        }
        else if (strcmp(data, "false"))
        {
            bd[0] = 0;
        }
        else
        {
            free(bd);
            *length = 0;
            bd = NULL;
        }
    }
    else
    {
        LogError("Unknown schema");
    }

    return bd;
}

char* SerialPnp_BinarySchemaToString(Schema schema, byte* Data, byte length)
{
    const int MAXRXSTRLEN = 12; // longest case INT_MIN = -2147483648 (11 characters + 1)
    char* rxstrdata = malloc(MAXRXSTRLEN);
    if (!rxstrdata)
    {
        LogError("Error out of memory");
        return NULL;
    }

    if ((Float == schema) && (4 == length))
    {
        sprintf_s(rxstrdata, MAXRXSTRLEN, "%.6f", *((float*)Data));
    }
    else if (((Int == schema) && (4 == length)))
    {
        sprintf_s(rxstrdata, MAXRXSTRLEN, "%d", *((int*)Data));
    }
    else if (((Boolean == schema) && (1 == length)))
    {
        sprintf_s(rxstrdata, MAXRXSTRLEN, "%d", *((int*)Data));
    }
    else
    {
        LogError("Unknown schema");
        free(rxstrdata);
        return NULL;
    }

    return rxstrdata;
}

void SerialPnp_UnsolicitedPacket(PSERIAL_DEVICE_CONTEXT device, byte* packet, DWORD length)
{
    // Got an event
    if (SERIALPNP_PACKET_TYPE_EVENT_NOTIFICATION == packet[SERIALPNP_PACKET_PACKET_TYPE_OFFSET])
    {
        byte rxInterfaceId = packet[SERIALPNP_PACKET_INTERFACE_NUMBER_OFFSET];
        byte rxNameLength = packet[SERIALPNP_PACKET_NAME_LENGTH_OFFSET];
        DWORD rxDataSize = length - rxNameLength - SERIALPNP_PACKET_NAME_OFFSET;

        char* event_name = malloc(sizeof(char) * (rxNameLength + 1));
        if (!event_name)
        {
            LogError("Error out of memory");
            return;
        }
        memcpy(event_name, packet + SERIALPNP_PACKET_NAME_OFFSET, rxNameLength);
        event_name[rxNameLength] = '\0';

        const EventDefinition* ev = SerialPnp_LookupEvent(device->InterfaceDefinitions, event_name, rxInterfaceId);
        if (!ev)
        {
            LogError("Couldn't find event");
            free(event_name);
            return;
        }

        byte* rxData = malloc(sizeof(byte) * rxDataSize);
        if (!rxData)
        {
            LogError("Error out of memory");
            free(event_name);
            return;
        }

        memcpy(rxData, packet + SERIALPNP_PACKET_NAME_OFFSET + rxNameLength, rxDataSize);

        char* rxstrdata = SerialPnp_BinarySchemaToString(ev->DataSchema, rxData, (byte)rxDataSize);
        if (!rxstrdata)
        {
            LogError("Unknown schema");
            free(event_name);
            free(rxData);
            return;
        }
        LogInfo("%s: %s", ev->defintion.Name, rxstrdata);

        SerialPnp_SendEventAsync(PnpAdapterInterface_GetPnpInterfaceClient(device->pnpAdapterInterface), ev->defintion.Name, rxstrdata);

        free(event_name);
        free(rxData);
    }
    // Got a property update
    else if (SERIALPNP_PACKET_TYPE_PROPERTY_NOTIFICATION == packet[SERIALPNP_PACKET_PACKET_TYPE_OFFSET])
    {
        // TODO
    }
}

const PropertyDefinition* SerialPnp_LookupProperty(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, const char* propertyName, int InterfaceId)
{
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(interfaceDefinitions);
    const InterfaceDefinition* interfaceDef;

    for (int i = 0; i < InterfaceId - 1; i++)
    {
        if (NULL == interfaceDefHandle)
        {
            return NULL;
        }
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);
    if (NULL == interfaceDef)
    {
        return NULL;
    }

    SINGLYLINKEDLIST_HANDLE property = interfaceDef->Properties;
    LIST_ITEM_HANDLE eventDef = singlylinkedlist_get_head_item(property);
    const PropertyDefinition* ev;
    while (NULL != eventDef)
    {
        ev = singlylinkedlist_item_get_value(eventDef);
        if (strcmp(ev->defintion.Name, propertyName) == 0)
        {
            return ev;
        }
        eventDef = singlylinkedlist_get_next_item(eventDef);
    }

    return NULL;
}

const CommandDefinition* SerialPnp_LookupCommand(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, const char* commandName, int InterfaceId)
{
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(interfaceDefinitions);
    const InterfaceDefinition* interfaceDef;

    for (int i = 0; i < InterfaceId - 1; i++)
    {
        if (NULL == interfaceDefHandle)
        {
            return NULL;
        }
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);

    SINGLYLINKEDLIST_HANDLE command = interfaceDef->Commands;
    LIST_ITEM_HANDLE eventDef = singlylinkedlist_get_head_item(command);
    const CommandDefinition* ev;
    while (NULL != eventDef)
    {
        ev = singlylinkedlist_item_get_value(eventDef);
        if (strcmp(ev->defintion.Name, commandName) == 0)
        {
            return ev;
        }
        eventDef = singlylinkedlist_get_next_item(eventDef);
    }

    return NULL;
}

int SerialPnp_PropertyHandler(PSERIAL_DEVICE_CONTEXT serialDevice, const char* property, char* data)
{
    const PropertyDefinition* prop = SerialPnp_LookupProperty(serialDevice->InterfaceDefinitions, property, 0);
    byte* input = (byte*)data;

    if (NULL == prop)
    {
        return -1;
    }

    // otherwise serialize data
    int dataLength = 0;
    byte* inputPayload = SerialPnp_StringSchemaToBinary(prop->DataSchema, input, &dataLength);
    if (!inputPayload)
    {
        return -1;
    }

    int nameLength = (int)strlen(property);
    int txlength = SERIALPNP_PACKET_NAME_OFFSET + nameLength + dataLength;
    byte* txPacket = malloc(txlength);
    if (!txPacket)
    {
        LogError("Error out of memory");
        free(inputPayload);
        return -1;
    }

    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET] = (byte)(txlength & 0xFF);
    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET + 1] = (byte)(txlength >> 8);
    txPacket[SERIALPNP_PACKET_PACKET_TYPE_OFFSET] = SERIALPNP_PACKET_TYPE_PROPERTY_REQUEST;
    // txPacket[3] is reserved
    txPacket[SERIALPNP_PACKET_INTERFACE_NUMBER_OFFSET] = (byte)0;
    txPacket[SERIALPNP_PACKET_NAME_LENGTH_OFFSET] = (byte)nameLength;

    memcpy(txPacket + SERIALPNP_PACKET_NAME_OFFSET, property, nameLength);
    memcpy(txPacket + SERIALPNP_PACKET_NAME_OFFSET + nameLength, inputPayload, dataLength);

    LogInfo("Setting property %s to %s", property, input);

    SerialPnp_TxPacket(serialDevice, txPacket, txlength);

    free(inputPayload);
    free(txPacket);

    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_CommandHandler(PSERIAL_DEVICE_CONTEXT serialDevice, const char* command, char* data, char** response)
{
    Lock(serialDevice->CommandLock);

    const CommandDefinition* cmd = SerialPnp_LookupCommand(serialDevice->InterfaceDefinitions, command, 0);
    byte* input = (byte*)data;

    if (NULL == cmd)
    {
        Unlock(serialDevice->CommandLock);
        return -1;
    }

    // otherwise serialize data
    int length = 0;
    byte* inputPayload = SerialPnp_StringSchemaToBinary(cmd->RequestSchema, input, &length);
    if (!inputPayload)
    {
        Unlock(serialDevice->CommandLock);
        return -1;
    }

    int nameLength = (int)strlen(command);
    int txlength = SERIALPNP_PACKET_NAME_OFFSET + nameLength + length;
    byte* txPacket = malloc(txlength);
    if (!txPacket)
    {
        LogError("Error out of memory");
        free(inputPayload);
        Unlock(serialDevice->CommandLock);
        return -1;
    }

    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET] = (byte)(txlength & 0xFF);
    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET + 1] = (byte)(txlength >> 8);
    txPacket[SERIALPNP_PACKET_PACKET_TYPE_OFFSET] = SERIALPNP_PACKET_TYPE_COMMAND_REQUEST;
    //txPacket[3] is reserved
    txPacket[SERIALPNP_PACKET_INTERFACE_NUMBER_OFFSET] = (byte)0;
    txPacket[SERIALPNP_PACKET_NAME_LENGTH_OFFSET] = (byte)nameLength;

    memcpy(txPacket + SERIALPNP_PACKET_NAME_OFFSET, command, nameLength);
    memcpy(txPacket + SERIALPNP_PACKET_NAME_OFFSET + nameLength, inputPayload, length);

    LogInfo("Invoking command %s to %s", command, input);

    if (DIGITALTWIN_CLIENT_OK != SerialPnp_TxPacket(serialDevice, txPacket, txlength))
    {
        LogError("Error: command not sent to device.");
        free(inputPayload);
        free(txPacket);
        Unlock(serialDevice->CommandLock);
        return -1;
    }

    Lock(serialDevice->CommandResponseWaitLock);
    if (COND_OK != Condition_Wait(serialDevice->CommandResponseWaitCondition, serialDevice->CommandResponseWaitLock, 60000))
    {
        LogError("Timeout waiting for response from device");
        free(inputPayload);
        free(txPacket);
        Unlock(serialDevice->CommandLock);
        Unlock(serialDevice->CommandResponseWaitLock);
        return -1;
    }
    Unlock(serialDevice->CommandResponseWaitLock);

    byte* responsePacket = serialDevice->pbMainBuffer;
    int dataOffset = SERIALPNP_PACKET_NAME_OFFSET + nameLength;
    byte* commandResponse = responsePacket + dataOffset;
    char* stval = SerialPnp_BinarySchemaToString(cmd->ResponseSchema, commandResponse, (byte)length);
    free(inputPayload);
    free(responsePacket);
    free(txPacket);

    if (!stval)
    {
        Unlock(serialDevice->CommandLock);
        return -1;
    }

    *response = stval;
    Unlock(serialDevice->CommandLock);
    return DIGITALTWIN_CLIENT_OK;
}

void SerialPnp_ParseDescriptor(SINGLYLINKEDLIST_HANDLE interfaceDefinitions, byte* descriptor, DWORD length)
{
    int c = 4;

    byte version = descriptor[c++];
    byte display_name_length = descriptor[c++];

    char display_name[128];
    memcpy(display_name, descriptor + c, display_name_length);
    display_name[display_name_length] = '\0';

    LogInfo("Device Version : %d", version);
    LogInfo("Device Name    : %s", display_name);

    c += display_name_length;

    while (c < (int)length)
    {
        if (descriptor[c++] == 0x05)
        {
            // inline interface
            InterfaceDefinition* indef = calloc(1, sizeof(InterfaceDefinition));
            indef->Events = singlylinkedlist_create();
            indef->Properties = singlylinkedlist_create();
            indef->Commands = singlylinkedlist_create();

            // parse ID5
            UINT16 interface_id_length = descriptor[c] | (descriptor[c + 1] << 8);
            c += 2;

            char* interface_id = malloc(sizeof(char)*(interface_id_length + 1));
            if (!interface_id)
            {
                LogError("Error out of memory");
                free(indef);
                return;
            }
            memcpy(interface_id, descriptor + c, interface_id_length);
            interface_id[interface_id_length] = '\0';
            LogInfo("Interface ID : %s", interface_id);
            c += interface_id_length;

            indef->Id = interface_id;

            // now process the different types
            while (c < (int)length)
            {
                if (descriptor[c] > 0x03) break; // not in a type of nested entry anymore

                FieldDefinition fielddef = { 0 };

                // extract common field properties
                byte ptype = descriptor[c++];
                byte pname_length = descriptor[c++];

                char* pname = malloc(sizeof(char)*(pname_length + 1));
                if (!pname)
                {
                    LogError("Error out of memory");
                    free(interface_id);
                    free(indef);
                    return;
                }
                memcpy(pname, descriptor + c, pname_length);
                pname[pname_length] = '\0';
                c += pname_length;

                byte pdisplay_name_length = descriptor[c++];
                char* pdisplay_name = malloc(sizeof(char)*(pdisplay_name_length + 1));
                if (!pdisplay_name)
                {
                    LogError("Error out of memory");
                    free(interface_id);
                    free(pname);
                    free(indef);
                    return;
                }
                memcpy(pdisplay_name, descriptor + c, pdisplay_name_length);
                pdisplay_name[pdisplay_name_length] = '\0';
                c += pdisplay_name_length;

                byte pdescription_length = descriptor[c++];
                char* pdescription = malloc(sizeof(char)*(pdescription_length + 1));
                if (!pdescription)
                {
                    LogError("Error out of memory");
                    free(interface_id);
                    free(pname);
                    free(pdisplay_name);
                    free(indef);
                    return;
                }
                memcpy(pdescription, descriptor + c, pdescription_length);
                pdescription[pdescription_length] = '\0';
                c += pdescription_length;

                LogInfo("\tProperty type : %d", ptype);
                LogInfo("\tName : %s", pname);
                LogInfo("\tDisplay Name : %s", pdisplay_name);
                LogInfo("\tDescription : %s", pdescription);

                fielddef.Name = pname;
                fielddef.DisplayName = pdisplay_name;
                fielddef.Description = pdescription;

                if (ptype == 0x01) // command
                {
                    CommandDefinition* tfdef = calloc(1, sizeof(CommandDefinition));
                    if (!tfdef)
                    {
                        LogError("Error out of memory");
                        free(interface_id);
                        free(pname);
                        free(pdisplay_name);
                        free(pdescription);
                        free(indef);
                        return;
                    }
                    tfdef->defintion = fielddef;

                    UINT16 prequest_schema = (UINT16)(descriptor[c] | (descriptor[c + 1] << 8));
                    c += 2;
                    UINT16 presponse_schema = (UINT16)(descriptor[c] | (descriptor[c + 1] << 8));
                    c += 2;

                    tfdef->RequestSchema = prequest_schema;
                    tfdef->ResponseSchema = presponse_schema;

                    singlylinkedlist_add(indef->Commands, tfdef);
                }
                else if (ptype == 0x02) // property
                {
                    PropertyDefinition* tfdef = calloc(1, sizeof(PropertyDefinition));
                    if (!tfdef)
                    {
                        LogError("Error out of memory");
                        free(interface_id);
                        free(pname);
                        free(pdisplay_name);
                        free(pdescription);
                        free(indef);
                        return;
                    }
                    tfdef->defintion = fielddef;

                    byte punit_length = descriptor[c++];
                    char* punit = malloc(sizeof(char) * (punit_length + 1));
                    if (!punit)
                    {
                        LogError("Unexpected error");
                        free(interface_id);
                        free(pname);
                        free(pdisplay_name);
                        free(pdescription);
                        free(tfdef);
                        free(indef);
                        return;
                    }
                    memcpy(punit, descriptor + c, punit_length);
                    punit[punit_length] = '\0';
                    c += punit_length;

                    LogInfo("\tUnit : %s", punit);

                    UINT16 schema = descriptor[c] | (descriptor[c + 1] << 8);
                    c += 2;
                    tfdef->DataSchema = schema;

                    byte flags = descriptor[c++];

                    bool prequired = (flags & (1 << 1)) != 0;
                    bool pwriteable = (flags & (1 << 0)) != 0;

                    tfdef->Units = punit;
                    tfdef->Required = prequired;
                    tfdef->Writeable = pwriteable;

                    singlylinkedlist_add(indef->Properties, tfdef);
                }
                else if (ptype == 0x03) // event
                {
                    EventDefinition* tfdef = calloc(1, sizeof(EventDefinition));
                    if (!tfdef)
                    {
                        LogError("Unexpected error");
                        free(interface_id);
                        free(pname);
                        free(pdisplay_name);
                        free(pdescription);
                        free(indef);
                        return;
                    }
                    tfdef->defintion = fielddef;

                    byte punit_length = descriptor[c++];
                    char* punit = malloc(sizeof(char) * (punit_length + 1));
                    if (!punit)
                    {
                        LogError("Error out of memory");
                        free(interface_id);
                        free(pname);
                        free(pdisplay_name);
                        free(pdescription);
                        free(tfdef);
                        free(indef);
                        return;
                    }
                    memcpy(punit, descriptor + c, punit_length);
                    punit[punit_length] = '\0';
                    c += punit_length;

                    LogInfo("\tUnit : %s", punit);

                    UINT16 schema = (UINT16)(descriptor[c] | (descriptor[c + 1] << 8));
                    c += 2;
                    tfdef->DataSchema = schema;
                    tfdef->Units = punit;

                    singlylinkedlist_add(indef->Events, tfdef);
                }
            }

            singlylinkedlist_add(interfaceDefinitions, indef);
        }
        else
        {
            LogError("Unsupported descriptor\n");
        }
    }
}

typedef struct _SERIAL_DEVICE
{
    char* InterfaceName;
} SERIAL_DEVICE, *PSERIAL_DEVICE;

SINGLYLINKEDLIST_HANDLE SerialDeviceList = NULL;
int SerialDeviceCount = 0;

#ifdef WIN32
int SerialPnp_FindSerialDevices()
{
    CONFIGRET cmResult = CR_SUCCESS;
    char* deviceInterfaceList;
    ULONG bufferSize = 0;

    SerialDeviceList = singlylinkedlist_create();

    cmResult = CM_Get_Device_Interface_List_Size(
        &bufferSize,
        (LPGUID)&GUID_DEVINTERFACE_COMPORT,
        NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (CR_SUCCESS != cmResult)
    {
        return -1;
    }

    deviceInterfaceList = malloc(bufferSize * sizeof(char));
    if (!deviceInterfaceList)
    {
        LogError("Error out of memory");
        return -1;
    }

    cmResult = CM_Get_Device_Interface_ListA(
        (LPGUID)&GUID_DEVINTERFACE_COMPORT,
        NULL,
        deviceInterfaceList,
        bufferSize,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (CR_SUCCESS != cmResult)
    {
        return -1;
    }

    for (PCHAR currentDeviceInterface = deviceInterfaceList;
        *currentDeviceInterface != '\0';
        currentDeviceInterface += strlen(currentDeviceInterface) + 1)
    {
        PSERIAL_DEVICE serialDevs = malloc(sizeof(SERIAL_DEVICE));
        if (!serialDevs)
        {
            LogError("Error out of memory");
            free(deviceInterfaceList);
            return -1;
        }
        serialDevs->InterfaceName = malloc((strlen(currentDeviceInterface) + 1) * sizeof(char));
        if (!serialDevs->InterfaceName)
        {
            LogError("Error out of memory");
            free(deviceInterfaceList);
            free(serialDevs);
            return -1;
        }
        strcpy_s(serialDevs->InterfaceName, strlen(currentDeviceInterface) + 1, currentDeviceInterface);
        singlylinkedlist_add(SerialDeviceList, serialDevs);
        SerialDeviceCount++;
    }

    return DIGITALTWIN_CLIENT_OK;
}
#endif

const char* serialDeviceChangeMessageformat = "{ \
                                                 \"identity\": \"serial-pnp-discovery\" \
                                               }";

int SerialPnp_OpenDeviceWorker(void* context)
{
    byte* desc;
    DWORD length;
    PSERIAL_DEVICE_CONTEXT deviceContext = context;
    int retries = SERIALPNP_RESET_OR_DESCRIPTOR_MAX_RETRIES;
    while (DIGITALTWIN_CLIENT_OK != SerialPnp_ResetDevice(deviceContext))
    {
        LogError("Error sending reset request. Retrying...");
        if (0 == --retries)
        {
            LogError("Error exceeded max number of reset request retries. ");
            return -1;
        }
        ThreadAPI_Sleep(5000);
    }
    retries = SERIALPNP_RESET_OR_DESCRIPTOR_MAX_RETRIES;
    while (DIGITALTWIN_CLIENT_OK != SerialPnp_DeviceDescriptorRequest(deviceContext, &desc, &length))
    {
        LogError("Descriptor response not received. Retrying...");
        if (0 == --retries)
        {
            LogError("Error exceeded max number of descriptor request retries. ");
            return -1;
        }
        ThreadAPI_Sleep(5000);
    }

    SerialPnp_ParseDescriptor(deviceContext->InterfaceDefinitions, desc, length);

    JSON_Value* json = json_parse_string(serialDeviceChangeMessageformat);
    JSON_Object* jsonObject = json_value_get_object(json);

    LIST_ITEM_HANDLE interfaceItem = singlylinkedlist_get_head_item(deviceContext->InterfaceDefinitions);
    while (NULL != interfaceItem)
    {
        const InterfaceDefinition* def = (const InterfaceDefinition*)singlylinkedlist_item_get_value(interfaceItem);
        //json_object_set_string(jsonObject, "InterfaceId", def->Id);

        PNPMESSAGE payload = NULL;
        PNPMESSAGE_PROPERTIES* props = NULL;

        PnpMessage_CreateMessage(&payload);

        PnpMessage_SetMessage(payload, json_serialize_to_string(json_object_get_wrapping_value(jsonObject)));
        PnpMessage_SetInterfaceId(payload, def->Id);
        
        props = PnpMessage_AccessProperties(payload);
        props->Context = deviceContext; 

        // Notify the pnpbridge of device discovery
        DiscoveryAdapter_ReportDevice(payload);
        interfaceItem = singlylinkedlist_get_next_item(interfaceItem);

        // Drop reference on the PNPMESSAGE
        PnpMemory_ReleaseReference(payload);
    }

    return DIGITALTWIN_CLIENT_OK;
}

#ifndef WIN32
int set_interface_attribs(int fd, int speed, int parity)
{
    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0)
    {
        LogError("error %d from tcgetattr", errno);
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
                                                    // disable IGNBRK for mismatched speed tests; otherwise receive break
                                                    // as \000 chars
    tty.c_iflag &= ~IGNBRK;         // disable break processing
    tty.c_lflag = 0;                // no signaling chars, no echo,
                                    // no canonical processing
    tty.c_oflag = 0;                // no remapping, no delays
    tty.c_cc[VMIN] = 1;            // read doesn't block
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    //tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                    // enable reading
    tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    //tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        LogError("error %d from tcsetattr", errno);
        return -1;
    }
    return DIGITALTWIN_CLIENT_OK;
}

void
set_blocking(int fd, int should_block)
{
    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0)
    {
        LogError("error %d from tggetattr", errno);
        return;
    }

    tty.c_cc[VMIN] = should_block ? 1 : 0;
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
        LogError("error %d setting term attributes", errno);
}
#endif 

int SerialPnp_OpenDevice(const char* port, DWORD baudRate)
{
#ifdef WIN32
    HANDLE hSerial = CreateFileA(port,
        GENERIC_READ | GENERIC_WRITE,
        0, // must be opened with exclusive-access
        0, // NULL, no security attributes
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        0);

    if (hSerial == INVALID_HANDLE_VALUE)
    {
        // Handle the error
        int error = GetLastError();
        LogError("Failed to open com port %s, %x", port, error);
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return -1;
        }

        return -1;
    }

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams))
    {
        return -1;
    }
    dcbSerialParams.BaudRate = baudRate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hSerial, &dcbSerialParams))
    {
        //error setting serial port state
        return -1;
    }

    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    if (!SetCommTimeouts(hSerial, &timeouts))
    {
        return -1;
    }
#else 
    char *portname = "/dev/ttyACM0";
    int fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
        LogError("error %d opening %s: %s", errno, portname, strerror(errno));
        return -1;
    }

    set_interface_attribs(fd, B115200, 0);  // set speed to 115,200 bps, 8n1 (no parity)
    //set_blocking(fd, 1);  // set no blocking
	ThreadAPI_Sleep(3000);
#endif

    PSERIAL_DEVICE_CONTEXT deviceContext = malloc(sizeof(SERIAL_DEVICE_CONTEXT));
    if (!deviceContext)
    {
        LogError("Error out of memory");
        return -1;
    }
    memset(deviceContext, 0, sizeof(SERIAL_DEVICE_CONTEXT));
    deviceContext->RxBufferIndex = 0;
    deviceContext->RxEscaped = false;

    deviceContext->CommandLock = Lock_Init();
    deviceContext->CommandResponseWaitLock = Lock_Init();
    deviceContext->CommandResponseWaitCondition = Condition_Init();
    if (NULL == deviceContext->CommandLock ||
        NULL == deviceContext->CommandResponseWaitLock ||
        NULL == deviceContext->CommandResponseWaitCondition)
    {
        if (NULL != deviceContext->CommandLock)
        {
            Lock_Deinit(deviceContext->CommandLock);
        }
        if (NULL != deviceContext->CommandResponseWaitLock)
        {
            Lock_Deinit(deviceContext->CommandResponseWaitLock);
        }
        if (NULL != deviceContext->CommandResponseWaitCondition)
        {
            Lock_Deinit(deviceContext->CommandResponseWaitCondition);
        }
        return -1;
    }

#ifdef WIN32
    deviceContext->hSerial = hSerial;
    deviceContext->osWriter.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    deviceContext->osReader.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (NULL == deviceContext->osWriter.hEvent ||
        NULL == deviceContext->osReader.hEvent)
    {
        if (NULL != deviceContext->osWriter.hEvent)
        {
            CloseHandle(deviceContext->osWriter.hEvent);
        }
        if (NULL != deviceContext->osReader.hEvent)
        {
            CloseHandle(deviceContext->osReader.hEvent);
        }
        return -1;
    }
#else 
    deviceContext->hSerial = fd;
#endif
    deviceContext->pnpAdapterInterface = NULL;
    deviceContext->InterfaceDefinitions = singlylinkedlist_create();

    // TODO error handling
    // https://docs.microsoft.com/en-us/previous-versions/ff802693(v=msdn.10)

    if (THREADAPI_OK != ThreadAPI_Create(&deviceContext->SerialDeviceWorker, SerialPnp_OpenDeviceWorker, deviceContext))
    {
        LogError("ThreadAPI_Create failed");
    }

    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_RxPacket(PSERIAL_DEVICE_CONTEXT serialDevice, byte** receivedPacket, DWORD* length, char packetType)
{
    byte inb = 0;
    DWORD dwRead = 0;
    *receivedPacket = NULL;
    *length = 0;
    int error = 0;

#ifdef WIN32
    while (true)
    {
        if (!ReadFile(serialDevice->hSerial, &inb, 1, &dwRead, &serialDevice->osReader)) // if completed asynchronously, wait. 
        {
            if (ERROR_IO_PENDING != (error = GetLastError()))
            {
                // Read returned actual error and not just pending
                LogError("read failed: %d", error);
                return -1;
            }
            else
            {
                if (!GetOverlappedResult(serialDevice->hSerial, &serialDevice->osReader, &dwRead, TRUE))
                {
                    error = GetLastError();
                    LogError("read failed: %d", error);
                    return -1;
                }
            }

        }

        // Read can be successful but with no bytes actually read, shouldn't happen though
        if (dwRead == 0)
        {
            continue;
        }
        //LogInfo("read completed successfully");
#else
    while ((dwRead = read(serialDevice->hSerial, (void*)&inb, 1)) != -1)
    {
#endif
        // Check for a start of packet byte
        if (SERIALPNP_START_OF_FRAME_BYTE == inb)
        {
            serialDevice->RxBufferIndex = 0;
            serialDevice->RxEscaped = false;
            continue;
        }

        // Check for an escape byte
        if (SERIALPNP_ESCAPE_BYTE == inb)
        {
            serialDevice->RxEscaped = true;
            continue;
        }

        // If last byte was an escape byte, increment current byte by 1
        if (serialDevice->RxEscaped)
        {
            inb++;
            serialDevice->RxEscaped = false;
        }

        serialDevice->RxBuffer[serialDevice->RxBufferIndex++] = (byte)inb;

        if (serialDevice->RxBufferIndex >= MAX_BUFFER_SIZE)
        {
            LogError("Filled Rx buffer. Protocol is bad.");
            return -1;
        }

        // Minimum packet length is 4, so once we are >= 4 begin checking
        // the receieve buffer length against the length field.
        if (serialDevice->RxBufferIndex >= SERIALPNP_MIN_PACKET_LENGTH)
        {
            int PacketLength = (int)((serialDevice->RxBuffer[0]) | (serialDevice->RxBuffer[1] << 8)); // LSB first, L-endian

            if (((int)serialDevice->RxBufferIndex == PacketLength) && (packetType == 0x00 || packetType == serialDevice->RxBuffer[2]))
            {
                *receivedPacket = malloc(serialDevice->RxBufferIndex * sizeof(byte));
                if (NULL == *receivedPacket)
                {
                    LogError("Error out of memory");
                    return -1;
                }
                *length = serialDevice->RxBufferIndex;
                memcpy(*receivedPacket, serialDevice->RxBuffer, serialDevice->RxBufferIndex);
                serialDevice->RxBufferIndex = 0; // This should be reset anyway. Deliver the newly receieved packet

                // both the main thread and this thread can be waiting for packets, 
                // but this function that does the reading only runs on this thread
                // command responses are expected on the main thread
                if (SERIALPNP_PACKET_TYPE_COMMAND_RESPONSE == serialDevice->RxBuffer[SERIALPNP_PACKET_PACKET_TYPE_OFFSET])
                {
                    // signal the main thread in this case, pass back the buffer 
                    // using the pointer in the serial context instead of the return value
                    serialDevice->pbMainBuffer = *receivedPacket;
                    *receivedPacket = NULL;
                    Lock(serialDevice->CommandResponseWaitLock);
                    Condition_Post(serialDevice->CommandResponseWaitCondition);
                    Unlock(serialDevice->CommandResponseWaitLock);
                }
                break;
            }
        }
    }
    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_ResetDevice(PSERIAL_DEVICE_CONTEXT serialDevice)
{
    int error = 0;
    // Prepare packet
    byte resetPacket[3] = { 0 }; // packet header
    byte* responsePacket = NULL;
    resetPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET] = 4; // length 4
    resetPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET + 1] = 0;
    resetPacket[SERIALPNP_PACKET_PACKET_TYPE_OFFSET] = SERIALPNP_PACKET_TYPE_RESET_REQUEST;

    // Send the new packet
    if (DIGITALTWIN_CLIENT_OK != SerialPnp_TxPacket(serialDevice, resetPacket, 4))
    {
        LogError("Error sending request packet");
        return -1;
    }
    LogInfo("Sent reset request");

    DWORD length;
    if (DIGITALTWIN_CLIENT_OK != SerialPnp_RxPacket(serialDevice, &responsePacket, &length, 0x02))
    {
        LogError("Error receiving response packet");
        return -1;
    }

    if (NULL == responsePacket)
    {
        LogError("received NULL for response packet");
        error = -1;
    }

    if (SERIALPNP_PACKET_TYPE_RESET_RESPONSE != responsePacket[2])
    {
        LogError("Bad reset response");
        error = -1;
    }

    if (0 == error)
    {
        LogInfo("Receieved reset response");
    }
    free(responsePacket);
    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_DeviceDescriptorRequest(PSERIAL_DEVICE_CONTEXT serialDevice, byte** desc, DWORD* length)
{
    // Prepare packet
    byte txPacket[3] = { 0 }; // packet header
    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET] = 4; // length 4
    txPacket[SERIALPNP_PACKET_PACKET_LENGTH_OFFSET + 1] = 0;
    txPacket[SERIALPNP_PACKET_PACKET_TYPE_OFFSET] = SERIALPNP_PACKET_TYPE_DESCRIPTOR_REQUEST;

    // Send the new packets
    if (DIGITALTWIN_CLIENT_OK != SerialPnp_TxPacket(serialDevice, txPacket, 4))
    {
        LogError("Error sending request packet");
        return -1;
    }
    LogInfo("Sent descriptor request");

    if (DIGITALTWIN_CLIENT_OK != SerialPnp_RxPacket(serialDevice, desc, length, 0x04))
    {
        LogError("Error receiving response packet");
        free(*desc);
        return -1;
    }

    if (NULL == *desc)
    {
        LogError("received NULL for response packet");
        free(*desc);

        return -1;
    }

    if (SERIALPNP_PACKET_TYPE_DESCRIPTOR_RESPONSE != (*desc)[2])
    {
        LogError("Bad descriptor response");
        free(*desc);
        return -1;
    }

    LogInfo("Receieved descriptor response, of length %d", *length);
    return DIGITALTWIN_CLIENT_OK;
}

int 
SerialPnp_StartDiscovery(
    _In_ PNPMEMORY deviceArgs,
    _In_ PNPMEMORY adapterArgs
    ) 
{
    if (NULL == deviceArgs)
    {
        LogInfo("SerialPnp_StartDiscovery: No device discovery parameters found in configuration.");
        return DIGITALTWIN_CLIENT_OK;
    }

    AZURE_UNREFERENCED_PARAMETER(adapterArgs);

    const char* port = NULL;
    const char* useComDevInterfaceStr;
    const char* baudRateParam;
    bool useComDeviceInterface = false;
    JSON_Value* jvalue = json_parse_string(((PDEVICE_ADAPTER_PARMAETERS)(((PPNPBRIDGE_MEMORY_TAG)deviceArgs)->memory))->AdapterParameters[0]);
    JSON_Object* args = json_value_get_object(jvalue);

    useComDevInterfaceStr = (const char*)json_object_dotget_string(args, "use_com_device_interface");
    if ((NULL != useComDevInterfaceStr) && (0 == strcmp(useComDevInterfaceStr, "true")))
    {
        useComDeviceInterface = true;
    }

    if (!useComDeviceInterface)
    {
        port = (const char*)json_object_dotget_string(args, "com_port");
        if (NULL == port)
        {
            LogError("ComPort parameter is missing in configuration");
            return -1;
        }
    }

    baudRateParam = (const char*)json_object_dotget_string(args, "baud_rate");
    if (NULL == baudRateParam)
    {
        LogError("BaudRate parameter is missing in configuration");
        return -1;
    }

    PSERIAL_DEVICE seriaDevice = NULL;
    DWORD baudRate = atoi(baudRateParam);
    if (useComDeviceInterface)
    {
#ifdef WIN32
        if (SerialPnp_FindSerialDevices() < 0)
#endif
        {
            LogError("Failed to get com port %s", port);
            return -1;
        }

        LIST_ITEM_HANDLE item = singlylinkedlist_get_head_item(SerialDeviceList);
        if (NULL == item)
        {
            LogError("No serial device was found %s", port);
            return -1;
        }

        seriaDevice = (PSERIAL_DEVICE)singlylinkedlist_item_get_value(item);
    }


    LogInfo("Opening com port %s", useComDeviceInterface ? seriaDevice->InterfaceName : port);

    SerialPnp_OpenDevice(useComDeviceInterface ? seriaDevice->InterfaceName : port, baudRate);
    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_StopDiscovery()
{
    return DIGITALTWIN_CLIENT_OK;
}

void SerialPnp_SendEventCallback(DIGITALTWIN_CLIENT_RESULT pnpSendEventStatus, void* userContextCallback)
{
    LogInfo("SerialDataSendEventCallback called, result=%d, userContextCallback=%p", pnpSendEventStatus, userContextCallback);
}

int SerialPnp_SendEventAsync(DIGITALTWIN_INTERFACE_CLIENT_HANDLE pnpInterface, char* eventName, char* data)
{
    DIGITALTWIN_CLIENT_RESULT pnpClientResult;

    if (pnpInterface == NULL)
    {
        return DIGITALTWIN_CLIENT_OK;
    }

    char telemetryMessageData[512] = { 0 };
    sprintf(telemetryMessageData, "{\"%s\":%s}", eventName, data);

    if ((pnpClientResult = DigitalTwin_InterfaceClient_SendTelemetryAsync(pnpInterface, (unsigned char*)telemetryMessageData, strlen(telemetryMessageData), SerialPnp_SendEventCallback, (void*)eventName)) != DIGITALTWIN_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_SendEventAsync failed, result=%d\n", pnpClientResult);
    }

    return pnpClientResult;
}

static void SerialPnp_PropertyUpdateHandler(const DIGITALTWIN_CLIENT_PROPERTY_UPDATE* dtClientPropertyUpdate, void* userContextCallback)
{
    DIGITALTWIN_CLIENT_RESULT pnpClientResult;
    DIGITALTWIN_CLIENT_PROPERTY_RESPONSE propertyResponse;
    PSERIAL_DEVICE_CONTEXT deviceContext;

    if (dtClientPropertyUpdate->propertyDesired && NULL != userContextCallback)
    {
        LogInfo("Processed property.  propertyUpdated = %.*s",
            (int)dtClientPropertyUpdate->propertyDesiredLen, dtClientPropertyUpdate->propertyDesired);

        deviceContext = (PSERIAL_DEVICE_CONTEXT)userContextCallback;

        SerialPnp_PropertyHandler(deviceContext, dtClientPropertyUpdate->propertyName, (char*)dtClientPropertyUpdate->propertyDesired);

        // TODO: Send the correct response code below
        propertyResponse.version = 1;
        //propertyResponse.propertyData = propertyDataUpdated;
        //propertyResponse.propertyDataLen = propertyDataUpdatedLen;
        propertyResponse.responseVersion = dtClientPropertyUpdate->desiredVersion;
        propertyResponse.statusCode = 200;
        propertyResponse.statusDescription = "Property Updated Successfully";

        pnpClientResult = DigitalTwin_InterfaceClient_ReportPropertyAsync(
            PnpAdapterInterface_GetPnpInterfaceClient(deviceContext->pnpAdapterInterface),
            dtClientPropertyUpdate->propertyName, dtClientPropertyUpdate->propertyDesired, dtClientPropertyUpdate->propertyDesiredLen, &propertyResponse, NULL, NULL);
    }

    
}

// PnPSampleEnvironmentalSensor_SetCommandResponse is a helper that fills out a PNP_CLIENT_COMMAND_RESPONSE
static void SerialPnp_SetCommandResponse(DIGITALTWIN_CLIENT_COMMAND_RESPONSE* pnpClientCommandResponseContext, const char* responseData, int status)
{
    size_t responseLen = strlen(responseData);
    memset(pnpClientCommandResponseContext, 0, sizeof(*pnpClientCommandResponseContext));
    pnpClientCommandResponseContext->version = DIGITALTWIN_CLIENT_COMMAND_RESPONSE_VERSION_1;

    // Allocate a copy of the response data to return to the invoker.  The PnP layer that invoked PnPSampleEnvironmentalSensor_Blink
    // takes responsibility for freeing this data.
    if ((pnpClientCommandResponseContext->responseData = malloc(responseLen + 1)) == NULL)
    {
        LogError("ENVIRONMENTAL_SENSOR_INTERFACE: Unable to allocate response data");
        pnpClientCommandResponseContext->status = 500;
    }
    else
    {
        pnpClientCommandResponseContext->responseData = (unsigned char*)responseData;
        //strcpy_s((char*)pnpClientCommandResponseContext->responseData, responseData);
        pnpClientCommandResponseContext->responseDataLen = responseLen;
        pnpClientCommandResponseContext->status = status;
    }
}

void 
SerialPnp_CommandUpdateHandler(
    const DIGITALTWIN_CLIENT_COMMAND_REQUEST* dtClientCommandContext,
    DIGITALTWIN_CLIENT_COMMAND_RESPONSE* dtClientCommandResponseContext,
    void* userContextCallback
    )
{
    PSERIAL_DEVICE_CONTEXT deviceContext;

    // LogInfo("Processed command frequency property updated.  propertyUpdated = %.*s", (int)propertyDataUpdatedLen, propertyDataUpdated);

    deviceContext = (PSERIAL_DEVICE_CONTEXT)userContextCallback;

    char* response = NULL;
    SerialPnp_CommandHandler(deviceContext, dtClientCommandContext->commandName, (char*)dtClientCommandContext->requestData, &response);
    SerialPnp_SetCommandResponse(dtClientCommandResponseContext, response, 200);
}

const InterfaceDefinition* SerialPnp_GetInterface(PSERIAL_DEVICE_CONTEXT deviceContext, const char* interfaceId)
{
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(deviceContext->InterfaceDefinitions);
    while (interfaceDefHandle != NULL)
    {
        const InterfaceDefinition* interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);
        if (strcmp(interfaceDef->Id, interfaceId) == 0)
        {
            return interfaceDef;
        }
        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }
    return NULL;
}

int SerialPnp_GetListCount(SINGLYLINKEDLIST_HANDLE list)
{
    if (NULL == list)
    {
        return 0;
    }

    LIST_ITEM_HANDLE item = singlylinkedlist_get_head_item(list);
    int count = 0;
    while (NULL != item)
    {
        count++;
        item = singlylinkedlist_get_next_item(item);
    }
    return count;
}

int
SerialPnp_StartPnpInterface(
    _In_ PNPADAPTER_INTERFACE_HANDLE PnpInterface
    )
{
    PSERIAL_DEVICE_CONTEXT deviceContext = PnpAdapterInterface_GetContext(PnpInterface);
    if (deviceContext == NULL)
    {
        LogError("Device context is null");
        return -1;
    }

    // Start telemetry
    SerialPnp_UartReceiver(deviceContext);

    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_CreatePnpInterface(PNPADAPTER_CONTEXT AdapterHandle, PNPMESSAGE msg) 
{
    PNPMESSAGE_PROPERTIES* pnpMsgProps = NULL;
    pnpMsgProps = PnpMessage_AccessProperties(msg);
    PSERIAL_DEVICE_CONTEXT deviceContext = (PSERIAL_DEVICE_CONTEXT)pnpMsgProps->Context;
    const char* interfaceId = PnpMessage_GetInterfaceId(msg);
    int result = 0;

    // Create an Azure Pnp interface for each interface in the SerialPnp descriptor
    LIST_ITEM_HANDLE interfaceDefHandle = singlylinkedlist_get_head_item(deviceContext->InterfaceDefinitions);
    while (interfaceDefHandle != NULL)
    {
        PNPADAPTER_INTERFACE_HANDLE pnpAdapterInterface = NULL;
        DIGITALTWIN_INTERFACE_CLIENT_HANDLE pnpInterfaceClient = NULL;
        int propertyCount = 0;
        int commandCount = 0;
        const InterfaceDefinition* interfaceDef = singlylinkedlist_item_get_value(interfaceDefHandle);

        propertyCount = SerialPnp_GetListCount(interfaceDef->Properties);
        commandCount = SerialPnp_GetListCount(interfaceDef->Commands);

        DIGITALTWIN_CLIENT_RESULT dtRes;
        //PNPMESSAGE_PROPERTIES* props = PnpMessage_AccessProperties(msg);
        dtRes = DigitalTwin_InterfaceClient_Create(interfaceId,
                                "serial" /* props->ComponentName */, // TODO: add component name into serial interface definition
                                NULL,
                                deviceContext,
                                &pnpInterfaceClient);
        if (DIGITALTWIN_CLIENT_OK != dtRes)
        {
            result = -1;
            goto exit;
        }

        if (propertyCount > 0) {
            dtRes = DigitalTwin_InterfaceClient_SetPropertiesUpdatedCallback(pnpInterfaceClient,
                        SerialPnp_PropertyUpdateHandler, (void*)deviceContext);
            if (DIGITALTWIN_CLIENT_OK != dtRes)
            {
                result = -1;
                goto exit;
            }
        }

        if (commandCount > 0) {
            dtRes = DigitalTwin_InterfaceClient_SetCommandsCallback(pnpInterfaceClient, 
                        SerialPnp_CommandUpdateHandler, (void*)deviceContext);
            if (DIGITALTWIN_CLIENT_OK != dtRes)
            {
                result = -1;
                goto exit;
            }
        }

        // Create PnpAdapter Interface
        {
            PNPADPATER_INTERFACE_PARAMS interfaceParams = { 0 };
            PNPADPATER_INTERFACE_PARAMS_INIT(&interfaceParams, AdapterHandle, pnpInterfaceClient);
            interfaceParams.InterfaceId = (char*)interfaceId;
            interfaceParams.ReleaseInterface = SerialPnp_ReleasePnpInterface;
            interfaceParams.StartInterface = SerialPnp_StartPnpInterface;

            result = PnpAdapterInterface_Create(&interfaceParams, &pnpAdapterInterface);
            if (result < 0)
            {
                goto exit;
            }
        }

        // Save the PnpAdapterInterface in device context
        deviceContext->pnpAdapterInterface = pnpAdapterInterface;

        PnpAdapterInterface_SetContext(pnpAdapterInterface, deviceContext);

        interfaceDefHandle = singlylinkedlist_get_next_item(interfaceDefHandle);
    }

exit:

    // Cleanup incase of failure
    if (result < 0) {
        // Destroy PnpInterfaceClient
        //if (NULL != pnpInterfaceClient) {
           //PnP_InterfaceClient_Destroy(pnpInterfaceClient);
        //}
    }
    
    return DIGITALTWIN_CLIENT_OK;
}

void SerialPnp_FreeFieldDefinition(FieldDefinition* fdef) {
    if (NULL != fdef->Description)
    {
        free(fdef->Description);
    }

    if (NULL != fdef->DisplayName)
    {
        free(fdef->DisplayName);
    }

    if (NULL != fdef->Name)
    {
        free(fdef->Name);
    }
}

void SerialPnp_FreeEventDefinition(SINGLYLINKEDLIST_HANDLE events)
{
    if (NULL == events)
    {
        return;
    }

    LIST_ITEM_HANDLE eventItem = singlylinkedlist_get_head_item(events);
    while (NULL != eventItem)
    {
        EventDefinition* e = (EventDefinition*)singlylinkedlist_item_get_value(eventItem);
        SerialPnp_FreeFieldDefinition(&e->defintion);
        if (NULL != e->Units)
        {
            free(e->Units);
        }
        free(e);
        eventItem = singlylinkedlist_get_next_item(eventItem);
    }
}

void SerialPnp_FreeCommandDefinition(SINGLYLINKEDLIST_HANDLE cmds)
{
    if (NULL == cmds)
    {
        return;
    }

    LIST_ITEM_HANDLE cmdItem = singlylinkedlist_get_head_item(cmds);
    while (NULL != cmdItem)
    {
        CommandDefinition* c = (CommandDefinition*)singlylinkedlist_item_get_value(cmdItem);
        SerialPnp_FreeFieldDefinition(&c->defintion);
        free(c);
        cmdItem = singlylinkedlist_get_next_item(cmdItem);
    }
}

void SerialPnp_FreePropertiesDefinition(SINGLYLINKEDLIST_HANDLE props)
{
    if (NULL == props)
    {
        return;
    }

    LIST_ITEM_HANDLE propItem = singlylinkedlist_get_head_item(props);
    while (NULL != propItem)
    {
        PropertyDefinition* p = (PropertyDefinition*)singlylinkedlist_item_get_value(propItem);
        SerialPnp_FreeFieldDefinition(&p->defintion);
        if (NULL != p->Units)
        {
            free(p->Units);
        }
        free(p);
        propItem = singlylinkedlist_get_next_item(propItem);
    }
}

int SerialPnp_ReleasePnpInterface(PNPADAPTER_INTERFACE_HANDLE pnpInterface)
{
    PSERIAL_DEVICE_CONTEXT deviceContext = PnpAdapterInterface_GetContext(pnpInterface);

    if (NULL == deviceContext)
    {
        return DIGITALTWIN_CLIENT_OK;
    }

#ifdef WIN32
    if (NULL != deviceContext->hSerial)
    {
        CloseHandle(deviceContext->hSerial);
    }
    if (NULL != deviceContext->osWriter.hEvent)
    {
        CloseHandle(deviceContext->osWriter.hEvent);
    }
    if (NULL != deviceContext->osReader.hEvent)
    {
        CloseHandle(deviceContext->osReader.hEvent);
    }
#else
    if (0 < deviceContext->hSerial)
    {
        close(deviceContext->hSerial);
    }
#endif

    if (deviceContext->InterfaceDefinitions)
    {
        LIST_ITEM_HANDLE interfaceItem = singlylinkedlist_get_head_item(deviceContext->InterfaceDefinitions);
        while (NULL != interfaceItem)
        {
            InterfaceDefinition* def = (InterfaceDefinition*)singlylinkedlist_item_get_value(interfaceItem);
            if (NULL != def->Id)
            {
                free(def->Id);
            }
            SerialPnp_FreeEventDefinition(def->Events);
            SerialPnp_FreeCommandDefinition(def->Commands);
            SerialPnp_FreePropertiesDefinition(def->Properties);

            free(def);
            interfaceItem = singlylinkedlist_get_next_item(interfaceItem);
        }
        singlylinkedlist_destroy(deviceContext->InterfaceDefinitions);
    }

    free(deviceContext);

    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_Initialize(const char* adapterArgs)
{
    AZURE_UNREFERENCED_PARAMETER(adapterArgs);
    return DIGITALTWIN_CLIENT_OK;
}

int SerialPnp_Shutdown()
{
    return DIGITALTWIN_CLIENT_OK;
}

DISCOVERY_ADAPTER SerialPnpDiscovery = {
    .Identity = "serial-pnp-discovery",
    .StartDiscovery = SerialPnp_StartDiscovery,
    .StopDiscovery = SerialPnp_StopDiscovery
};

PNP_ADAPTER SerialPnpInterface = {
    .identity = "serial-pnp-interface",
    .initialize = SerialPnp_Initialize,
    .shutdown = SerialPnp_Shutdown,
    .createPnpInterface = SerialPnp_CreatePnpInterface
};

