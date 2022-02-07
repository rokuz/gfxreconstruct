/*
** Copyright (c) 2022 LunarG, Inc.
** Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#include "decode/dx12_resource_value_mapper.h"

#include "decode/custom_dx12_struct_decoders.h"

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

namespace /* anonymous */
{

template <typename T>
T* GetExtraInfo(const DxObjectInfo* info)
{
    if ((info != nullptr) && (info->extra_info != nullptr) && (info->extra_info->extra_info_type == T::kType))
    {
        return static_cast<T*>(info->extra_info.get());
    }

    GFXRECON_LOG_FATAL("%s object does not have an associated info structure", T::kObjectType);

    return nullptr;
}

static D3D12ResourceInfo* GetResourceExtraInfo(DxObjectInfo* resource_object_info)
{
    GFXRECON_ASSERT(resource_object_info != nullptr);
    if (resource_object_info->extra_info == nullptr)
    {
        resource_object_info->extra_info = std::make_unique<D3D12ResourceInfo>();
    }
    return GetExtraInfo<D3D12ResourceInfo>(resource_object_info);
}

} // namespace

void Dx12ResourceValueMapper::PostProcessCommandListReset(DxObjectInfo* command_list_object_info)
{
    // Clear tracked info.
    auto command_list_extra_info = GetExtraInfo<D3D12CommandListInfo>(command_list_object_info);
    command_list_extra_info->resource_value_info_map.clear();
}

void Dx12ResourceValueMapper::PreProcessExecuteCommandLists(
    DxObjectInfo*                             command_queue_object_info,
    UINT                                      num_command_lists,
    HandlePointerDecoder<ID3D12CommandList*>* command_lists_decoder,
    bool&                                     needs_mapping)
{
    auto command_queue            = static_cast<ID3D12CommandQueue*>(command_queue_object_info->object);
    auto command_queue_extra_info = GetExtraInfo<D3D12CommandQueueInfo>(command_queue_object_info);

    GFXRECON_ASSERT(command_queue_extra_info != nullptr);

    if (command_queue_extra_info->resource_value_map_fence == nullptr)
    {
        InitializeRequiredObjects(command_queue, command_queue_extra_info);
    }

    // Determine if there are values that need to be mapped in resources referenced by the command list.
    needs_mapping = false;
    for (UINT i = 0; (i < num_command_lists) && !needs_mapping; ++i)
    {
        auto command_list_extra_info =
            GetExtraInfo<D3D12CommandListInfo>(get_object_info_func_(command_lists_decoder->GetPointer()[i]));
        GFXRECON_ASSERT(command_list_extra_info != nullptr);

        for (auto& resource_value_info_pair : command_list_extra_info->resource_value_info_map)
        {
            if (!resource_value_info_pair.second.empty())
            {
                needs_mapping = true;
                break;
            }
        }
    }

    if (needs_mapping)
    {
        // Signal the resource_value_map_fence to indicate it is safe to begin mapping, then wait for the
        // resource_value_map_fence to ensure mapping has completed. No other objects should be signaling or waiting
        // on this fence, so no events need to be added to the command queue's pending events list.
        command_queue->Signal(command_queue_extra_info->resource_value_map_fence,
                              command_queue_extra_info->resource_value_map_fence_value);
        command_queue->Wait(command_queue_extra_info->resource_value_map_fence,
                            command_queue_extra_info->resource_value_map_fence_value + 1);
    }
}

void Dx12ResourceValueMapper::PostProcessExecuteCommandLists(
    DxObjectInfo*                             command_queue_object_info,
    UINT                                      num_command_lists,
    HandlePointerDecoder<ID3D12CommandList*>* command_lists_decoder,
    bool                                      needs_mapping)
{
    if (needs_mapping)
    {
        auto command_queue            = static_cast<ID3D12CommandQueue*>(command_queue_object_info->object);
        auto command_queue_extra_info = GetExtraInfo<D3D12CommandQueueInfo>(command_queue_object_info);

        GFXRECON_ASSERT(command_queue_extra_info != nullptr);

        auto& pending_events = command_queue_extra_info->pending_events;

        // Signal the fence again to indicate the command list has completed and the resources with mapped values can be
        // restored to the unmapped values.
        command_queue->Signal(command_queue_extra_info->resource_value_map_fence,
                              command_queue_extra_info->resource_value_map_fence_value + 2);
        command_queue->Wait(command_queue_extra_info->resource_value_map_fence,
                            command_queue_extra_info->resource_value_map_fence_value + 3);

        ProcessResourceMappingsArgs process_args{ command_queue_extra_info->resource_value_map_fence,
                                                  command_queue_extra_info->resource_value_map_fence_value,
                                                  command_queue_extra_info->resource_value_map_event };

        for (UINT i = 0; i < num_command_lists; ++i)
        {
            auto command_list_extra_info =
                GetExtraInfo<D3D12CommandListInfo>(get_object_info_func_(command_lists_decoder->GetPointer()[i]));
            GFXRECON_ASSERT(command_list_extra_info != nullptr);

            for (const auto& resource_value_info_pair : command_list_extra_info->resource_value_info_map)
            {
                if (!resource_value_info_pair.second.empty())
                {
                    process_args.resource_value_info_map[resource_value_info_pair.first].insert(
                        resource_value_info_pair.second.begin(), resource_value_info_pair.second.end());
                }
            }
        }

        // Run (or queue) resource value mapping process.
        if (pending_events.empty())
        {
            ProcessResourceMappings(std::move(process_args));
        }
        else
        {
            command_queue_extra_info->pending_events.push_back(
                CreateProcessProcessResourceMappingsSyncEvent(std::move(process_args)));
        }

        command_queue_extra_info->resource_value_map_fence_value += 4;
    }
}

void Dx12ResourceValueMapper::PostProcessCreateCommandSignature(HandlePointerDecoder<void*>* command_signature_decoder,
                                                                const D3D12_COMMAND_SIGNATURE_DESC* desc)
{
    auto command_signature_object_info = static_cast<DxObjectInfo*>(command_signature_decoder->GetConsumerData(0));
    auto command_signature_extra_info  = GetExtraInfo<D3D12CommandSignatureInfo>(command_signature_object_info);

    GFXRECON_ASSERT(command_signature_extra_info != nullptr);

    // Determine offsets in the command signature that will contain GPU VAs
    command_signature_extra_info->byte_stride = desc->ByteStride;
    auto& resource_value_infos                = command_signature_extra_info->resource_value_infos;
    resource_value_infos.clear();
    UINT byte_offset = 0;
    for (UINT i = 0; i < desc->NumArgumentDescs; ++i)
    {
        const auto& arg_desc = desc->pArgumentDescs[i];
        switch (arg_desc.Type)
        {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
                byte_offset += sizeof(D3D12_DRAW_ARGUMENTS);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
                byte_offset += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
                byte_offset += sizeof(D3D12_DISPATCH_ARGUMENTS);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
                resource_value_infos.insert({ byte_offset + offsetof(D3D12_VERTEX_BUFFER_VIEW, BufferLocation),
                                              ResourceValueType::kGpuVirtualAddress,
                                              sizeof(D3D12_VERTEX_BUFFER_VIEW::BufferLocation) });
                byte_offset += sizeof(D3D12_VERTEX_BUFFER_VIEW);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
                resource_value_infos.insert({ byte_offset + offsetof(D3D12_INDEX_BUFFER_VIEW, BufferLocation),
                                              ResourceValueType::kGpuVirtualAddress,
                                              sizeof(D3D12_INDEX_BUFFER_VIEW::BufferLocation) });
                byte_offset += sizeof(D3D12_INDEX_BUFFER_VIEW);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                byte_offset += arg_desc.Constant.Num32BitValuesToSet * 4;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
                resource_value_infos.insert(
                    { byte_offset, ResourceValueType::kGpuVirtualAddress, sizeof(D3D12_GPU_VIRTUAL_ADDRESS) });
                byte_offset += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
                // TODO (GH# 416): Implement support for D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS. Also validate that
                // byte_offset is updated correctly.
                GFXRECON_LOG_ERROR("Application is using unsupported indirect argument type: "
                                   "D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS");
                byte_offset += sizeof(D3D12_DISPATCH_RAYS_DESC);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
                byte_offset += sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
                break;
            default:
                GFXRECON_LOG_ERROR("Ignoring unknown command signature argument type %d.", arg_desc.Type);
                break;
        }
    }
}

void Dx12ResourceValueMapper::PostProcessExecuteIndirect(DxObjectInfo* command_list_object_info,
                                                         DxObjectInfo* command_signature_object_info,
                                                         UINT          max_command_count,
                                                         DxObjectInfo* argument_buffer_object_info,
                                                         UINT64        argument_buffer_offset,
                                                         DxObjectInfo* count_buffer_object_info,
                                                         UINT64        count_buffer_offset)
{
    auto command_list_extra_info = GetExtraInfo<D3D12CommandListInfo>(command_list_object_info);
    GFXRECON_ASSERT(command_list_extra_info != nullptr);

    auto command_signature_extra_info = GetExtraInfo<D3D12CommandSignatureInfo>(command_signature_object_info);
    GFXRECON_ASSERT(command_signature_extra_info != nullptr);

    auto& resource_value_infos = command_list_extra_info->resource_value_info_map[argument_buffer_object_info];

    // TODO (GH# 417): Support pCountBuffer argument.
    if ((count_buffer_object_info != nullptr) && (count_buffer_object_info->object != nullptr))
    {
        GFXRECON_LOG_WARNING_ONCE("The pCountBuffer argument for ExecuteIndirect is not currently supported. The "
                                  "contents of pArgumentBuffer may become corrupted.");
    }

    // Add resource value offsets to resource_value_infos based on the command signature's arguments.
    uint64_t command_byte_offset = argument_buffer_offset;
    for (UINT i = 0; i < max_command_count; ++i)
    {
        for (const auto& resource_value_info : command_signature_extra_info->resource_value_infos)
        {
            resource_value_infos.insert({ resource_value_info.offset + command_byte_offset, resource_value_info.type });
        }
        command_byte_offset += command_signature_extra_info->byte_stride;
    }
}

void Dx12ResourceValueMapper::ProcessResourceMappings(ProcessResourceMappingsArgs args)
{
    GFXRECON_ASSERT(args.fence != nullptr);

    // Wait for the command queue before mapping.
    if (args.fence->GetCompletedValue() == UINT64_MAX)
    {
        GFXRECON_LOG_FATAL(
            "Invalid fence value (UINT64_MAX). Device may have been removed. GFXR is unable to continue.");
        return;
    }
    if (args.fence->GetCompletedValue() < args.fence_value)
    {
        ResetEvent(args.fence_event);
        args.fence->SetEventOnCompletion(args.fence_value, args.fence_event);
        WaitForSingleObject(args.fence_event, INFINITE);
    }

    // Apply the resource value mappings to the resources on the GPU.
    std::map<DxObjectInfo*, MappedResourceRevertInfo> resource_data_to_revert;
    MapResources(args.resource_value_info_map, resource_data_to_revert);

    // Signal to the command queue that the mapping is completed.
    args.fence->Signal(args.fence_value + 1);

    // Wait for the command queue before performing resource mapping.
    if (args.fence->GetCompletedValue() < args.fence_value + 2)
    {
        ResetEvent(args.fence_event);
        args.fence->SetEventOnCompletion(args.fence_value + 2, args.fence_event);
        WaitForSingleObject(args.fence_event, INFINITE);
    }

    // Revert the resource values to their unmapped values.
    for (auto& resource_data_pair : resource_data_to_revert)
    {
        GFXRECON_ASSERT(!resource_data_pair.second.data.empty());
        HRESULT hr =
            resource_data_util_->WriteToResource(static_cast<ID3D12Resource*>(resource_data_pair.first->object),
                                                 true,
                                                 resource_data_pair.second.states,
                                                 resource_data_pair.second.states,
                                                 resource_data_pair.second.data,
                                                 { 0 },
                                                 { resource_data_pair.second.data.size() });
        if (SUCCEEDED(hr))
        {
            // The mapped values were reverted in the resource data, so revert the set of mapped values here.
            auto extra_info                  = GetResourceExtraInfo(resource_data_pair.first);
            extra_info->mapped_gpu_addresses = std::move(resource_data_pair.second.mapped_gpu_addresses);
        }
        else
        {
            GFXRECON_LOG_ERROR("Failed to revert data for mapped resource %" PRIu64,
                               resource_data_pair.first->capture_id);
        }
    }

    // Signal to the command queue that the mapping is completed.
    args.fence->Signal(args.fence_value + 3);
}

void Dx12ResourceValueMapper::MapValue(const ResourceValueInfo& value_info,
                                       std::vector<uint8_t>&    result_data,
                                       D3D12ResourceInfo*       resource_info)
{
    if ((value_info.type == ResourceValueType::kGpuVirtualAddress) ||
        (value_info.type == ResourceValueType::kGpuDescriptorHandle))
    {
        D3D12_GPU_VIRTUAL_ADDRESS* address =
            reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS*>(result_data.data() + value_info.offset);
        auto current_address = *address;

        if (current_address == 0)
        {
            return;
        }

        // If the current value at the given offset matches the result of a previous mapping, don't attempt to map
        // again.
        auto mapped_value_iter = resource_info->mapped_gpu_addresses.find(value_info.offset);
        if (mapped_value_iter != resource_info->mapped_gpu_addresses.end())
        {
            if (current_address == mapped_value_iter->second)
            {
                return;
            }
        }

        if (value_info.type == ResourceValueType::kGpuVirtualAddress)
        {
            map_gpu_va_func_(*address);

            if ((*address) == current_address)
            {
                GFXRECON_LOG_WARNING_ONCE("Mapping a GPU virtual address resulted in no change.");
            }
            else
            {
                // Track the mapped value.
                resource_info->mapped_gpu_addresses[value_info.offset] = *address;
            }
        }
        else if (value_info.type == ResourceValueType::kGpuDescriptorHandle)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE descriptor_handle;
            descriptor_handle.ptr = *address;
            map_gpu_desc_handle_func_(descriptor_handle);
            *address = descriptor_handle.ptr;

            if ((*address) == current_address)
            {
                GFXRECON_LOG_WARNING_ONCE("Mapping a GPU descriptor handle resulted in no change.");
            }
            else
            {
                // Track the mapped value.
                resource_info->mapped_gpu_addresses[value_info.offset] = *address;
            }
        }
    }
    else
    {
        GFXRECON_ASSERT(false && "Unrecognized resource value type.");
    }
}

void Dx12ResourceValueMapper::MapResources(const ResourceValueInfoMap&                        resource_value_info_map,
                                           std::map<DxObjectInfo*, MappedResourceRevertInfo>& resource_data_to_revert)
{
    for (const auto& resource_value_infos : resource_value_info_map)
    {
        const auto& value_infos = resource_value_infos.second;
        if (value_infos.empty())
        {
            continue;
        }

        auto resource_object_info = resource_value_infos.first;
        auto resource             = static_cast<ID3D12Resource*>(resource_object_info->object);

        D3D12_HEAP_PROPERTIES heap_properties;
        D3D12_HEAP_FLAGS      heap_flags;
        HRESULT               result = resource->GetHeapProperties(&heap_properties, &heap_flags);
        temp_resource_states.clear();
        if (heap_properties.Type == D3D12_HEAP_TYPE_UPLOAD)
        {
            // If the resource is on an upload heap, it cannot be transitioned away from
            // D3D12_RESOURCE_STATE_GENERIC_READ.
            temp_resource_states.push_back({ D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_BARRIER_FLAG_NONE });
        }
        else if (heap_properties.Type == D3D12_HEAP_TYPE_READBACK)
        {
            // If the resource is on an readback heap, it cannot be transitioned away from
            // D3D12_RESOURCE_STATE_COPY_DEST.
            temp_resource_states.push_back({ D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_FLAG_NONE });
        }
        else
        {
            // TODO (GH #415): Track resource state transitions for resources on other heap types.
            temp_resource_states.push_back({ D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_BARRIER_FLAG_NONE });
        }

        temp_resource_data.clear();
        temp_resource_sizes.clear();
        temp_resource_offsets.clear();
        HRESULT hr = resource_data_util_->ReadFromResource(resource,
                                                           true,
                                                           temp_resource_states,
                                                           temp_resource_states,
                                                           temp_resource_data,
                                                           temp_resource_sizes,
                                                           temp_resource_offsets);

        if (SUCCEEDED(hr))
        {
            auto resource_extra_info = GetResourceExtraInfo(resource_object_info);

            MappedResourceRevertInfo revert_info;
            revert_info.data                                    = temp_resource_data;
            revert_info.states                                  = temp_resource_states;
            revert_info.mapped_gpu_addresses                    = resource_extra_info->mapped_gpu_addresses;
            resource_data_to_revert[resource_value_infos.first] = std::move(revert_info);

            for (const auto& value_info : value_infos)
            {
                MapValue(value_info, temp_resource_data, resource_extra_info);
            }

            hr = resource_data_util_->WriteToResource(resource,
                                                      true,
                                                      temp_resource_states,
                                                      temp_resource_states,
                                                      temp_resource_data,
                                                      temp_resource_sizes,
                                                      temp_resource_offsets);

            if (FAILED(hr))
            {
                GFXRECON_LOG_ERROR("Failed to write data for mapping values in resource (id=%" PRIu64 ")",
                                   resource_object_info->capture_id);
            }
        }
        else
        {
            GFXRECON_LOG_ERROR("Failed to read data for mapping values in resource (id=%" PRIu64 ")",
                               resource_object_info->capture_id);
        }
    }
}

void Dx12ResourceValueMapper::InitializeRequiredObjects(ID3D12CommandQueue*    command_queue,
                                                        D3D12CommandQueueInfo* command_queue_extra_info)
{
    graphics::dx12::ID3D12DeviceComPtr device = nullptr;
    HRESULT                            result = command_queue->GetDevice(IID_PPV_ARGS(&device));
    GFXRECON_ASSERT(SUCCEEDED(result));

    // Create the fence for synchronizing resource mapping.
    auto fence_result = device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&command_queue_extra_info->resource_value_map_fence));

    if (SUCCEEDED(fence_result))
    {
        command_queue_extra_info->resource_value_map_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        command_queue_extra_info->resource_value_map_fence_info.object =
            command_queue_extra_info->resource_value_map_fence;
        command_queue_extra_info->resource_value_map_fence_info.extra_info = std::make_unique<D3D12FenceInfo>();
    }
    else
    {
        GFXRECON_LOG_ERROR("Failed to create ID3D12Fence object for synchronizing the mapping for resource values with "
                           "command list execution.");
    }

    if (resource_data_util_ == nullptr)
    {
        resource_data_util_ = std::make_unique<graphics::Dx12ResourceDataUtil>(device, 0);
    }
}

QueueSyncEventInfo
Dx12ResourceValueMapper::CreateProcessProcessResourceMappingsSyncEvent(ProcessResourceMappingsArgs args)
{
    return QueueSyncEventInfo{ false, false, nullptr, 0, [this, captured_args = std::move(args)]() {
                                  ProcessResourceMappings(captured_args);
                              } };
}

GFXRECON_END_NAMESPACE(decode)
GFXRECON_END_NAMESPACE(gfxrecon)
