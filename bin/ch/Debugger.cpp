//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "Debugger.h"

// Link with ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

DebuggerCh::DebuggerCh(LPCWSTR ipAddr, unsigned short port)
    : m_port(port), m_dbgConnectSocket(INVALID_SOCKET), m_dbgSocket(INVALID_SOCKET), m_context(nullptr), m_chakraDebugObject(nullptr), m_processJsrtEventData(nullptr), m_processDebugProtocolJSON(nullptr),
    m_waitingForMessage(false), m_isProcessingDebuggerMsg(false), m_msgQueue(), m_buf(nullptr), m_buflen(1024)
{
    memcpy_s(this->m_ipAddr, sizeof(wchar_t) * 20, ipAddr, sizeof(wchar_t) * (wcslen(ipAddr) + 1));

    this->m_buf = new char[this->m_buflen];
}

DebuggerCh::~DebuggerCh()
{
    if(this->m_dbgSocket != INVALID_SOCKET)
    {
        closesocket(this->m_dbgSocket);
        this->m_dbgSocket = INVALID_SOCKET;
    }

    if(this->m_dbgConnectSocket != INVALID_SOCKET)
    {
        closesocket(this->m_dbgConnectSocket);
        this->m_dbgConnectSocket = INVALID_SOCKET;
    }

    if(this->m_buf != nullptr)
    {
        delete[] this->m_buf;
    }
}

wchar_t* DebuggerCh::PopMessage()
{
    wchar_t* msg = this->m_msgQueue.front();
    this->m_msgQueue.pop();

    return msg;
}

void DebuggerCh::SendMsg(const wchar_t* msg, size_t msgLen)
{
    AssertMsg(msgLen < this->m_buflen, "Unexpectedly long msg!!!");

    for(size_t i = 0; i < msgLen; ++i)
    {
        this->m_buf[i] = (char)msg[i];
    }

    int iResult = send(this->m_dbgSocket, this->m_buf, (int)msgLen, 0);
    if(iResult == SOCKET_ERROR) 
    {
        wprintf(L"send failed with error: %d\n", WSAGetLastError());
        exit(1);
    }
}

bool DebuggerCh::IsEmpty()
{
    //if this is empty try reading out as many messages as we can
    if(this->m_msgQueue.empty())
    {
        int iResult;
        do
        {
            iResult = recv(this->m_dbgSocket, this->m_buf, (int)this->m_buflen, 0);
            if(iResult > 0)
            {
                AssertMsg(iResult < this->m_buflen, "Unexpectedly large message.");

                wchar_t* wbuff = (wchar_t*)malloc((iResult + 1) * sizeof(wchar_t));
                for(int i = 0; i < iResult; ++i)
                {
                    wbuff[i] = this->m_buf[i];
                }
                wbuff[iResult] = L'\0';

                this->m_msgQueue.push(wbuff);
            }

        } while(iResult != 0);
    }

    return !this->m_msgQueue.empty();
}

bool DebuggerCh::ShouldContinue()
{
    JsPropertyIdRef propertyIdRef;
    DGB_ENSURE_OK(ChakraRTInterface::JsGetPropertyIdFromName(L"shouldContinue", &propertyIdRef));

    JsValueRef shouldContinueRef;
    DGB_ENSURE_OK(ChakraRTInterface::JsGetProperty(this->m_chakraDebugObject, propertyIdRef, &shouldContinueRef));

    bool shouldContinue = true;
    DGB_ENSURE_OK(ChakraRTInterface::JsBooleanToBool(shouldContinueRef, &shouldContinue));

    return shouldContinue;
}

void DebuggerCh::WaitForMessage()
{
    //
    //We single threaded spin wait
    //
    while(this->IsEmpty()) 
    {
        Sleep(50);
    }
}

void DebuggerCh::ProcessDebuggerMessage()
{
    if(this->m_isProcessingDebuggerMsg)
    {
        return;
    }

    if(!this->IsEmpty())
    {
        this->m_isProcessingDebuggerMsg = true;

        wchar_t* msg = this->PopMessage();

        JsValueRef msgArg;
        DGB_ENSURE_OK(ChakraRTInterface::JsPointerToString(msg, wcslen(msg), &msgArg));

        JsValueRef undef;
        DGB_ENSURE_OK(ChakraRTInterface::JsGetUndefinedValue(&undef));

        JsValueRef args[] = { undef, msgArg };
        JsValueRef responseRef;
        DGB_ENSURE_OK(ChakraRTInterface::JsCallFunction(this->m_processDebugProtocolJSON, args, _countof(args), &responseRef));

        JsValueType resultType;
        DGB_ENSURE_OK(ChakraRTInterface::JsGetValueType(responseRef, &resultType));

        if(resultType == JsString)
        {
            const wchar_t* msgPtr = nullptr;
            size_t msgLen = 0;
            DGB_ENSURE_OK(ChakraRTInterface::JsStringToPointer(responseRef, &msgPtr, &msgLen));

            this->SendMsg(msgPtr, msgLen);
        }

        delete[] msg;
        this->m_isProcessingDebuggerMsg = false;
    }
}

bool DebuggerCh::ProcessJsrtDebugEvent(JsDiagDebugEvent debugEvent, JsValueRef eventData)
{
    JsValueRef debugEventRef;
    DGB_ENSURE_OK(ChakraRTInterface::JsIntToNumber(debugEvent, &debugEventRef));

    JsValueRef undef;
    DGB_ENSURE_OK(ChakraRTInterface::JsGetUndefinedValue(&undef));

    JsValueRef args[] = { undef, debugEventRef, eventData };
    JsValueRef result;
    DGB_ENSURE_OK(ChakraRTInterface::JsCallFunction(this->m_processJsrtEventData, args, _countof(args), &result));

    JsValueType resultType;
    DGB_ENSURE_OK(ChakraRTInterface::JsGetValueType(result, &resultType));

    if(resultType == JsString)
    {
        const wchar_t* msgPtr = nullptr;
        size_t msgLen = 0;
        DGB_ENSURE_OK(ChakraRTInterface::JsStringToPointer(result, &msgPtr, &msgLen));

        this->SendMsg(msgPtr, msgLen);
    }

    return this->ShouldContinue();
}

bool DebuggerCh::Initialize(JsRuntimeHandle runtime)
{
    HRESULT hr = S_OK;
    // setup chakra_debug.js callbacks
    // put runtime in debug mode

    IfJsrtErrorHR(ChakraRTInterface::JsCreateContext(runtime, &this->m_context));
    IfJsrtErrorHR(ChakraRTInterface::JsSetCurrentContext(this->m_context));

    JsValueRef globalFunc = JS_INVALID_REFERENCE;
    IfJsrtErrorHR(ChakraRTInterface::JsParseScriptWithFlags(controllerScript, JS_SOURCE_CONTEXT_NONE, L"chakra_debug.js", JsParseScriptAttributeLibraryCode, &globalFunc));

    JsValueRef undefinedValue;
    IfJsrtErrorHR(ChakraRTInterface::JsGetUndefinedValue(&undefinedValue));
    JsValueRef args[] = { undefinedValue };
    JsValueRef result = JS_INVALID_REFERENCE;
    IfJsrtErrorHR(ChakraRTInterface::JsCallFunction(globalFunc, args, _countof(args), &result));

    JsValueRef globalObj = JS_INVALID_REFERENCE;
    IfJsrtErrorHR(ChakraRTInterface::JsGetGlobalObject(&globalObj));

    JsPropertyIdRef chakraDebugPropId;
    IfJsrtErrorHR(ChakraRTInterface::JsGetPropertyIdFromName(L"chakraDebug", &chakraDebugPropId));

    JsPropertyIdRef chakraDebugObject;
    IfJsrtErrorHR(ChakraRTInterface::JsGetProperty(globalObj, chakraDebugPropId, &chakraDebugObject));

    return this->InstallDebugCallbacks(chakraDebugObject);

Error:
    return hr == S_OK;
}

bool DebuggerCh::InstallDebugCallbacks(JsValueRef chakraDebugObject)
{
    JsPropertyIdRef propertyIdRef;
    JsErrorCode errorCode = JsGetPropertyIdFromName(L"ProcessDebugProtocolJSON", &propertyIdRef);
    if(errorCode != JsNoError)
    {
        return false;
    }

    errorCode = JsGetProperty(chakraDebugObject, propertyIdRef, &this->m_processDebugProtocolJSON);
    if(errorCode != JsNoError)
    {
        return false;
    }

    errorCode = JsGetPropertyIdFromName(L"ProcessJsrtEventData", &propertyIdRef);
    if(errorCode != JsNoError)
    {
        return false;
    }

    errorCode = JsGetProperty(chakraDebugObject, propertyIdRef, &this->m_processJsrtEventData);
    if(errorCode != JsNoError)
    {
        return false;
    }

    this->m_chakraDebugObject = chakraDebugObject;

    ////
    bool installOk = true;

    installOk &= this->InstallHostCallback(chakraDebugObject, L"log", DebuggerCh::Log);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetScripts", DebuggerCh::JsDiagGetScripts);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetSource", DebuggerCh::JsGetSource);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagResume", DebuggerCh::JsDiagResume);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagSetBreakpoint", DebuggerCh::JsSetBreakpoint);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetFunctionPosition", DebuggerCh::JsDiagGetFunctionPosition);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetStacktrace", DebuggerCh::JsGetStacktrace);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetStackProperties", DebuggerCh::JsDiagGetStackProperties);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagLookupHandles", DebuggerCh::JsDiagLookupHandles);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagEvaluateScript", DebuggerCh::JsEvaluateScript);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagEvaluate", DebuggerCh::JsDiagEvaluate);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetBreakpoints", DebuggerCh::JsDiagGetBreakpoints);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetProperties", DebuggerCh::JsDiagGetProperties);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagRemoveBreakpoint", DebuggerCh::JsDiagRemoveBreakpoint);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagSetBreakOnException", DebuggerCh::JsDiagSetBreakOnException);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"JsDiagGetBreakOnException", DebuggerCh::JsDiagGetBreakOnException);
    installOk &= this->InstallHostCallback(chakraDebugObject, L"SendDelayedRespose", DebuggerCh::SendDelayedRespose);

    return installOk;
}

bool DebuggerCh::InstallHostCallback(JsValueRef chakraDebugObject, const wchar_t *name, JsNativeFunction nativeFunction)
{
    HRESULT hr = S_OK;

    JsPropertyIdRef propertyIdRef;
    IfJsrtErrorHR(ChakraRTInterface::JsGetPropertyIdFromName(name, &propertyIdRef));

    JsValueRef funcRef;
    IfJsrtErrorHR(ChakraRTInterface::JsCreateFunction(nativeFunction, nullptr, &funcRef));

    IfJsrtErrorHR(ChakraRTInterface::JsSetProperty(chakraDebugObject, propertyIdRef, funcRef, true));
Error:
    return hr == S_OK;
}

bool DebuggerCh::CallFunction(wchar_t const * functionName, JsValueRef* arguments, unsigned short argumentCount, JsValueRef *result)
{
    HRESULT hr = S_OK;
    JsValueRef globalObj = JS_INVALID_REFERENCE;
    JsValueRef targetFunc = JS_INVALID_REFERENCE;
    JsPropertyIdRef targetFuncId = JS_INVALID_REFERENCE;

    // Save the current context
    JsContextRef prevContext = JS_INVALID_REFERENCE;
    IfJsrtErrorHR(ChakraRTInterface::JsGetCurrentContext(&prevContext));

    // Set the current context
    IfJsrtErrorHR(ChakraRTInterface::JsSetCurrentContext(this->m_context));

    // Get the global object
    IfJsrtErrorHR(ChakraRTInterface::JsGetGlobalObject(&globalObj));

    // Get a script string for the function name
    IfJsrtErrorHR(ChakraRTInterface::JsGetPropertyIdFromName(functionName, &targetFuncId));

    // Get the target function
    IfJsrtErrorHR(ChakraRTInterface::JsGetProperty(globalObj, targetFuncId, &targetFunc));

    // Call the function
    IfJsrtErrorHR(ChakraRTInterface::JsCallFunction(targetFunc, arguments, argumentCount, result));

    // Restore the previous context
    IfJsrtErrorHR(ChakraRTInterface::JsSetCurrentContext(prevContext));

Error:
    return hr == S_OK;
}

void CALLBACK DebuggerCh::JsDiagDebugEventHandler(_In_ JsDiagDebugEvent debugEvent, _In_ JsValueRef eventData, _In_opt_ void* callbackState)
{
    DebuggerCh* debugger = (DebuggerCh*)callbackState;

    //If we haven't talked with the debugger yet wait for their connection
    if(debugger->m_dbgSocket == INVALID_SOCKET)
    {
        // Create a SOCKET for accepting incoming requests.
        debugger->m_dbgSocket = accept(debugger->m_dbgConnectSocket, NULL, NULL);
        if(debugger->m_dbgSocket == INVALID_SOCKET)
        {
            wprintf(L"accept failed with error: %ld\n", GetLastError());
            exit(1);
        }

        //set our listening socket to non-blocking
        DWORD yes = 1;
        if(ioctlsocket(debugger->m_dbgSocket, FIONBIO, &yes) == SOCKET_ERROR)
        {
            wprintf(L"ioctl failed with error: %ld\n", GetLastError());
            exit(1);
        }
    }

    //disable TTD actions before we do things in the JS runtime
    ChakraRTInterface::JsTTDPauseTimeTravelBeforeRuntimeOperation();

    debugger->HandleDebugEvent(debugEvent, eventData);

    //re-enable TTD actions after we are done with whatever we wanted in the JS runtime
    ChakraRTInterface::JsTTDReStartTimeTravelAfterRuntimeOperation();
}

JsValueRef DebuggerCh::Log(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    if(argumentCount > 1)
    {
        JsValueRef strRef = JS_INVALID_REFERENCE;
        LPCWSTR str = nullptr;
        size_t length;
        IfJsrtErrorRet(ChakraRTInterface::JsConvertValueToString(arguments[1], &strRef));
        IfJsrtErrorRet(ChakraRTInterface::JsStringToPointer(strRef, &str, &length));

        wprintf(L"%s\n", str);
    }

    return JS_INVALID_REFERENCE;
}

JsValueRef DebuggerCh::JsDiagGetScripts(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef sourcesList = JS_INVALID_REFERENCE;
    IfJsrtErrorRet(ChakraRTInterface::JsDiagGetScripts(&sourcesList));
    return sourcesList;
}

JsValueRef DebuggerCh::JsGetSource(JsValueRef callee, bool isConstructCall, JsValueRef * arguments, unsigned short argumentCount, void * callbackState)
{
    int scriptId;
    JsValueRef source = JS_INVALID_REFERENCE;

    if(argumentCount > 1)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[1], &scriptId));
        IfJsrtErrorRet(ChakraRTInterface::JsDiagGetSource(scriptId, &source));
    }

    return source;
}

JsValueRef DebuggerCh::JsDiagResume(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    bool success = false;
    int resumeType;
    if(argumentCount > 1 && ChakraRTInterface::JsNumberToInt(arguments[1], &resumeType) == JsNoError) {

        IfJsrtErrorRet(ChakraRTInterface::JsDiagResume((JsDiagResumeType)resumeType));
        success = true;
    }

    JsValueRef returnRef;
    if(success)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsGetTrueValue(&returnRef));
    }
    else
    {
        IfJsrtErrorRet(ChakraRTInterface::JsGetFalseValue(&returnRef));
    }

    return returnRef;
}

JsValueRef DebuggerCh::JsSetBreakpoint(JsValueRef callee, bool isConstructCall, JsValueRef * arguments, unsigned short argumentCount, void * callbackState)
{
    int scriptId;
    int line;
    int column;
    JsValueRef bpObject = JS_INVALID_REFERENCE;

    if(argumentCount > 3)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[1], &scriptId));
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[2], &line));
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[3], &column));

        IfJsrtErrorRet(ChakraRTInterface::JsDiagSetBreakpoint(scriptId, line, column, &bpObject));
    }

    return bpObject;
}

JsValueRef DebuggerCh::JsDiagGetFunctionPosition(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef valueRef = JS_INVALID_REFERENCE;
    IfJsrtErrorRet(ChakraRTInterface::JsConvertValueToObject(arguments[1], &valueRef));

    JsValueRef funcInfo = JS_INVALID_REFERENCE;
    IfJsrtErrorRet(ChakraRTInterface::JsDiagGetFunctionPosition(valueRef, &funcInfo));

    return funcInfo;
}

JsValueRef DebuggerCh::JsGetStacktrace(JsValueRef callee, bool isConstructCall, JsValueRef * arguments, unsigned short argumentCount, void * callbackState)
{
    JsValueRef stackInfo = JS_INVALID_REFERENCE;
    IfJsrtErrorRet(ChakraRTInterface::JsDiagGetStacktrace(&stackInfo));

    return stackInfo;
}

JsValueRef DebuggerCh::JsDiagGetStackProperties(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef properties = JS_INVALID_REFERENCE;
    int stackFrameIndex;

    if(argumentCount > 1)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[1], &stackFrameIndex));
        IfJsrtErrorRet(ChakraRTInterface::JsDiagGetStackProperties(stackFrameIndex, &properties));
    }

    return properties;
}

JsValueRef DebuggerCh::JsDiagLookupHandles(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef properties = JS_INVALID_REFERENCE;

    if(argumentCount > 1)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsDiagLookupHandles(arguments[1], &properties));
    }

    return properties;
}

JsValueRef DebuggerCh::JsEvaluateScript(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef result = JS_INVALID_REFERENCE;

    if(argumentCount > 1)
    {
        JsValueRef scriptRef;
        const wchar_t *script;
        size_t length;
        IfJsrtErrorRet(ChakraRTInterface::JsConvertValueToString(arguments[1], &scriptRef));
        IfJsrtErrorRet(ChakraRTInterface::JsStringToPointer(scriptRef, &script, &length));

        IfJsrtErrorRet(JsRunScript(script, JS_SOURCE_CONTEXT_NONE, L"", &result));
    }

    return result;
}

JsValueRef DebuggerCh::JsDiagEvaluate(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    int stackFrameIndex;
    JsValueRef result = JS_INVALID_REFERENCE;

    if(argumentCount > 2)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(arguments[1], &stackFrameIndex));

        JsValueRef strRef = JS_INVALID_REFERENCE;
        LPCWSTR str = nullptr;
        size_t length;
        IfJsrtErrorRet(ChakraRTInterface::JsConvertValueToString(arguments[2], &strRef));
        IfJsrtErrorRet(ChakraRTInterface::JsStringToPointer(strRef, &str, &length));

        IfJsrtErrorRet(ChakraRTInterface::JsDiagEvaluate(str, stackFrameIndex, &result));
    }

    return result;
}

JsValueRef DebuggerCh::JsDiagGetBreakpoints(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef properties = JS_INVALID_REFERENCE;
    IfJsrtErrorRet(ChakraRTInterface::JsDiagGetBreakpoints(&properties));

    return properties;
}

JsValueRef DebuggerCh::JsDiagGetProperties(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsValueRef properties = JS_INVALID_REFERENCE;

    if(argumentCount > 1)
    {
        IfJsrtErrorRet(ChakraRTInterface::JsDiagGetProperties(arguments[1], &properties));
    }

    return properties;
}

JsValueRef DebuggerCh::JsDiagRemoveBreakpoint(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    int bpId;
    if(argumentCount > 1)
    {
        JsValueRef numberValue;
        IfJsrtErrorRet(ChakraRTInterface::JsConvertValueToNumber(arguments[1], &numberValue));
        IfJsrtErrorRet(ChakraRTInterface::JsNumberToInt(numberValue, &bpId));

        IfJsrtErrorRet(ChakraRTInterface::JsDiagRemoveBreakpoint(bpId));
    }

    return JS_INVALID_REFERENCE;
}

JsValueRef DebuggerCh::JsDiagSetBreakOnException(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return JS_INVALID_REFERENCE;
}

JsValueRef DebuggerCh::JsDiagGetBreakOnException(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return JS_INVALID_REFERENCE;
}

JsValueRef DebuggerCh::SendDelayedRespose(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return JS_INVALID_REFERENCE;
}

DebuggerCh* DebuggerCh::GetDebugger()
{
    return DebuggerCh::debugger;
}

void DebuggerCh::CloseDebugger()
{
    if(DebuggerCh::debugger != nullptr)
    {
        delete DebuggerCh::debugger;
        DebuggerCh::debugger = nullptr;
    }
}

void DebuggerCh::StartDebugging(JsRuntimeHandle runtime, LPCWSTR ipAddr, unsigned short port)
{
    //Setup the debugger
    DebuggerCh::debugger = new DebuggerCh(ipAddr, port);
    DebuggerCh::debugger->Initialize(runtime);

    JsErrorCode errorCode = ChakraRTInterface::JsDiagStartDebugging(runtime, DebuggerCh::JsDiagDebugEventHandler, DebuggerCh::debugger);
    if(errorCode != JsNoError)
    {
        return;
    }

    int rc;
    WSADATA wsd;
    struct sockaddr_in saddr;

    //init Winsock
    rc = WSAStartup(MAKEWORD(2, 2), &wsd);
    if(rc != 0)
    {
        wprintf(L"Unable to load Winsock: %d\n", rc);
        exit(1);
    }

    //create socket to listen on for incomming requests
    DebuggerCh::debugger->m_dbgConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(DebuggerCh::debugger->m_dbgConnectSocket == INVALID_SOCKET)
    {
        wprintf(L"socket failed with error: %d\n", GetLastError());
        exit(1);
    }

    //bind to the socket 
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(DebuggerCh::debugger->m_port);
    InetPton(AF_INET, DebuggerCh::debugger->m_ipAddr, &saddr.sin_addr);

    rc = bind(DebuggerCh::debugger->m_dbgConnectSocket, (SOCKADDR*)&saddr, sizeof(saddr));
    if(rc == SOCKET_ERROR)
    {
        wprintf(L"connect failed with error: %d\n", WSAGetLastError());
        exit(1);
    }

    // Listen for incoming connection requests on the created socket
    if(listen(DebuggerCh::debugger->m_dbgConnectSocket, 1) == SOCKET_ERROR)
    {
        wprintf(L"listen failed with error: %ld\n", WSAGetLastError());
        exit(1);
    }
}

bool DebuggerCh::HandleDebugEvent(JsDiagDebugEvent debugEvent, JsValueRef eventData)
{
    bool shouldContinue = true;
    bool isMsgToProcess = !this->IsEmpty();

    do {
        while(!this->m_isProcessingDebuggerMsg && isMsgToProcess)
        {
            this->ProcessDebuggerMessage();
            isMsgToProcess = !this->IsEmpty();
        }

        if(eventData != nullptr)
        {
            this->ProcessJsrtDebugEvent(debugEvent, eventData);
            eventData = nullptr;
        }

        shouldContinue = this->ShouldContinue();

        isMsgToProcess = !this->IsEmpty();

        while(!shouldContinue && !isMsgToProcess && !this->m_isProcessingDebuggerMsg)
        {
            this->WaitForMessage();
            isMsgToProcess = !this->IsEmpty();
        }

        isMsgToProcess = !this->IsEmpty();

    } while(isMsgToProcess && !this->m_isProcessingDebuggerMsg && !shouldContinue);

    return true;
}

