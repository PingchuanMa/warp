/** Copyright (c) 2022 NVIDIA CORPORATION.  All rights reserved.
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "volume.h"
#include "warp.h"
#include "cuda_util.h"

#include <map>

using namespace wp;

namespace
{

struct VolumeDesc
{
    // NanoVDB buffer either in device or host memory
    void* buffer; 
    uint64_t size_in_bytes;

    // offset to the voxel values of the first leaf node realtive to buffer
    uint64_t first_voxel_data_offs;

    // copy of the grids's metadata to keep on the host for device volumes
    pnanovdb_grid_t grid_data;

    // copy of the tree's metadata to keep on the host for device volumes
    pnanovdb_tree_t tree_data;

    // CUDA context for this volume (NULL if CPU)
    void* context;
};

// Host-side volume desciptors. Maps each CPU/GPU volume buffer address (id) to a CPU desc
std::map<uint64_t, VolumeDesc> g_volume_descriptors;

bool volume_get_descriptor(uint64_t id, VolumeDesc& volumeDesc)
{
    if (id == 0) return false;

    const auto& iter = g_volume_descriptors.find(id);
    if (iter == g_volume_descriptors.end())
        return false;
    else
        volumeDesc = iter->second;
    return true;
}

void volume_add_descriptor(uint64_t id, const VolumeDesc& volumeDesc)
{
    g_volume_descriptors[id] = volumeDesc;
}

void volume_rem_descriptor(uint64_t id)
{
    g_volume_descriptors.erase(id);
}

} // anonymous namespace


// NB: buf must be a host pointer
uint64_t volume_create_host(void* buf, uint64_t size)
{
    if (size < sizeof(pnanovdb_grid_t) + sizeof(pnanovdb_tree_t))
        return 0;  // This cannot be a valid NanoVDB grid with data

    VolumeDesc volume;

    volume.context = NULL;

    memcpy_h2h(&volume.grid_data, buf, sizeof(pnanovdb_grid_t));
    memcpy_h2h(&volume.tree_data, (pnanovdb_grid_t*)buf + 1, sizeof(pnanovdb_tree_t));

    if (volume.grid_data.magic != PNANOVDB_MAGIC_NUMBER)
        return 0;

    volume.size_in_bytes = size;
    volume.buffer = alloc_host(size);
    memcpy_h2h(volume.buffer, buf, size);

    volume.first_voxel_data_offs =
        sizeof(pnanovdb_grid_t) + volume.tree_data.node_offset_leaf + PNANOVDB_GRID_TYPE_GET(PNANOVDB_GRID_TYPE_FLOAT, leaf_off_table);

    uint64_t id = (uint64_t)volume.buffer;

    volume_add_descriptor(id, volume);

    return id;
}

// NB: buf must be a pointer on the same device
uint64_t volume_create_device(void* context, void* buf, uint64_t size)
{
    if (size < sizeof(pnanovdb_grid_t) + sizeof(pnanovdb_tree_t))
        return 0;  // This cannot be a valid NanoVDB grid with data

    ContextGuard guard(context);

    VolumeDesc volume;

    volume.context = context ? context : cuda_context_get_current();

    memcpy_d2h(WP_CURRENT_CONTEXT, &volume.grid_data, buf, sizeof(pnanovdb_grid_t));
    memcpy_d2h(WP_CURRENT_CONTEXT, &volume.tree_data, (pnanovdb_grid_t*)buf + 1, sizeof(pnanovdb_tree_t));

    if (volume.grid_data.magic != PNANOVDB_MAGIC_NUMBER)
        return 0;

    volume.size_in_bytes = size;
    volume.buffer = alloc_device(WP_CURRENT_CONTEXT, size);
    memcpy_d2d(WP_CURRENT_CONTEXT, volume.buffer, buf, size);

    volume.first_voxel_data_offs =
        sizeof(pnanovdb_grid_t) + volume.tree_data.node_offset_leaf + PNANOVDB_GRID_TYPE_GET(PNANOVDB_GRID_TYPE_FLOAT, leaf_off_table);

    uint64_t id = (uint64_t)volume.buffer;

    volume_add_descriptor(id, volume);

    return id;
}


static void volume_get_buffer_info(uint64_t id, void** buf, uint64_t* size)
{
    *buf = 0;
    *size = 0;

    VolumeDesc volume;
    if (volume_get_descriptor(id, volume))
    {
        *buf = volume.buffer;
        *size = volume.size_in_bytes;
    }
}

void volume_get_buffer_info_host(uint64_t id, void** buf, uint64_t* size)
{
    volume_get_buffer_info(id, buf, size);
}

void volume_get_buffer_info_device(uint64_t id, void** buf, uint64_t* size)
{
    volume_get_buffer_info(id, buf, size);
}

void volume_destroy_host(uint64_t id)
{
    free_host((void*)id);
    volume_rem_descriptor(id);
}

void volume_destroy_device(uint64_t id)
{
    VolumeDesc volume;
    if (volume_get_descriptor(id, volume))
    {
        ContextGuard guard(volume.context);
        free_device(WP_CURRENT_CONTEXT, volume.buffer);
        volume_rem_descriptor(id);
    }
}
