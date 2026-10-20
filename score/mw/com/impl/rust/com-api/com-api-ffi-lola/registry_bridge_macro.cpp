/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

/// \file registry_bridge_macro.cpp
/// \brief C FFI wrapper implementations for COM-API that use registry-based type resolution
/// \details This file provides extern "C" functions that implement the COM-API FFI using the registry-based approach
/// defined in registry_bridge_macro.h. These functions are called by Rust code through FFI and provide safe,
/// C-compatible interfaces to the C++ COM-API implementation.
/// The actual logic of these functions relies on the type and interface registries to resolve the correct operations at
/// runtime. The functions bridge between:
/// - Rust side: String-based, safe wrapper APIs
/// - C++ side: Template-based, type-erased implementation

#include "score/mw/com/impl/rust/com-api/com-api-ffi-lola/registry_bridge_macro.h"
#include "score/mw/com/impl/configuration/quality_type.h"
#include "score/mw/com/impl/find_service_handler.h"
#include "score/mw/com/impl/methods/proxy_method_binding.h"
#include "score/mw/com/impl/methods/skeleton_method_binding.h"
#include "score/mw/com/impl/plumbing/sample_ptr.h"
#include "score/mw/com/impl/proxy_base.h"
#include "score/mw/com/impl/proxy_event.h"
#include "score/mw/com/impl/proxy_event_base.h"
#include "score/mw/com/impl/runtime.h"
#include "score/mw/com/impl/skeleton_base.h"
#include "score/mw/com/impl/skeleton_event.h"
#include "score/mw/com/impl/skeleton_event_base.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/types.h"
#include "score/mw/log/logging.h"

#include <score/span.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace score::mw::com::impl::rust
{

namespace field_detail
{
void LogFieldTypeMismatch(const std::size_t size,
                          const std::size_t align,
                          const std::size_t expected_size,
                          const std::size_t expected_align) noexcept
{
    mw::log::LogError("com-api") << "Field access rejected: value size/alignment" << size << "/" << align
                                 << "does not match the field type" << expected_size << "/" << expected_align;
}
}  // namespace field_detail

extern "C" {

/// \brief Get event pointer from proxy by event name
/// \details Retrieves an event from a proxy instance by string name.
/// The returned pointer should be cast to ProxyEvent<T>* where T is the event type.
/// \param proxy_ptr Opaque proxy pointer (actually ProxyType*)
/// \param interface_id UTF-8 string view of interface ID
/// \param event_id UTF-8 string view of event name
/// \return Pointer to ProxyEventBase if found, nullptr otherwise
ProxyEventBase* mw_com_get_event_from_proxy(ProxyBase* proxy_ptr, StringView interface_id, StringView event_id)
{
    if (proxy_ptr == nullptr || interface_id.data == nullptr || event_id.data == nullptr)
    {
        return nullptr;
    }
    auto event = static_cast<std::string_view>(event_id);
    auto id = static_cast<std::string_view>(interface_id);

    auto* registry = GlobalRegistryMapping::FindMemberOperation(id, event);

    if (registry == nullptr)
    {
        return nullptr;
    }
    return registry->GetProxyEvent(proxy_ptr);
}

/// \brief Get event pointer from skeleton by event name
/// \details Retrieves an event from a skeleton instance by string name.
/// Similar to mw_com_get_event_from_proxy but for skeleton instances.
/// \param skeleton_ptr Opaque skeleton pointer (actually SkeletonType*)
/// \param interface_id UTF-8 string view of interface ID
/// \param event_id UTF-8 string view of event name
/// \return Pointer to SkeletonEventBase if found, nullptr otherwise
SkeletonEventBase* mw_com_get_event_from_skeleton(SkeletonBase* skeleton_ptr,
                                                  StringView interface_id,
                                                  StringView event_id)
{
    if (skeleton_ptr == nullptr || interface_id.data == nullptr || event_id.data == nullptr)
    {
        return nullptr;
    }
    auto event = static_cast<std::string_view>(event_id);
    auto id = static_cast<std::string_view>(interface_id);

    auto* registry = GlobalRegistryMapping::FindMemberOperation(id, event);

    if (registry == nullptr)
    {
        return nullptr;
    }
    return registry->GetSkeletonEvent(skeleton_ptr);
}

/// \brief Send data via a skeleton event by name
/// \details Sends event data to all subscribed proxy instances.
/// \param event_ptr Opaque skeleton event pointer (SkeletonEvent<T>*)
/// \param type_ops Pointer to TypeOperations for the event data type T
/// \param data_ptr Pointer to event data (T*)
/// \return true if send successful, false otherwise
bool mw_com_skeleton_send_event(SkeletonEventBase* event_ptr, const TypeOperations* type_ops, void* data_ptr)
{
    if (event_ptr == nullptr || type_ops == nullptr || data_ptr == nullptr)
    {
        return false;
    }

    return type_ops->SkeletonSendEvent(event_ptr, data_ptr);
}

/// \brief Subscribe to a proxy event to allocate sample buffers
/// \details Must be called before GetNewSamples to initialize the event's sample tracker.
/// \param event_ptr Opaque event pointer (ProxyEventBase*)
/// \param max_sample_count Maximum number of concurrent samples to allocate
/// \return true if subscription successful, false otherwise
bool mw_com_proxy_event_subscribe(ProxyEventBase* event_ptr, uint32_t max_sample_count)
{
    if (event_ptr == nullptr)
    {
        return false;
    }

    auto result = event_ptr->Subscribe(max_sample_count);

    return result.has_value();
}

/// \brief Unsubscribe from a proxy event to release sample buffers
/// \details Must be called only after no `SamplePtr` instances are held on the Rust side
/// \param event_ptr Opaque event pointer (ProxyEventBase*)
void mw_com_proxy_event_unsubscribe(ProxyEventBase* event_ptr)
{
    if (event_ptr == nullptr)
    {
        return;
    }
    event_ptr->Unsubscribe();
}

/// \brief Create proxy instance dynamically
/// \details Creates a proxy for the given interface UID using the provided handle.
/// \param interface_id UTF-8 string view of interface UID (e.g., "mw_com_IpcBridge")
/// \param handle_ptr Opaque handle identifying the service instance
/// \return Pointer to ProxyBase instance, or nullptr on failure
ProxyBase* mw_com_create_proxy(StringView interface_id, const HandleType& handle_ptr)
{
    if (interface_id.data == nullptr)
    {
        return nullptr;
    }
    auto id = static_cast<std::string_view>(interface_id);
    auto* registry = GlobalRegistryMapping::FindInterfaceRegistry(id);

    if (registry == nullptr)
    {
        return nullptr;
    }
    return registry->CreateProxy(handle_ptr);
}

/// \brief Create skeleton instance dynamically
/// \details Creates a skeleton for the given interface UID.
/// \param interface_id UTF-8 string view of interface UID
/// \param instance_spec Pointer to InstanceSpecifier identifying the service to offer
/// \return Pointer to SkeletonBase instance, or nullptr on failure
SkeletonBase* mw_com_create_skeleton(StringView interface_id, ::score::mw::com::InstanceSpecifier* instance_spec)
{
    if (interface_id.data == nullptr || instance_spec == nullptr)
    {
        return nullptr;
    }

    auto id = static_cast<std::string_view>(interface_id);
    auto* registry = GlobalRegistryMapping::FindInterfaceRegistry(id);

    if (registry == nullptr)
    {
        return nullptr;
    }

    return registry->CreateSkeleton(*instance_spec);
}

/// \brief Offer service for skeleton instance
/// \details Starts offering the service on the provided skeleton instance.
/// \param skeleton_ptr Opaque skeleton pointer
/// \return true if service is offered successfully, false otherwise
bool mw_com_skeleton_offer_service(SkeletonBase* skeleton_ptr)
{
    if (skeleton_ptr == nullptr)
    {
        return false;
    }

    bool result = skeleton_ptr->OfferService().has_value();
    return result;
}

/// \brief Stop offering service for skeleton instance
/// \details Stops offering the service on the provided skeleton instance.
/// \param skeleton_ptr Opaque skeleton pointer
void mw_com_skeleton_stop_offer_service(SkeletonBase* skeleton_ptr)
{
    if (skeleton_ptr == nullptr)
    {
        return;
    }
    skeleton_ptr->StopOfferService();
}

/// \brief Destroy proxy instance
/// \details Deallocates a proxy created with mw_com_create_proxy.
/// \param proxy_ptr Opaque proxy pointer to destroy
void mw_com_destroy_proxy(ProxyBase* proxy_ptr)
{
    if (proxy_ptr == nullptr)
    {
        return;
    }

    delete proxy_ptr;
    ;
}

/// \brief Destroy skeleton instance
/// \details Deallocates a skeleton created with mw_com_create_skeleton.
/// \param skeleton_ptr Opaque skeleton pointer to destroy
void mw_com_destroy_skeleton(SkeletonBase* skeleton_ptr)
{
    if (skeleton_ptr == nullptr)
    {
        return;
    }
    delete skeleton_ptr;
}

/// \brief Get samples from proxy event of specific type
/// \details Retrieves new samples from a proxy event using the type operations registry.
/// \param event_ptr Opaque proxy event pointer (ProxyEventBase*)
/// \param type_ops Pointer to TypeOperations for the event data type T
/// \param callback Pointer to FatPtr callback for sample processing
/// \param max_samples Maximum number of samples to retrieve
/// \return Number of samples retrieved, or std::numeric_limits<std::uint32_t>::max() on error
std::uint32_t mw_com_type_registry_get_samples_from_event(ProxyEventBase* event_ptr,
                                                          const TypeOperations* type_ops,
                                                          const FatPtr* callback,
                                                          uint32_t max_samples)
{
    if (event_ptr == nullptr || type_ops == nullptr || callback == nullptr)
    {
        return std::numeric_limits<std::uint32_t>::max();
    }

    auto result = type_ops->GetSamplesFromEvent(event_ptr, max_samples, *callback);

    if (!result.has_value())
    {
        return std::numeric_limits<std::uint32_t>::max();
    }

    return result.value();
}

/// @brief Get sample data pointer from SamplePtr<T>
/// @param sample_ptr Opaque sample pointer
/// @param type_ops Type operations pointer
/// @return Pointer to sample data, or nullptr if type mismatch
const void* mw_com_get_sample_ptr(const void* sample_ptr, const TypeOperations* type_ops)
{
    if (sample_ptr == nullptr || type_ops == nullptr)
    {
        return nullptr;
    }

    return type_ops->GetSamplePtrData(sample_ptr);
}

/// @brief Delete sample pointer of specific type
/// @param sample_ptr Opaque sample pointer
/// @param type_ops Type operations pointer
void mw_com_delete_sample_ptr(void* sample_ptr, const TypeOperations* type_ops)
{
    if (sample_ptr == nullptr || type_ops == nullptr)
    {
        return;
    }

    type_ops->DeleteSamplePtr(sample_ptr);
}

/// @brief Get allocatee pointer from skeleton event of specific type
/// @param event_ptr Opaque skeleton event pointer
/// @param allocatee_ptr Pointer to pre-allocated memory for allocatee
/// @param type_ops Type operations pointer
/// @return True if allocatee pointer was retrieved successfully, false otherwise
bool mw_com_get_allocatee_ptr(SkeletonEventBase* event_ptr, void* allocatee_ptr, const TypeOperations* type_ops)
{
    if (event_ptr == nullptr || type_ops == nullptr)
    {
        return false;
    }

    return type_ops->GetAllocateePtr(event_ptr, allocatee_ptr);
}

/// @brief Delete allocatee pointer of specific type
/// @param allocatee_ptr Pointer to SampleAllocateePtr<T>
/// @param type_ops Type operations pointer
void mw_com_delete_allocatee_ptr(void* allocatee_ptr, const TypeOperations* type_ops)
{
    if (allocatee_ptr == nullptr || type_ops == nullptr)
    {
        return;
    }

    type_ops->DeleteAllocateePtr(allocatee_ptr);
}

/// @brief Get allocatee data pointer from allocatee of specific type
/// @param allocatee_ptr Pointer to SampleAllocateePtr<T>
/// @param type_ops Type operations pointer
/// @return Pointer to allocatee data, or nullptr if type mismatch
void* mw_com_get_allocatee_data_ptr(void* allocatee_ptr, const TypeOperations* type_ops)
{
    if (allocatee_ptr == nullptr || type_ops == nullptr)
    {
        return nullptr;
    }

    return type_ops->GetAllocateeDataPtr(allocatee_ptr);
}

/// @brief  Send event via skeleton using allocatee pointer of specific type
/// @param event_ptr Opaque skeleton event pointer
/// @param type_ops Type operations pointer
/// @param allocatee_ptr Pointer to SampleAllocateePtr<T>
/// @return True if event was sent successfully, false otherwise
bool mw_com_skeleton_send_event_allocatee(SkeletonEventBase* event_ptr,
                                          const TypeOperations* type_ops,
                                          void* allocatee_ptr)
{
    if (event_ptr == nullptr || type_ops == nullptr || allocatee_ptr == nullptr)
    {
        return false;
    }

    return type_ops->SkeletonSendEventAllocatee(event_ptr, allocatee_ptr);
}

// --- Method FFI functions. They operate directly on the type-erased ProxyMethodBinding*/SkeletonMethodBinding*
// (see MethodMemberOperation in registry_bridge_macro.h). The C++ binding supports one call in flight
// (kCallQueueSize == 1), so the Rust side always passes queue_position 0.

/// \brief Get the type-erased ProxyMethodBinding for a named method on a proxy.
/// \details Mirrors mw_com_get_event_from_proxy's shape, but looks the member up in the *method* registry
/// (InterfaceOperations::method_operation_map_) rather than the event one, and returns the binding directly
/// instead of a `ProxyEventBase*` (there is no per-type dispatch needed here, see the file comment above).
/// \param proxy_ptr Opaque proxy pointer (actually ProxyType*)
/// \param interface_id UTF-8 string view of interface ID
/// \param method_id UTF-8 string view of method name
/// \return Pointer to ProxyMethodBinding if found, nullptr otherwise (including: the binding failed to construct —
/// see ProxyMethodBase's constructor, which stores nullptr in binding_ and the real error in
/// binding_construction_result_ on failure, not surfaced through this FFI function).
ProxyMethodBinding* mw_com_get_method_from_proxy(ProxyBase* proxy_ptr, StringView interface_id, StringView method_id)
{
    if (proxy_ptr == nullptr || interface_id.data == nullptr || method_id.data == nullptr)
    {
        return nullptr;
    }
    auto* const registry = GlobalRegistryMapping::FindMethodOperation(static_cast<std::string_view>(interface_id),
                                                                      static_cast<std::string_view>(method_id));
    if (registry == nullptr)
    {
        return nullptr;
    }
    return registry->GetProxyMethodBinding(proxy_ptr);
}

/// \brief Get the type-erased SkeletonMethodBinding for a named method on a skeleton.
/// \param skeleton_ptr Opaque skeleton pointer (actually SkeletonType*)
/// \param interface_id UTF-8 string view of interface ID
/// \param method_id UTF-8 string view of method name
/// \return Pointer to SkeletonMethodBinding if found, nullptr otherwise
SkeletonMethodBinding* mw_com_get_method_from_skeleton(SkeletonBase* skeleton_ptr,
                                                       StringView interface_id,
                                                       StringView method_id)
{
    if (skeleton_ptr == nullptr || interface_id.data == nullptr || method_id.data == nullptr)
    {
        return nullptr;
    }
    auto* const registry = GlobalRegistryMapping::FindMethodOperation(static_cast<std::string_view>(interface_id),
                                                                      static_cast<std::string_view>(method_id));
    if (registry == nullptr)
    {
        return nullptr;
    }
    return registry->GetSkeletonMethodBinding(skeleton_ptr);
}

/// \brief Allocate/retrieve the in-arguments buffer for a proxy method call at queue position 0.
/// \details Wraps ProxyMethodBinding::GetInArgsBuffer. The Rust caller serializes its arguments with the same
/// layout as type_erased_storage.h's SerializeArgs (score_com_concept::MethodArgsCodec); a mismatch would corrupt
/// memory silently.
/// \param binding Pointer to ProxyMethodBinding, obtained via mw_com_get_method_from_proxy
/// \param queue_position Call-queue position (always 0, kCallQueueSize == 1)
/// \param out_data [out] Pointer to the start of the in-args buffer on success
/// \param out_len [out] Length in bytes of the in-args buffer on success
/// \return true if the buffer was retrieved successfully, false otherwise (including: this method has no
/// in-arguments at all — GetInArgsBuffer must not be called in that case per its own doc comment, so this
/// function must only be invoked by Rust code that already knows, from the method's static Signature, that
/// ArgTypes is non-empty)
bool mw_com_proxy_method_get_in_args_buffer(ProxyMethodBinding* binding,
                                            std::size_t queue_position,
                                            std::uint8_t** out_data,
                                            std::size_t* out_len)
{
    if (binding == nullptr || out_data == nullptr || out_len == nullptr)
    {
        return false;
    }
    auto result = binding->GetInArgsBuffer(queue_position);
    if (!result.has_value())
    {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): reinterpreting a type-erased std::byte buffer as
    // std::uint8_t bytes for the FFI boundary, both are 1-byte object types with no representation difference.
    *out_data = reinterpret_cast<std::uint8_t*>(result.value().data());
    *out_len = result.value().size();
    return true;
}

/// \brief Allocate/retrieve the return-value buffer for a proxy method call at queue position 0.
/// \details Wraps ProxyMethodBinding::GetReturnValueBuffer.
/// \param binding Pointer to ProxyMethodBinding, obtained via mw_com_get_method_from_proxy
/// \param queue_position Call-queue position (always 0)
/// \param out_data [out] Pointer to the start of the return-value buffer on success
/// \param out_len [out] Length in bytes of the return-value buffer on success
/// \return true if the buffer was retrieved successfully, false otherwise (including: this method has a void
/// return type — GetReturnValueBuffer must not be called in that case per its own doc comment)
bool mw_com_proxy_method_get_return_value_buffer(ProxyMethodBinding* binding,
                                                 std::size_t queue_position,
                                                 std::uint8_t** out_data,
                                                 std::size_t* out_len)
{
    if (binding == nullptr || out_data == nullptr || out_len == nullptr)
    {
        return false;
    }
    auto result = binding->GetReturnValueBuffer(queue_position);
    if (!result.has_value())
    {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see mw_com_proxy_method_get_in_args_buffer above.
    *out_data = reinterpret_cast<std::uint8_t*>(result.value().data());
    *out_len = result.value().size();
    return true;
}

/// \brief Perform the actual method call at queue position 0.
/// \details Wraps ProxyMethodBinding::DoCall. The in-args buffer (if any) must be filled and the return-value buffer
/// (if any) retrieved first, as in ProxyMethod<Signature>::operator(). Blocks until the call completes.
/// \param binding Pointer to ProxyMethodBinding, obtained via mw_com_get_method_from_proxy
/// \param queue_position Call-queue position (always 0)
/// \return true if the call succeeded, false otherwise
bool mw_com_proxy_method_do_call(ProxyMethodBinding* binding, std::size_t queue_position)
{
    if (binding == nullptr)
    {
        return false;
    }
    return binding->DoCall(queue_position).has_value();
}

/// \brief Register a Rust closure as the handler for a skeleton method.
/// \details Wraps SkeletonMethodBinding::RegisterHandler, adapting its TypeErasedHandler signature
/// (void(QualityType, optional<span<byte>>, optional<span<byte>>)) to the flattened
/// mw_com_impl_call_method_handler FFI callback (see that function's doc comment in registry_bridge_macro.h for
/// why the optionals are flattened to (ptr, len, present) triples).
/// \param binding Pointer to SkeletonMethodBinding, obtained via mw_com_get_method_from_skeleton
/// \param boxed_handler Pointer to FatPtr containing the Rust closure to invoke on each call
/// \return true if the handler was registered successfully, false otherwise
/// \note SkeletonMethodBinding has no way to unregister a handler, so the boxed Rust closure lives as long as the
/// process (unlike event receive handlers, freed via mw_com_impl_delete_boxed_fnmut).
bool mw_com_skeleton_method_register_handler(SkeletonMethodBinding* binding, const FatPtr* boxed_handler)
{
    if (binding == nullptr || boxed_handler == nullptr)
    {
        return false;
    }
    const FatPtr handler_ptr = *boxed_handler;
    SkeletonMethodBinding::TypeErasedHandler type_erased_handler =
        [handler_ptr](QualityType quality_type,
                      std::optional<score::cpp::span<std::byte>> in_args,
                      std::optional<score::cpp::span<std::byte>> return_value) mutable {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::byte* -> std::uint8_t*, safe for the
            // same reason as mw_com_proxy_method_get_in_args_buffer above.
            std::uint8_t* const in_args_data =
                in_args.has_value() ? reinterpret_cast<std::uint8_t*>(in_args->data()) : nullptr;
            const std::size_t in_args_len = in_args.has_value() ? in_args->size() : 0U;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see above.
            std::uint8_t* const return_data =
                return_value.has_value() ? reinterpret_cast<std::uint8_t*>(return_value->data()) : nullptr;
            const std::size_t return_len = return_value.has_value() ? return_value->size() : 0U;
            mw_com_impl_call_method_handler(&handler_ptr,
                                            static_cast<std::uint8_t>(quality_type),
                                            in_args_data,
                                            in_args_len,
                                            in_args.has_value(),
                                            return_data,
                                            return_len,
                                            return_value.has_value());
        };
    return binding->RegisterHandler(std::move(type_erased_handler)).has_value();
}

// --- Field get/set/update FFI. These go through the typed ProxyField<T>/
// SkeletonField<T> API registered by EXPORT_MW_COM_FIELD (see FieldMemberOperation in registry_bridge_macro.h), not
// through the type-erased method bindings: a field getter/setter returns score::Result<T> on the wire, and
// SkeletonField implements the set-handler and default get-handler semantics itself.

/// \brief Call a field's getter (ProxyField<T>::Get()) and copy the received value into `out_value`.
/// \param proxy_ptr Proxy created via mw_com_create_proxy
/// \param out_value Uninitialized storage for one value of the field's type, owned by the caller
/// \param size, align Size and alignment of the caller's value type, checked against the field's type
/// \param out_error On failure: the C++ error code, kFieldOperationUnavailable if the field is not exported via
/// EXPORT_MW_COM_FIELD, has no WithGetter tag, or a pointer is invalid, or kFieldTypeMismatch
/// \return true on success
bool mw_com_proxy_field_get(ProxyBase* proxy_ptr,
                            StringView interface_id,
                            StringView field_id,
                            void* out_value,
                            std::size_t size,
                            std::size_t align,
                            std::int32_t* out_error)
{
    auto* const ops = field_detail::FindFieldOperation(interface_id, field_id);
    if ((ops == nullptr) || (proxy_ptr == nullptr))
    {
        field_detail::SetError(out_error, kFieldOperationUnavailable);
        return false;
    }
    return ops->ProxyGet(proxy_ptr, out_value, size, align, out_error);
}

/// \brief Call a field's setter (ProxyField<T>::Set()) with `*in_value` and copy the value the skeleton accepted
/// into `out_value`. See mw_com_proxy_field_get for the other parameters.
bool mw_com_proxy_field_set(ProxyBase* proxy_ptr,
                            StringView interface_id,
                            StringView field_id,
                            const void* in_value,
                            void* out_value,
                            std::size_t size,
                            std::size_t align,
                            std::int32_t* out_error)
{
    auto* const ops = field_detail::FindFieldOperation(interface_id, field_id);
    if ((ops == nullptr) || (proxy_ptr == nullptr))
    {
        field_detail::SetError(out_error, kFieldOperationUnavailable);
        return false;
    }
    return ops->ProxySet(proxy_ptr, in_value, out_value, size, align, out_error);
}

/// \brief Update a field's value (SkeletonField<T>::Update()). Before OfferService this stores the initial value.
/// \param skeleton_ptr Skeleton created via mw_com_create_skeleton
/// \param value Pointer to one value of the field's type
/// \param size, align Size and alignment of the caller's value type, checked against the field's type
/// \return true on success
bool mw_com_skeleton_field_update(SkeletonBase* skeleton_ptr,
                                  StringView interface_id,
                                  StringView field_id,
                                  const void* value,
                                  std::size_t size,
                                  std::size_t align)
{
    auto* const ops = field_detail::FindFieldOperation(interface_id, field_id);
    if ((ops == nullptr) || (skeleton_ptr == nullptr))
    {
        return false;
    }
    return ops->SkeletonUpdate(skeleton_ptr, value, size, align);
}

/// \brief Install a Rust closure as a field's set handler (SkeletonField<T>::RegisterSetHandler()).
/// \details The closure is invoked through mw_com_impl_call_field_set_handler. Registering through SkeletonField
/// (rather than the setter's method binding) is what lets OfferService see that the set handler exists, and makes
/// SkeletonField publish the accepted value to subscribers. Same handler-disposal gap as
/// mw_com_skeleton_method_register_handler: the boxed closure lives as long as the process.
/// \param boxed_handler Pointer to FatPtr containing the Rust closure
/// \param size, align Size and alignment of the handler's value type, checked against the field's type
/// \return true if the handler was registered; false if the field is not exported, has no WithSetter tag, the value
/// type does not match, or the registration failed
bool mw_com_skeleton_field_register_set_handler(SkeletonBase* skeleton_ptr,
                                                StringView interface_id,
                                                StringView field_id,
                                                const FatPtr* boxed_handler,
                                                std::size_t size,
                                                std::size_t align)
{
    auto* const ops = field_detail::FindFieldOperation(interface_id, field_id);
    if ((ops == nullptr) || (skeleton_ptr == nullptr) || (boxed_handler == nullptr))
    {
        return false;
    }
    return ops->SkeletonRegisterSetHandler(skeleton_ptr, *boxed_handler, size, align);
}

// --- Field subscription queries. A field's notifier is a real ProxyEvent<T>, so FieldSubscription's
// get_free_sample_count()/get_num_new_samples_available() map onto ProxyEventBase directly.

/// \brief Query the number of free (unused) sample slots remaining for a proxy event subscription.
/// \details Wraps ProxyEventBase::GetFreeSampleCount() (score/mw/com/impl/proxy_event_base.h), which is
/// noexcept and cannot fail.
/// \param event_ptr Opaque proxy event pointer (ProxyEventBase*), obtained via mw_com_get_event_from_proxy
/// \return The number of free sample slots, or 0 if event_ptr is null.
std::size_t mw_com_proxy_event_get_free_sample_count(ProxyEventBase* event_ptr)
{
    if (event_ptr == nullptr)
    {
        return 0U;
    }
    return event_ptr->GetFreeSampleCount();
}

/// \brief Query the number of new samples currently available to receive for a proxy event subscription.
/// \details Wraps ProxyEventBase::GetNumNewSamplesAvailable() (score/mw/com/impl/proxy_event_base.h),
/// which returns a Result<size_t> (unlike GetFreeSampleCount, this can fail).
/// \param event_ptr Opaque proxy event pointer (ProxyEventBase*)
/// \param out_count [out] The number of new samples available, on success
/// \return true on success (with *out_count populated), false otherwise (leaving *out_count untouched)
bool mw_com_proxy_event_get_num_new_samples_available(ProxyEventBase* event_ptr, std::size_t* out_count)
{
    if (event_ptr == nullptr || out_count == nullptr)
    {
        return false;
    }
    auto result = event_ptr->GetNumNewSamplesAvailable();
    if (!result.has_value())
    {
        return false;
    }
    *out_count = result.value();
    return true;
}

/// \brief Set event receive handler for proxy event
/// \details Registers a Rust FnMut handler for a proxy event. The handler will be called when new samples are received.
/// \param event_ptr Opaque proxy event pointer (ProxyEventBase*)
/// \param boxed_handler Pointer to FatPtr containing the Rust FnMut handler
/// @return True if handler was set successfully, false otherwise
bool mw_com_proxy_set_event_receive_handler(ProxyEventBase* event_ptr, const FatPtr* boxed_handler)
{
    if (event_ptr == nullptr || boxed_handler == nullptr)
    {
        return false;
    }

    auto result = event_ptr->SetReceiveHandler(RustFnMutCallable<RustBoxedCallable>{*boxed_handler});
    return result.has_value();
}

/// \brief Clear event receive handler for proxy event
/// \details Unregisters the event receive handler for a proxy event, if any.
/// \param event_ptr Opaque proxy event pointer (ProxyEventBase*)
void mw_com_proxy_clear_event_receive_handler(ProxyEventBase* event_ptr)
{
    if (event_ptr == nullptr)
    {
        return;
    }
    score::cpp::ignore = event_ptr->UnsetReceiveHandler();
}

/// \brief Start asynchronous service discovery with a callback
/// \details Initiates a service discovery operation using a Rust callback. The callback will be invoked
/// when matching services are found. The callback signature is:
/// void(ServiceHandleContainer<HandleType>, FindServiceHandle)
///
/// The callback can be invoked:
/// - Synchronously: if matching services already exist when StartFindService is called
/// - Asynchronously: if new services become available after the search starts (called from worker thread)
///
/// \param callback FatPtr to Rust FnMut closure that matches the FindServiceHandler signature
/// \param instance_spec Pointer to InstanceSpecifier for service discovery criteria
/// \return Opaque pointer to FindServiceHandle on success, nullptr on failure
/// \note The FindServiceHandle returned is passed to the callback to allow StopFindService calls
void* mw_com_start_find_service(const FatPtr* callback, InstanceSpecifier* instance_spec)
{
    if (callback == nullptr || instance_spec == nullptr)
    {
        return nullptr;
    }

    // Create a RustFnMutCallable with RustBoxedCallable handler
    // Callback signature: void(ServiceHandleContainer<HandleType>, FindServiceHandle)
    RustFnMutCallable<RustBoxedCallable, void, ServiceHandleContainer<HandleType>, FindServiceHandle> rust_callable{
        *callback};

    if (auto result = ::score::mw::com::impl::Runtime::getInstance().GetServiceDiscovery().StartFindService(
            std::move(rust_callable), std::move(*instance_spec));
        result.has_value())
    {
        return new FindServiceHandle{std::move(result).value()};
    }
    else
    {
        return nullptr;
    }
}

/// \brief Stop an ongoing service discovery operation and delete the handle
/// \details Stops the service discovery operation associated with the provided FindServiceHandle
/// and deallocates the handle. This is the only place where the handle should be deleted.
/// \param find_service_handle_ptr Opaque pointer to FindServiceHandle returned by mw_com_start_find_service
void mw_com_stop_find_service(void* find_service_handle_ptr)
{
    if (find_service_handle_ptr == nullptr)
    {
        return;
    }

    auto* find_service_handle = static_cast<FindServiceHandle*>(find_service_handle_ptr);

    // Stop the service discovery
    auto result =
        ::score::mw::com::impl::Runtime::getInstance().GetServiceDiscovery().StopFindService(*find_service_handle);

    if (!result.has_value())
    {
        mw::log::LogError("com-api") << "Failed to stop service discovery for handle: " << result.error();
    }

    delete find_service_handle;
}

/// \brief Get type operations instance for a given interface and member id
/// \details Retrieves a pointer to the TypeOperations instance associated with the specified interface ID and member id
/// (e.g., event name). This allows Rust code to perform type-specific operations for events without using the
/// registry-based approach.
/// \param interface_id UTF-8 string view of interface ID
/// \param member_name UTF-8 string view of member name (e.g., event name)
/// \return Const pointer to TypeOperations instance if found, nullptr otherwise
const void* mw_com_get_type_ops_instance(StringView interface_id, StringView member_name)
{
    if (interface_id.data == nullptr || member_name.data == nullptr)
    {
        return nullptr;
    }

    auto id = static_cast<std::string_view>(interface_id);
    auto member = static_cast<std::string_view>(member_name);

    auto* registry = GlobalRegistryMapping::FindMemberOperation(id, member);

    if (registry == nullptr)
    {
        return nullptr;
    }

    return registry->GetTypeOps();
}

/// \brief Create an InstanceSpecifier from a UTF-8 string
/// \details Allocates and returns a new InstanceSpecifier based on the provided UTF-8 string
/// \param instance_specifier UTF-8 string representing the instance specifier
/// \param instance_specifier_length Length of the UTF-8 string
/// \return Pointer to newly allocated InstanceSpecifier, or nullptr on failure
::score::mw::com::InstanceSpecifier* mw_com_impl_instance_specifier_create(
    const char* const instance_specifier,
    const std::uint32_t instance_specifier_length) noexcept
{
    if (auto result =
            ::score::mw::com::InstanceSpecifier::Create(std::string{instance_specifier, instance_specifier_length});
        result.has_value())
    {
        return new ::score::mw::com::InstanceSpecifier{std::move(result).value()};
    }
    else
    {
        return nullptr;
    }
}

/// \brief Clone an existing InstanceSpecifier
/// \details Allocates and returns a new InstanceSpecifier that is a copy of the provided one
/// \param instance_specifier Reference to the existing InstanceSpecifier to clone
/// \return Pointer to newly allocated InstanceSpecifier, or nullptr on failure
::score::mw::com::InstanceSpecifier* mw_com_impl_instance_specifier_clone(
    const ::score::mw::com::InstanceSpecifier& instance_specifier) noexcept
{
    return new ::score::mw::com::InstanceSpecifier{instance_specifier};
}

void mw_com_impl_instance_specifier_delete(::score::mw::com::InstanceSpecifier* instance_specifier) noexcept
{
    delete instance_specifier;
}

/// \brief Find services matching an InstanceSpecifier
/// \details Performs service discovery for services matching the provided InstanceSpecifier.
/// \param instance_specifier Pointer to InstanceSpecifier specifying the service criteria
/// \return Pointer to ServiceHandleContainer containing matching handles, or nullptr if none found
::score::mw::com::ServiceHandleContainer<::score::mw::com::impl::HandleType>* mw_com_impl_find_service(
    ::score::mw::com::InstanceSpecifier* instance_specifier) noexcept
{
    if (auto result = ::score::mw::com::impl::Runtime::getInstance().GetServiceDiscovery().FindService(
            std::move(*instance_specifier));
        result.has_value())
    {
        return new ::score::mw::com::ServiceHandleContainer<::score::mw::com::impl::HandleType>{
            std::move(result).value()};
    }
    else
    {
        return nullptr;
    }
}

/// \brief Delete a ServiceHandleContainer
/// \param container Pointer to the ServiceHandleContainer to delete
void mw_com_impl_handle_container_delete(
    ::score::mw::com::ServiceHandleContainer<::score::mw::com::impl::HandleType>* container) noexcept
{
    delete container;
}

/// \brief Get the size of a ServiceHandleContainer
/// \param container Pointer to the ServiceHandleContainer
/// \return Number of elements in the container
std::uint32_t mw_com_impl_handle_container_get_size(
    const ::score::mw::com::ServiceHandleContainer<::score::mw::com::impl::HandleType>* container)
{
    return static_cast<std::uint32_t>(container->size());
}

/// \brief Get a handle at a specific position in a ServiceHandleContainer
/// \param container Pointer to the ServiceHandleContainer
/// \param pos Index of the handle to retrieve
/// \return Pointer to the handle at the specified position, or nullptr if out of bounds
const ::score::mw::com::impl::HandleType* mw_com_impl_handle_container_get_handle_at(
    const ::score::mw::com::ServiceHandleContainer<::score::mw::com::impl::HandleType>* container,
    std::uint32_t pos) noexcept
{
    return &container->at(pos);
}

/// \brief Initialize the runtime
/// \param argv Array of command-line arguments
/// \param argc Number of command-line arguments
void mw_com_impl_initialize(const char* argv[], std::int32_t argc)
{
    ::score::mw::com::runtime::InitializeRuntime(argc, argv);
}

/// \brief Get the size of a SamplePtr
/// \return Size of the SamplePtr type in bytes
std::uint32_t mw_com_impl_sample_ptr_get_size() noexcept
{
    return sizeof(::score::mw::com::impl::SamplePtr<std::uint32_t>);
}

}  // extern "C"
}  // namespace score::mw::com::impl::rust
