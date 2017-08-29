/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "src/inspector/v8-runtime-agent-impl.h"

#include "src/debug/debug-interface.h"
#include "src/inspector/injected-script.h"
#include "src/inspector/inspected-context.h"
#include "src/inspector/protocol/Protocol.h"
#include "src/inspector/remote-object-id.h"
#include "src/inspector/string-util.h"
#include "src/inspector/v8-console-message.h"
#include "src/inspector/v8-debugger-agent-impl.h"
#include "src/inspector/v8-debugger.h"
#include "src/inspector/v8-inspector-impl.h"
#include "src/inspector/v8-inspector-session-impl.h"
#include "src/inspector/v8-stack-trace-impl.h"
#include "src/inspector/v8-value-utils.h"
#include "src/tracing/trace-event.h"

#include "include/v8-inspector.h"

namespace v8_inspector {

namespace V8RuntimeAgentImplState {
static const char customObjectFormatterEnabled[] =
    "customObjectFormatterEnabled";
static const char runtimeEnabled[] = "runtimeEnabled";
};

using protocol::Runtime::RemoteObject;

namespace {

template <typename ProtocolCallback>
class EvaluateCallbackWrapper : public EvaluateCallback {
 public:
  static std::unique_ptr<EvaluateCallback> wrap(
      std::unique_ptr<ProtocolCallback> callback) {
    return std::unique_ptr<EvaluateCallback>(
        new EvaluateCallbackWrapper(std::move(callback)));
  }
  void sendSuccess(std::unique_ptr<protocol::Runtime::RemoteObject> result,
                   protocol::Maybe<protocol::Runtime::ExceptionDetails>
                       exceptionDetails) override {
    return m_callback->sendSuccess(std::move(result),
                                   std::move(exceptionDetails));
  }
  void sendFailure(const protocol::DispatchResponse& response) override {
    return m_callback->sendFailure(response);
  }

 private:
  explicit EvaluateCallbackWrapper(std::unique_ptr<ProtocolCallback> callback)
      : m_callback(std::move(callback)) {}

  std::unique_ptr<ProtocolCallback> m_callback;
};

template <typename ProtocolCallback>
bool wrapEvaluateResultAsync(InjectedScript* injectedScript,
                             v8::MaybeLocal<v8::Value> maybeResultValue,
                             const v8::TryCatch& tryCatch,
                             const String16& objectGroup, bool returnByValue,
                             bool generatePreview, ProtocolCallback* callback) {
  std::unique_ptr<RemoteObject> result;
  Maybe<protocol::Runtime::ExceptionDetails> exceptionDetails;

  Response response = injectedScript->wrapEvaluateResult(
      maybeResultValue, tryCatch, objectGroup, returnByValue, generatePreview,
      &result, &exceptionDetails);
  if (response.isSuccess()) {
    callback->sendSuccess(std::move(result), std::move(exceptionDetails));
    return true;
  }
  callback->sendFailure(response);
  return false;
}

void innerCallFunctionOn(
    V8InspectorSessionImpl* session, InjectedScript::Scope& scope,
    v8::Local<v8::Value> recv, const String16& expression,
    Maybe<protocol::Array<protocol::Runtime::CallArgument>> optionalArguments,
    bool silent, bool returnByValue, bool generatePreview, bool userGesture,
    bool awaitPromise, const String16& objectGroup,
    std::unique_ptr<V8RuntimeAgentImpl::CallFunctionOnCallback> callback) {
  V8InspectorImpl* inspector = session->inspector();

  std::unique_ptr<v8::Local<v8::Value>[]> argv = nullptr;
  int argc = 0;
  if (optionalArguments.isJust()) {
    protocol::Array<protocol::Runtime::CallArgument>* arguments =
        optionalArguments.fromJust();
    argc = static_cast<int>(arguments->length());
    argv.reset(new v8::Local<v8::Value>[argc]);
    for (int i = 0; i < argc; ++i) {
      v8::Local<v8::Value> argumentValue;
      Response response = scope.injectedScript()->resolveCallArgument(
          arguments->get(i), &argumentValue);
      if (!response.isSuccess()) {
        callback->sendFailure(response);
        return;
      }
      argv[i] = argumentValue;
    }
  }

  if (silent) scope.ignoreExceptionsAndMuteConsole();
  if (userGesture) scope.pretendUserGesture();

  v8::MaybeLocal<v8::Value> maybeFunctionValue;
  v8::Local<v8::Script> functionScript;
  if (inspector
          ->compileScript(scope.context(), "(" + expression + ")", String16())
          .ToLocal(&functionScript)) {
    v8::MicrotasksScope microtasksScope(inspector->isolate(),
                                        v8::MicrotasksScope::kRunMicrotasks);
    maybeFunctionValue = functionScript->Run(scope.context());
  }
  // Re-initialize after running client's code, as it could have destroyed
  // context or session.
  Response response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (scope.tryCatch().HasCaught()) {
    wrapEvaluateResultAsync(scope.injectedScript(), maybeFunctionValue,
                            scope.tryCatch(), objectGroup, false, false,
                            callback.get());
    return;
  }

  v8::Local<v8::Value> functionValue;
  if (!maybeFunctionValue.ToLocal(&functionValue) ||
      !functionValue->IsFunction()) {
    callback->sendFailure(
        Response::Error("Given expression does not evaluate to a function"));
    return;
  }

  v8::MaybeLocal<v8::Value> maybeResultValue;
  {
    v8::MicrotasksScope microtasksScope(inspector->isolate(),
                                        v8::MicrotasksScope::kRunMicrotasks);
    maybeResultValue = functionValue.As<v8::Function>()->Call(
        scope.context(), recv, argc, argv.get());
  }
  // Re-initialize after running client's code, as it could have destroyed
  // context or session.
  response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (!awaitPromise || scope.tryCatch().HasCaught()) {
    wrapEvaluateResultAsync(scope.injectedScript(), maybeResultValue,
                            scope.tryCatch(), objectGroup, returnByValue,
                            generatePreview, callback.get());
    return;
  }

  scope.injectedScript()->addPromiseCallback(
      session, maybeResultValue, objectGroup, returnByValue, generatePreview,
      EvaluateCallbackWrapper<V8RuntimeAgentImpl::CallFunctionOnCallback>::wrap(
          std::move(callback)));
}

Response ensureContext(V8InspectorImpl* inspector, int contextGroupId,
                       Maybe<int> executionContextId, int* contextId) {
  if (executionContextId.isJust()) {
    *contextId = executionContextId.fromJust();
  } else {
    v8::HandleScope handles(inspector->isolate());
    v8::Local<v8::Context> defaultContext =
        inspector->client()->ensureDefaultContextInGroup(contextGroupId);
    if (defaultContext.IsEmpty())
      return Response::Error("Cannot find default execution context");
    *contextId = InspectedContext::contextId(defaultContext);
  }
  return Response::OK();
}

}  // namespace

V8RuntimeAgentImpl::V8RuntimeAgentImpl(
    V8InspectorSessionImpl* session, protocol::FrontendChannel* FrontendChannel,
    protocol::DictionaryValue* state)
    : m_session(session),
      m_state(state),
      m_frontend(FrontendChannel),
      m_inspector(session->inspector()),
      m_enabled(false) {}

V8RuntimeAgentImpl::~V8RuntimeAgentImpl() {}

void V8RuntimeAgentImpl::evaluate(
    const String16& expression, Maybe<String16> objectGroup,
    Maybe<bool> includeCommandLineAPI, Maybe<bool> silent,
    Maybe<int> executionContextId, Maybe<bool> returnByValue,
    Maybe<bool> generatePreview, Maybe<bool> userGesture,
    Maybe<bool> awaitPromise, std::unique_ptr<EvaluateCallback> callback) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "EvaluateScript");
  int contextId = 0;
  Response response = ensureContext(m_inspector, m_session->contextGroupId(),
                                    std::move(executionContextId), &contextId);
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  InjectedScript::ContextScope scope(m_session, contextId);
  response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (silent.fromMaybe(false)) scope.ignoreExceptionsAndMuteConsole();
  if (userGesture.fromMaybe(false)) scope.pretendUserGesture();

  if (includeCommandLineAPI.fromMaybe(false)) scope.installCommandLineAPI();

  bool evalIsDisabled = !scope.context()->IsCodeGenerationFromStringsAllowed();
  // Temporarily enable allow evals for inspector.
  if (evalIsDisabled) scope.context()->AllowCodeGenerationFromStrings(true);

  v8::MaybeLocal<v8::Value> maybeResultValue;
  v8::Local<v8::Script> script;
  if (m_inspector->compileScript(scope.context(), expression, String16())
          .ToLocal(&script)) {
    v8::MicrotasksScope microtasksScope(m_inspector->isolate(),
                                        v8::MicrotasksScope::kRunMicrotasks);
    maybeResultValue = script->Run(scope.context());
  }

  if (evalIsDisabled) scope.context()->AllowCodeGenerationFromStrings(false);

  // Re-initialize after running client's code, as it could have destroyed
  // context or session.
  response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (!awaitPromise.fromMaybe(false) || scope.tryCatch().HasCaught()) {
    wrapEvaluateResultAsync(scope.injectedScript(), maybeResultValue,
                            scope.tryCatch(), objectGroup.fromMaybe(""),
                            returnByValue.fromMaybe(false),
                            generatePreview.fromMaybe(false), callback.get());
    return;
  }
  scope.injectedScript()->addPromiseCallback(
      m_session, maybeResultValue, objectGroup.fromMaybe(""),
      returnByValue.fromMaybe(false), generatePreview.fromMaybe(false),
      EvaluateCallbackWrapper<EvaluateCallback>::wrap(std::move(callback)));
}

void V8RuntimeAgentImpl::awaitPromise(
    const String16& promiseObjectId, Maybe<bool> returnByValue,
    Maybe<bool> generatePreview,
    std::unique_ptr<AwaitPromiseCallback> callback) {
  InjectedScript::ObjectScope scope(m_session, promiseObjectId);
  Response response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }
  if (!scope.object()->IsPromise()) {
    callback->sendFailure(
        Response::Error("Could not find promise with given id"));
    return;
  }
  scope.injectedScript()->addPromiseCallback(
      m_session, scope.object(), scope.objectGroupName(),
      returnByValue.fromMaybe(false), generatePreview.fromMaybe(false),
      EvaluateCallbackWrapper<AwaitPromiseCallback>::wrap(std::move(callback)));
}

void V8RuntimeAgentImpl::callFunctionOn(
    Maybe<String16> objectId, const String16& expression,
    Maybe<protocol::Array<protocol::Runtime::CallArgument>> optionalArguments,
    Maybe<bool> silent, Maybe<bool> returnByValue, Maybe<bool> generatePreview,
    Maybe<bool> userGesture, Maybe<bool> awaitPromise,
    Maybe<int> executionContextId, Maybe<String16> objectGroup,
    std::unique_ptr<CallFunctionOnCallback> callback) {
  if (objectId.isJust() && executionContextId.isJust()) {
    callback->sendFailure(Response::Error(
        "ObjectId must not be specified together with executionContextId"));
    return;
  }
  if (!objectId.isJust() && !executionContextId.isJust()) {
    callback->sendFailure(Response::Error(
        "Either ObjectId or executionContextId must be specified"));
    return;
  }
  if (objectId.isJust()) {
    InjectedScript::ObjectScope scope(m_session, objectId.fromJust());
    Response response = scope.initialize();
    if (!response.isSuccess()) {
      callback->sendFailure(response);
      return;
    }
    innerCallFunctionOn(
        m_session, scope, scope.object(), expression,
        std::move(optionalArguments), silent.fromMaybe(false),
        returnByValue.fromMaybe(false), generatePreview.fromMaybe(false),
        userGesture.fromMaybe(false), awaitPromise.fromMaybe(false),
        objectGroup.isJust() ? objectGroup.fromMaybe(String16())
                             : scope.objectGroupName(),
        std::move(callback));
  } else {
    int contextId = 0;
    Response response =
        ensureContext(m_inspector, m_session->contextGroupId(),
                      std::move(executionContextId.fromJust()), &contextId);
    if (!response.isSuccess()) {
      callback->sendFailure(response);
      return;
    }
    InjectedScript::ContextScope scope(m_session, contextId);
    response = scope.initialize();
    if (!response.isSuccess()) {
      callback->sendFailure(response);
      return;
    }
    innerCallFunctionOn(
        m_session, scope, scope.context()->Global(), expression,
        std::move(optionalArguments), silent.fromMaybe(false),
        returnByValue.fromMaybe(false), generatePreview.fromMaybe(false),
        userGesture.fromMaybe(false), awaitPromise.fromMaybe(false),
        objectGroup.fromMaybe(""), std::move(callback));
  }
}

Response V8RuntimeAgentImpl::getProperties(
    const String16& objectId, Maybe<bool> ownProperties,
    Maybe<bool> accessorPropertiesOnly, Maybe<bool> generatePreview,
    std::unique_ptr<protocol::Array<protocol::Runtime::PropertyDescriptor>>*
        result,
    Maybe<protocol::Array<protocol::Runtime::InternalPropertyDescriptor>>*
        internalProperties,
    Maybe<protocol::Runtime::ExceptionDetails>* exceptionDetails) {
  using protocol::Runtime::InternalPropertyDescriptor;

  InjectedScript::ObjectScope scope(m_session, objectId);
  Response response = scope.initialize();
  if (!response.isSuccess()) return response;

  scope.ignoreExceptionsAndMuteConsole();
  v8::MicrotasksScope microtasks_scope(m_inspector->isolate(),
                                       v8::MicrotasksScope::kRunMicrotasks);
  if (!scope.object()->IsObject())
    return Response::Error("Value with given id is not an object");

  v8::Local<v8::Object> object = scope.object().As<v8::Object>();
  response = scope.injectedScript()->getProperties(
      object, scope.objectGroupName(), ownProperties.fromMaybe(false),
      accessorPropertiesOnly.fromMaybe(false), generatePreview.fromMaybe(false),
      result, exceptionDetails);
  if (!response.isSuccess()) return response;
  if (exceptionDetails->isJust() || accessorPropertiesOnly.fromMaybe(false))
    return Response::OK();
  v8::Local<v8::Array> propertiesArray;
  if (!m_inspector->debugger()
           ->internalProperties(scope.context(), scope.object())
           .ToLocal(&propertiesArray)) {
    return Response::InternalError();
  }
  std::unique_ptr<protocol::Array<InternalPropertyDescriptor>>
      propertiesProtocolArray =
          protocol::Array<InternalPropertyDescriptor>::create();
  for (uint32_t i = 0; i < propertiesArray->Length(); i += 2) {
    v8::Local<v8::Value> name;
    if (!propertiesArray->Get(scope.context(), i).ToLocal(&name) ||
        !name->IsString()) {
      return Response::InternalError();
    }
    v8::Local<v8::Value> value;
    if (!propertiesArray->Get(scope.context(), i + 1).ToLocal(&value))
      return Response::InternalError();
    std::unique_ptr<RemoteObject> wrappedValue;
    protocol::Response response = scope.injectedScript()->wrapObject(
        value, scope.objectGroupName(), false, false, &wrappedValue);
    if (!response.isSuccess()) return response;
    propertiesProtocolArray->addItem(
        InternalPropertyDescriptor::create()
            .setName(toProtocolString(name.As<v8::String>()))
            .setValue(std::move(wrappedValue))
            .build());
  }
  if (propertiesProtocolArray->length())
    *internalProperties = std::move(propertiesProtocolArray);
  return Response::OK();
}

Response V8RuntimeAgentImpl::releaseObject(const String16& objectId) {
  InjectedScript::ObjectScope scope(m_session, objectId);
  Response response = scope.initialize();
  if (!response.isSuccess()) return response;
  scope.injectedScript()->releaseObject(objectId);
  return Response::OK();
}

Response V8RuntimeAgentImpl::releaseObjectGroup(const String16& objectGroup) {
  m_session->releaseObjectGroup(objectGroup);
  return Response::OK();
}

Response V8RuntimeAgentImpl::runIfWaitingForDebugger() {
  m_inspector->client()->runIfWaitingForDebugger(m_session->contextGroupId());
  return Response::OK();
}

Response V8RuntimeAgentImpl::setCustomObjectFormatterEnabled(bool enabled) {
  m_state->setBoolean(V8RuntimeAgentImplState::customObjectFormatterEnabled,
                      enabled);
  if (!m_enabled) return Response::Error("Runtime agent is not enabled");
  m_session->setCustomObjectFormatterEnabled(enabled);
  return Response::OK();
}

Response V8RuntimeAgentImpl::discardConsoleEntries() {
  V8ConsoleMessageStorage* storage =
      m_inspector->ensureConsoleMessageStorage(m_session->contextGroupId());
  storage->clear();
  return Response::OK();
}

Response V8RuntimeAgentImpl::compileScript(
    const String16& expression, const String16& sourceURL, bool persistScript,
    Maybe<int> executionContextId, Maybe<String16>* scriptId,
    Maybe<protocol::Runtime::ExceptionDetails>* exceptionDetails) {
  if (!m_enabled) return Response::Error("Runtime agent is not enabled");

  int contextId = 0;
  Response response = ensureContext(m_inspector, m_session->contextGroupId(),
                                    std::move(executionContextId), &contextId);
  if (!response.isSuccess()) return response;
  InjectedScript::ContextScope scope(m_session, contextId);
  response = scope.initialize();
  if (!response.isSuccess()) return response;

  if (!persistScript) m_inspector->debugger()->muteScriptParsedEvents();
  v8::Local<v8::Script> script;
  bool isOk = m_inspector->compileScript(scope.context(), expression, sourceURL)
                  .ToLocal(&script);
  if (!persistScript) m_inspector->debugger()->unmuteScriptParsedEvents();
  if (!isOk) {
    if (scope.tryCatch().HasCaught()) {
      response = scope.injectedScript()->createExceptionDetails(
          scope.tryCatch(), String16(), false, exceptionDetails);
      if (!response.isSuccess()) return response;
      return Response::OK();
    } else {
      return Response::Error("Script compilation failed");
    }
  }

  if (!persistScript) return Response::OK();

  String16 scriptValueId =
      String16::fromInteger(script->GetUnboundScript()->GetId());
  std::unique_ptr<v8::Global<v8::Script>> global(
      new v8::Global<v8::Script>(m_inspector->isolate(), script));
  m_compiledScripts[scriptValueId] = std::move(global);
  *scriptId = scriptValueId;
  return Response::OK();
}

void V8RuntimeAgentImpl::runScript(
    const String16& scriptId, Maybe<int> executionContextId,
    Maybe<String16> objectGroup, Maybe<bool> silent,
    Maybe<bool> includeCommandLineAPI, Maybe<bool> returnByValue,
    Maybe<bool> generatePreview, Maybe<bool> awaitPromise,
    std::unique_ptr<RunScriptCallback> callback) {
  if (!m_enabled) {
    callback->sendFailure(Response::Error("Runtime agent is not enabled"));
    return;
  }

  auto it = m_compiledScripts.find(scriptId);
  if (it == m_compiledScripts.end()) {
    callback->sendFailure(Response::Error("No script with given id"));
    return;
  }

  int contextId = 0;
  Response response = ensureContext(m_inspector, m_session->contextGroupId(),
                                    std::move(executionContextId), &contextId);
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  InjectedScript::ContextScope scope(m_session, contextId);
  response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (silent.fromMaybe(false)) scope.ignoreExceptionsAndMuteConsole();

  std::unique_ptr<v8::Global<v8::Script>> scriptWrapper = std::move(it->second);
  m_compiledScripts.erase(it);
  v8::Local<v8::Script> script = scriptWrapper->Get(m_inspector->isolate());
  if (script.IsEmpty()) {
    callback->sendFailure(Response::Error("Script execution failed"));
    return;
  }

  if (includeCommandLineAPI.fromMaybe(false)) scope.installCommandLineAPI();

  v8::MaybeLocal<v8::Value> maybeResultValue;
  {
    v8::MicrotasksScope microtasksScope(m_inspector->isolate(),
                                        v8::MicrotasksScope::kRunMicrotasks);
    maybeResultValue = script->Run(scope.context());
  }

  // Re-initialize after running client's code, as it could have destroyed
  // context or session.
  response = scope.initialize();
  if (!response.isSuccess()) {
    callback->sendFailure(response);
    return;
  }

  if (!awaitPromise.fromMaybe(false) || scope.tryCatch().HasCaught()) {
    wrapEvaluateResultAsync(scope.injectedScript(), maybeResultValue,
                            scope.tryCatch(), objectGroup.fromMaybe(""),
                            returnByValue.fromMaybe(false),
                            generatePreview.fromMaybe(false), callback.get());
    return;
  }
  scope.injectedScript()->addPromiseCallback(
      m_session, maybeResultValue.ToLocalChecked(),
      objectGroup.fromMaybe(""), returnByValue.fromMaybe(false),
      generatePreview.fromMaybe(false),
      EvaluateCallbackWrapper<RunScriptCallback>::wrap(std::move(callback)));
}

Response V8RuntimeAgentImpl::queryObjects(
    const String16& prototypeObjectId,
    std::unique_ptr<protocol::Runtime::RemoteObject>* objects) {
  InjectedScript::ObjectScope scope(m_session, prototypeObjectId);
  Response response = scope.initialize();
  if (!response.isSuccess()) return response;
  if (!scope.object()->IsObject()) {
    return Response::Error("Prototype should be instance of Object");
  }
  v8::Local<v8::Array> resultArray = m_inspector->debugger()->queryObjects(
      scope.context(), v8::Local<v8::Object>::Cast(scope.object()));
  return scope.injectedScript()->wrapObject(
      resultArray, scope.objectGroupName(), false, false, objects);
}

void V8RuntimeAgentImpl::restore() {
  if (!m_state->booleanProperty(V8RuntimeAgentImplState::runtimeEnabled, false))
    return;
  m_frontend.executionContextsCleared();
  enable();
  if (m_state->booleanProperty(
          V8RuntimeAgentImplState::customObjectFormatterEnabled, false))
    m_session->setCustomObjectFormatterEnabled(true);
}

Response V8RuntimeAgentImpl::enable() {
  if (m_enabled) return Response::OK();
  m_inspector->client()->beginEnsureAllContextsInGroup(
      m_session->contextGroupId());
  m_enabled = true;
  m_state->setBoolean(V8RuntimeAgentImplState::runtimeEnabled, true);
  m_inspector->enableStackCapturingIfNeeded();
  m_session->reportAllContexts(this);
  V8ConsoleMessageStorage* storage =
      m_inspector->ensureConsoleMessageStorage(m_session->contextGroupId());
  for (const auto& message : storage->messages()) {
    if (!reportMessage(message.get(), false)) break;
  }
  return Response::OK();
}

Response V8RuntimeAgentImpl::disable() {
  if (!m_enabled) return Response::OK();
  m_enabled = false;
  m_state->setBoolean(V8RuntimeAgentImplState::runtimeEnabled, false);
  m_inspector->disableStackCapturingIfNeeded();
  m_session->setCustomObjectFormatterEnabled(false);
  reset();
  m_inspector->client()->endEnsureAllContextsInGroup(
      m_session->contextGroupId());
  return Response::OK();
}

void V8RuntimeAgentImpl::reset() {
  m_compiledScripts.clear();
  if (m_enabled) {
    int sessionId = m_session->sessionId();
    m_inspector->forEachContext(m_session->contextGroupId(),
                                [&sessionId](InspectedContext* context) {
                                  context->setReported(sessionId, false);
                                });
    m_frontend.executionContextsCleared();
  }
}

void V8RuntimeAgentImpl::reportExecutionContextCreated(
    InspectedContext* context) {
  if (!m_enabled) return;
  context->setReported(m_session->sessionId(), true);
  std::unique_ptr<protocol::Runtime::ExecutionContextDescription> description =
      protocol::Runtime::ExecutionContextDescription::create()
          .setId(context->contextId())
          .setName(context->humanReadableName())
          .setOrigin(context->origin())
          .build();
  if (!context->auxData().isEmpty())
    description->setAuxData(protocol::DictionaryValue::cast(
        protocol::StringUtil::parseJSON(context->auxData())));
  m_frontend.executionContextCreated(std::move(description));
}

void V8RuntimeAgentImpl::reportExecutionContextDestroyed(
    InspectedContext* context) {
  if (m_enabled && context->isReported(m_session->sessionId())) {
    context->setReported(m_session->sessionId(), false);
    m_frontend.executionContextDestroyed(context->contextId());
  }
}

void V8RuntimeAgentImpl::inspect(
    std::unique_ptr<protocol::Runtime::RemoteObject> objectToInspect,
    std::unique_ptr<protocol::DictionaryValue> hints) {
  if (m_enabled)
    m_frontend.inspectRequested(std::move(objectToInspect), std::move(hints));
}

void V8RuntimeAgentImpl::messageAdded(V8ConsoleMessage* message) {
  if (m_enabled) reportMessage(message, true);
}

bool V8RuntimeAgentImpl::reportMessage(V8ConsoleMessage* message,
                                       bool generatePreview) {
  message->reportToFrontend(&m_frontend, m_session, generatePreview);
  m_frontend.flush();
  return m_inspector->hasConsoleMessageStorage(m_session->contextGroupId());
}

}  // namespace v8_inspector
