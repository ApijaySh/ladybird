/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

class WrappedFunction final : public FunctionObject {
    JS_OBJECT(WrappedFunction, FunctionObject);
    GC_DECLARE_ALLOCATOR(WrappedFunction);

public:
    static ThrowCompletionOr<GC::Ref<WrappedFunction>> create(Realm&, Realm& caller_realm, FunctionObject& target_function);

    virtual ~WrappedFunction() = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;

    virtual Realm* realm() const override { return m_realm; }

    FunctionObject const& wrapped_target_function() const { return m_wrapped_target_function; }
    FunctionObject& wrapped_target_function() { return m_wrapped_target_function; }

private:
    WrappedFunction(Realm&, FunctionObject&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // Internal Slots of Wrapped Function Exotic Objects, https://tc39.es/proposal-shadowrealm/#table-internal-slots-of-wrapped-function-exotic-objects
    GC::Ref<FunctionObject> m_wrapped_target_function; // [[WrappedTargetFunction]]
    GC::Ref<Realm> m_realm;                            // [[Realm]]
};

ThrowCompletionOr<Value> ordinary_wrapped_function_call(WrappedFunction const&, Value this_argument, ReadonlySpan<Value> arguments_list);
void prepare_for_wrapped_function_call(WrappedFunction const&, ExecutionContext& callee_context);

}
