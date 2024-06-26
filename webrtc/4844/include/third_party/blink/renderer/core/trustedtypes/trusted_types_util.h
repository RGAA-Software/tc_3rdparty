// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_TRUSTEDTYPES_TRUSTED_TYPES_UTIL_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_TRUSTEDTYPES_TRUSTED_TYPES_UTIL_H_

#include "third_party/blink/renderer/bindings/core/v8/v8_typedefs.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/script/script_element_base.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class ExceptionState;
class ExecutionContext;
class V8UnionStringOrTrustedScript;
class V8UnionStringTreatNullAsEmptyStringOrTrustedScript;

enum class SpecificTrustedType {
  kNone,
  kHTML,
  kScript,
  kScriptURL,
};

// Perform Trusted Type checks, with the IDL union types as input. All of these
// will call String& versions below to do the heavy lifting.
[[nodiscard]] CORE_EXPORT String
TrustedTypesCheckFor(SpecificTrustedType type,
                     const V8TrustedType* trusted,
                     const ExecutionContext* execution_context,
                     ExceptionState& exception_state);
[[nodiscard]] CORE_EXPORT String
TrustedTypesCheckForScript(const V8UnionStringOrTrustedScript* value,
                           const ExecutionContext* execution_context,
                           ExceptionState& exception_state);
[[nodiscard]] CORE_EXPORT String TrustedTypesCheckForScript(
    const V8UnionStringTreatNullAsEmptyStringOrTrustedScript* value,
    const ExecutionContext* execution_context,
    ExceptionState& exception_state);

// Perform Trusted Type checks, for a dynamically or statically determined
// type.
// Returns the effective value (which may have been modified by the "default"
// policy.
[[nodiscard]] String TrustedTypesCheckFor(SpecificTrustedType,
                                          String,
                                          const ExecutionContext*,
                                          ExceptionState&);
[[nodiscard]] CORE_EXPORT String
TrustedTypesCheckForHTML(String, const ExecutionContext*, ExceptionState&);
[[nodiscard]] CORE_EXPORT String
TrustedTypesCheckForScript(String, const ExecutionContext*, ExceptionState&);
[[nodiscard]] CORE_EXPORT String
TrustedTypesCheckForScriptURL(String, const ExecutionContext*, ExceptionState&);

// Functionally equivalent to TrustedTypesCheckForScript(const String&, ...),
// but with setup & error handling suitable for the asynchronous execution
// cases.
String TrustedTypesCheckForJavascriptURLinNavigation(String, ExecutionContext*);
CORE_EXPORT String GetStringForScriptExecution(String,
                                               ScriptElementBase::Type,
                                               ExecutionContext*);

// Functionally equivalent to TrustedTypesCheckForHTML(const String&, ...),
// but with separate enable flag and use counter, to ensure this won't break
// existing sites before enabling it in full.
CORE_EXPORT String TrustedTypesCheckForExecCommand(String,
                                                   const ExecutionContext*,
                                                   ExceptionState&)
    WARN_UNUSED_RESULT;

// Determine whether a Trusted Types check is needed in this execution context.
//
// Note: All methods above handle this internally and will return success if a
// check is not required. However, in cases where not-required doesn't
// immediately imply "okay" this method can be used.
// Example: To determine whether 'eval' may pass, one needs to also take CSP
// into account.
CORE_EXPORT bool RequireTrustedTypesCheck(const ExecutionContext*);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TRUSTEDTYPES_TRUSTED_TYPES_UTIL_H_
