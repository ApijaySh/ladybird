/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamDefaultReaderPrototype.h>
#include <LibWeb/Fetch/Infrastructure/IncrementalReadLoopReadRequest.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStreamDefaultReader);
GC_DEFINE_ALLOCATOR(ReadLoopReadRequest);

void ReadLoopReadRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_reader);
    visitor.visit(m_success_steps);
    visitor.visit(m_failure_steps);
    visitor.visit(m_chunk_steps);
}

// https://streams.spec.whatwg.org/#default-reader-constructor
WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> ReadableStreamDefaultReader::construct_impl(JS::Realm& realm, GC::Ref<ReadableStream> stream)
{
    auto reader = realm.create<ReadableStreamDefaultReader>(realm);

    // 1. Perform ? SetUpReadableStreamDefaultReader(this, stream);
    TRY(set_up_readable_stream_default_reader(reader, *stream));

    return reader;
}

ReadableStreamDefaultReader::ReadableStreamDefaultReader(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
    , ReadableStreamGenericReaderMixin(realm)
{
}

void ReadableStreamDefaultReader::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStreamDefaultReader);
    Base::initialize(realm);
}

void ReadableStreamDefaultReader::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    ReadableStreamGenericReaderMixin::visit_edges(visitor);
    for (auto& request : m_read_requests)
        visitor.visit(request);
    visitor.visit(m_readable_stream_pipe_to_operation);
}

// https://streams.spec.whatwg.org/#read-loop
ReadLoopReadRequest::ReadLoopReadRequest(JS::VM& vm, JS::Realm& realm, ReadableStreamDefaultReader& reader, GC::Ref<SuccessSteps> success_steps, GC::Ref<FailureSteps> failure_steps, GC::Ptr<ChunkSteps> chunk_steps)
    : m_vm(vm)
    , m_realm(realm)
    , m_reader(reader)
    , m_success_steps(success_steps)
    , m_failure_steps(failure_steps)
    , m_chunk_steps(chunk_steps)
{
}

// chunk steps, given chunk
void ReadLoopReadRequest::on_chunk(JS::Value chunk)
{
    // 1. If chunk is not a Uint8Array object, call failureSteps with a TypeError and abort these steps.
    if (!chunk.is_object() || !is<JS::Uint8Array>(chunk.as_object())) {
        m_failure_steps->function()(JS::TypeError::create(m_realm, "Chunk data is not Uint8Array"sv));
        return;
    }

    auto const& array = static_cast<JS::Uint8Array const&>(chunk.as_object());
    auto const& buffer = array.viewed_array_buffer()->buffer();

    // 2. Append the bytes represented by chunk to bytes.
    m_bytes.append(buffer);

    if (m_chunk_steps) {
        // FIXME: Can we move the buffer out of the `chunk`? Unclear if that is safe.
        m_chunk_steps->function()(MUST(ByteBuffer::copy(buffer)));
    }

    // FIXME: As the spec suggests, implement this non-recursively - instead of directly. It is not too big of a deal currently
    //        as we enqueue the entire blob buffer in one go, meaning that we only recurse a single time. Once we begin queuing
    //        up more than one chunk at a time, we may run into stack overflow problems.
    //
    // 3. Read-loop given reader, bytes, successSteps, and failureSteps.
    readable_stream_default_reader_read(m_reader, *this);
}

// close steps
void ReadLoopReadRequest::on_close()
{
    // 1. Call successSteps with bytes.
    m_success_steps->function()(move(m_bytes));
}

// error steps, given e
void ReadLoopReadRequest::on_error(JS::Value error)
{
    // 1. Call failureSteps with e.
    m_failure_steps->function()(error);
}

class DefaultReaderReadRequest final : public ReadRequest {
    GC_CELL(DefaultReaderReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(DefaultReaderReadRequest);

public:
    DefaultReaderReadRequest(JS::Realm& realm, WebIDL::Promise& promise)
        : m_realm(realm)
        , m_promise(promise)
    {
    }

    virtual void on_chunk(JS::Value chunk) override
    {
        WebIDL::resolve_promise(m_realm, m_promise, JS::create_iterator_result_object(m_realm->vm(), chunk, false));
    }

    virtual void on_close() override
    {
        WebIDL::resolve_promise(m_realm, m_promise, JS::create_iterator_result_object(m_realm->vm(), JS::js_undefined(), true));
    }

    virtual void on_error(JS::Value error) override
    {
        WebIDL::reject_promise(m_realm, m_promise, error);
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_promise);
    }

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<WebIDL::Promise> m_promise;
};

GC_DEFINE_ALLOCATOR(DefaultReaderReadRequest);

// https://streams.spec.whatwg.org/#default-reader-read
GC::Ref<WebIDL::Promise> ReadableStreamDefaultReader::read()
{
    auto& realm = this->realm();

    // 1. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Cannot read from an empty stream"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 2. Let promise be a new promise.
    auto promise_capability = WebIDL::create_promise(realm);

    // 3. Let readRequest be a new read request with the following items:
    //    chunk steps, given chunk
    //        Resolve promise with «[ "value" → chunk, "done" → false ]».
    //    close steps
    //        Resolve promise with «[ "value" → undefined, "done" → true ]».
    //    error steps, given e
    //        Reject promise with e.
    auto read_request = heap().allocate<DefaultReaderReadRequest>(realm, promise_capability);

    // 4. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
    readable_stream_default_reader_read(*this, read_request);

    // 5. Return promise.
    return promise_capability;
}

void ReadableStreamDefaultReader::read_a_chunk(Fetch::Infrastructure::IncrementalReadLoopReadRequest& read_request)
{
    // To read a chunk from a ReadableStreamDefaultReader reader, given a read request readRequest,
    // perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
    readable_stream_default_reader_read(*this, read_request);
}

// https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-all-bytes
void ReadableStreamDefaultReader::read_all_bytes(GC::Ref<ReadLoopReadRequest::SuccessSteps> success_steps, GC::Ref<ReadLoopReadRequest::FailureSteps> failure_steps)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let readRequest be a new read request with the following items:
    //    NOTE: items and steps in ReadLoopReadRequest.
    auto read_request = heap().allocate<ReadLoopReadRequest>(vm, realm, *this, success_steps, failure_steps);

    // 2. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
    readable_stream_default_reader_read(*this, read_request);
}

// FIXME: This function is a promise-based wrapper around "read all bytes". The spec changed this function to not use promises
//        in https://github.com/whatwg/streams/commit/f894acdd417926a2121710803cef593e15127964 - however, it seems that the
//        FileAPI blob specification has not been updated to match, see: https://github.com/w3c/FileAPI/issues/187.
GC::Ref<WebIDL::Promise> ReadableStreamDefaultReader::read_all_bytes_deprecated()
{
    auto& realm = this->realm();

    auto promise = WebIDL::create_promise(realm);

    auto success_steps = GC::create_function(realm.heap(), [promise, &realm](ByteBuffer bytes) {
        auto buffer = JS::ArrayBuffer::create(realm, move(bytes));
        WebIDL::resolve_promise(realm, promise, buffer);
    });

    auto failure_steps = GC::create_function(realm.heap(), [promise, &realm](JS::Value error) {
        WebIDL::reject_promise(realm, promise, error);
    });

    read_all_bytes(success_steps, failure_steps);

    return promise;
}

// https://streams.spec.whatwg.org/#default-reader-release-lock
void ReadableStreamDefaultReader::release_lock()
{
    // 1. If this.[[stream]] is undefined, return.
    if (!m_stream)
        return;

    // 2. Perform ! ReadableStreamDefaultReaderRelease(this).
    readable_stream_default_reader_release(*this);
}

}
