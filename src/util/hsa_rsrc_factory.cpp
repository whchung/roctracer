/**********************************************************************
Copyright ©2013 Advanced Micro Devices, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

<95>    Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.
<95>    Redistributions in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
********************************************************************/

#include "util/hsa_rsrc_factory.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <hsa.h>
#include <hsa_ext_amd.h>
#include <hsa_ext_finalize.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef AQL_PROFILE_READ_API_ENABLE
#define AQL_PROFILE_READ_API_ENABLE 0
#endif

namespace roctracer {
namespace util {

// Callback function to get available in the system agents
hsa_status_t HsaRsrcFactory::GetHsaAgentsCallback(hsa_agent_t agent, void* data) {
  hsa_status_t status = HSA_STATUS_ERROR;
  HsaRsrcFactory* hsa_rsrc = reinterpret_cast<HsaRsrcFactory*>(data);
  const AgentInfo* agent_info = hsa_rsrc->AddAgentInfo(agent);
  if (agent_info != NULL) status = HSA_STATUS_SUCCESS;
  return status;
}

// This function checks to see if the provided
// pool has the HSA_AMD_SEGMENT_GLOBAL property. If the kern_arg flag is true,
// the function adds an additional requirement that the pool have the
// HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT property. If kern_arg is false,
// pools must NOT have this property.
// Upon finding a pool that meets these conditions, HSA_STATUS_INFO_BREAK is
// returned. HSA_STATUS_SUCCESS is returned if no errors were encountered, but
// no pool was found meeting the requirements. If an error is encountered, we
// return that error.
static hsa_status_t
FindGlobalPool(hsa_amd_memory_pool_t pool, void* data, bool kern_arg) {
  hsa_status_t err;
  hsa_amd_segment_t segment;
  uint32_t flag;

  if (nullptr == data) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  err = hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT,
                                     &segment);
  CHECK_STATUS("hsa_amd_memory_pool_get_info", err);
  if (HSA_AMD_SEGMENT_GLOBAL != segment) {
    return HSA_STATUS_SUCCESS;
  }

  err = hsa_amd_memory_pool_get_info(pool,
                                HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flag);
  CHECK_STATUS("hsa_amd_memory_pool_get_info", err);

  uint32_t karg_st = flag & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT;

  if ((karg_st == 0 && kern_arg) ||
      (karg_st != 0 && !kern_arg)) {
    return HSA_STATUS_SUCCESS;
  }

  *(reinterpret_cast<hsa_amd_memory_pool_t*>(data)) = pool;
  return HSA_STATUS_INFO_BREAK;
}

// This is the call-back function for hsa_amd_agent_iterate_memory_pools() that
// finds a pool with the properties of HSA_AMD_SEGMENT_GLOBAL and that is NOT
// HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT
hsa_status_t FindStandardPool(hsa_amd_memory_pool_t pool, void* data) {
  return FindGlobalPool(pool, data, false);
}

// This is the call-back function for hsa_amd_agent_iterate_memory_pools() that
// finds a pool with the properties of HSA_AMD_SEGMENT_GLOBAL and that IS
// HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT
hsa_status_t FindKernArgPool(hsa_amd_memory_pool_t pool, void* data) {
  return FindGlobalPool(pool, data, true);
}
#if 0
// Callback function to find and bind kernarg region of an agent
hsa_status_t HsaRsrcFactory::FindMemRegionsCallback(hsa_region_t region, void* data) {
  hsa_region_global_flag_t flags;
  hsa_region_segment_t segment_id;

  hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment_id);
  if (segment_id != HSA_REGION_SEGMENT_GLOBAL) {
    return HSA_STATUS_SUCCESS;
  }

  AgentInfo* agent_info = (AgentInfo*)data;
  hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
  if (flags & HSA_REGION_GLOBAL_FLAG_COARSE_GRAINED) {
    agent_info->coarse_region = region;
  }

  if (flags & HSA_REGION_GLOBAL_FLAG_KERNARG) {
    agent_info->kernarg_region = region;
  }

  return HSA_STATUS_SUCCESS;
}
#endif
// Constructor of the class
HsaRsrcFactory::HsaRsrcFactory(bool initialize_hsa) : initialize_hsa_(initialize_hsa) {
  hsa_status_t status;
  // Initialize the Hsa Runtime
  if (initialize_hsa_) {
    status = hsa_init();
    CHECK_STATUS("Error in hsa_init", status);
  }
  // Discover the set of Gpu devices available on the platform
  status = hsa_iterate_agents(GetHsaAgentsCallback, this);
  CHECK_STATUS("Error Calling hsa_iterate_agents", status);

  // Get AqlProfile API table
  aqlprofile_api_ = {0};
#ifdef ROCP_LD_AQLPROFILE
  status = LoadAqlProfileLib(&aqlprofile_api_);
#else
  status = hsa_system_get_extension_table(HSA_EXTENSION_AMD_AQLPROFILE, 1, 0, &aqlprofile_api_);
#endif
  CHECK_STATUS("aqlprofile API table load failed", status);

  // Get Loader API table
  loader_api_ = {0};
  status = hsa_system_get_extension_table(HSA_EXTENSION_AMD_LOADER, 1, 0, &loader_api_);
  CHECK_STATUS("loader API table query failed", status);
}

// Destructor of the class
HsaRsrcFactory::~HsaRsrcFactory() {
  for (auto p : cpu_list_) delete p;
  for (auto p : gpu_list_) delete p;
  if (initialize_hsa_) {
    hsa_status_t status = hsa_shut_down();
    CHECK_STATUS("Error in hsa_shut_down", status);
  }
}

hsa_status_t HsaRsrcFactory::LoadAqlProfileLib(aqlprofile_pfn_t* api) {
    void* handle = dlopen(kAqlProfileLib, RTLD_NOW);
    if (handle == NULL) {
      fprintf(stderr, "Loading '%s' failed, %s\n", kAqlProfileLib, dlerror());
      return HSA_STATUS_ERROR;
    }
    dlerror(); /* Clear any existing error */

    api->hsa_ven_amd_aqlprofile_error_string =
      (decltype(::hsa_ven_amd_aqlprofile_error_string)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_error_string");
    api->hsa_ven_amd_aqlprofile_validate_event =
      (decltype(::hsa_ven_amd_aqlprofile_validate_event)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_validate_event");
    api->hsa_ven_amd_aqlprofile_start =
      (decltype(::hsa_ven_amd_aqlprofile_start)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_start");
    api->hsa_ven_amd_aqlprofile_stop =
      (decltype(::hsa_ven_amd_aqlprofile_stop)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_stop");
#if AQL_PROFILE_READ_API_ENABLE
    api->hsa_ven_amd_aqlprofile_read =
      (decltype(::hsa_ven_amd_aqlprofile_read)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_read");
#endif  // AQL_PROFILE_READ_API_ENABLE
    api->hsa_ven_amd_aqlprofile_legacy_get_pm4 =
      (decltype(::hsa_ven_amd_aqlprofile_legacy_get_pm4)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_legacy_get_pm4");
    api->hsa_ven_amd_aqlprofile_get_info =
      (decltype(::hsa_ven_amd_aqlprofile_get_info)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_get_info");
    api->hsa_ven_amd_aqlprofile_iterate_data =
      (decltype(::hsa_ven_amd_aqlprofile_iterate_data)*)
        dlsym(handle, "hsa_ven_amd_aqlprofile_iterate_data");

  return HSA_STATUS_SUCCESS;
}

// Add system agent info
const AgentInfo* HsaRsrcFactory::AddAgentInfo(const hsa_agent_t agent) {
  // Determine if device is a Gpu agent
  hsa_status_t status;
  AgentInfo* agent_info = NULL;

  hsa_device_type_t type;
  status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
  CHECK_STATUS("Error Calling hsa_agent_get_info", status);

  if (type == HSA_DEVICE_TYPE_CPU) {
    agent_info = new AgentInfo{};
    agent_info->dev_id = agent;
    agent_info->dev_type = HSA_DEVICE_TYPE_CPU;
    agent_info->dev_index = cpu_list_.size();

    status = hsa_amd_agent_iterate_memory_pools(agent, FindStandardPool, &agent_info->cpu_pool);
    CHECK_ITER_STATUS("hsa_amd_agent_iterate_memory_pools(cpu pool)", status);
    status = hsa_amd_agent_iterate_memory_pools(agent, FindKernArgPool, &agent_info->kern_arg_pool);
    CHECK_ITER_STATUS("hsa_amd_agent_iterate_memory_pools(kern arg pool)", status);
    agent_info->gpu_pool = {};

    cpu_list_.push_back(agent_info);
    cpu_agents_.push_back(agent);
  }

  if (type == HSA_DEVICE_TYPE_GPU) {
    agent_info = new AgentInfo{};
    agent_info->dev_id = agent;
    agent_info->dev_type = HSA_DEVICE_TYPE_GPU;
    hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, agent_info->name);
    strncpy(agent_info->gfxip, agent_info->name, 4);
    agent_info->gfxip[4] = '\0';
    hsa_agent_get_info(agent, HSA_AGENT_INFO_WAVEFRONT_SIZE, &agent_info->max_wave_size);
    hsa_agent_get_info(agent, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &agent_info->max_queue_size);
    hsa_agent_get_info(agent, HSA_AGENT_INFO_PROFILE, &agent_info->profile);
    agent_info->is_apu = (agent_info->profile == HSA_PROFILE_FULL) ? true : false;
    hsa_agent_get_info(agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT), &agent_info->cu_num);
    hsa_agent_get_info(agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU), &agent_info->waves_per_cu);
    hsa_agent_get_info(agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU), &agent_info->simds_per_cu);
    hsa_agent_get_info(agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES), &agent_info->se_num);
    hsa_agent_get_info(agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_NUM_SHADER_ARRAYS_PER_SE), &agent_info->shader_arrays_per_se);

    agent_info->cpu_pool = {};
    agent_info->kern_arg_pool = {};
    status = hsa_amd_agent_iterate_memory_pools(agent, FindStandardPool, &agent_info->gpu_pool);
    CHECK_ITER_STATUS("hsa_amd_agent_iterate_memory_pools(gpu pool)", status);
#if 0
    // Initialize memory regions to zero
    agent_info->kernarg_region.handle = 0;
    agent_info->coarse_region.handle = 0;
    // Find and Bind Memory regions of the Gpu agent
    hsa_agent_iterate_regions(agent, FindMemRegionsCallback, agent_info);
#endif

    // Set GPU index
    agent_info->dev_index = gpu_list_.size();
    gpu_list_.push_back(agent_info);
    gpu_agents_.push_back(agent);
  }

  if (agent_info) agent_map_[agent.handle] = agent_info;

  return agent_info;
}

// Return systen agent info
const AgentInfo* HsaRsrcFactory::GetAgentInfo(const hsa_agent_t agent) {
  const AgentInfo* agent_info = NULL;
  auto it = agent_map_.find(agent.handle);
  if (it != agent_map_.end()) {
    agent_info = it->second;
  }
  return agent_info;
}

// Get the count of Hsa Gpu Agents available on the platform
//
// @return uint32_t Number of Gpu agents on platform
//
uint32_t HsaRsrcFactory::GetCountOfGpuAgents() { return uint32_t(gpu_list_.size()); }

// Get the count of Hsa Cpu Agents available on the platform
//
// @return uint32_t Number of Cpu agents on platform
//
uint32_t HsaRsrcFactory::GetCountOfCpuAgents() { return uint32_t(cpu_list_.size()); }

// Get the AgentInfo handle of a Gpu device
//
// @param idx Gpu Agent at specified index
//
// @param agent_info Output parameter updated with AgentInfo
//
// @return bool true if successful, false otherwise
//
bool HsaRsrcFactory::GetGpuAgentInfo(uint32_t idx, const AgentInfo** agent_info) {
  // Determine if request is valid
  uint32_t size = uint32_t(gpu_list_.size());
  if (idx >= size) {
    return false;
  }

  // Copy AgentInfo from specified index
  *agent_info = gpu_list_[idx];

  return true;
}

// Get the AgentInfo handle of a Cpu device
//
// @param idx Cpu Agent at specified index
//
// @param agent_info Output parameter updated with AgentInfo
//
// @return bool true if successful, false otherwise
//
bool HsaRsrcFactory::GetCpuAgentInfo(uint32_t idx, const AgentInfo** agent_info) {
  // Determine if request is valid
  uint32_t size = uint32_t(cpu_list_.size());
  if (idx >= size) {
    return false;
  }

  // Copy AgentInfo from specified index
  *agent_info = cpu_list_[idx];
  return true;
}

// Create a Queue object and return its handle. The queue object is expected
// to support user requested number of Aql dispatch packets.
//
// @param agent_info Gpu Agent on which to create a queue object
//
// @param num_Pkts Number of packets to be held by queue
//
// @param queue Output parameter updated with handle of queue object
//
// @return bool true if successful, false otherwise
//
bool HsaRsrcFactory::CreateQueue(const AgentInfo* agent_info, uint32_t num_pkts,
                                 hsa_queue_t** queue) {
  hsa_status_t status;
  status = hsa_queue_create(agent_info->dev_id, num_pkts, HSA_QUEUE_TYPE_MULTI, NULL, NULL,
                            UINT32_MAX, UINT32_MAX, queue);
  return (status == HSA_STATUS_SUCCESS);
}

// Create a Signal object and return its handle.
// @param value Initial value of signal object
// @param signal Output parameter updated with handle of signal object
// @return bool true if successful, false otherwise
bool HsaRsrcFactory::CreateSignal(uint32_t value, hsa_signal_t* signal) {
  hsa_status_t status;
  status = hsa_signal_create(value, 0, NULL, signal);
  return (status == HSA_STATUS_SUCCESS);
}

// Allocate memory for use by a kernel of specified size in specified
// agent's memory region.
// @param agent_info Agent from whose memory region to allocate
// @param size Size of memory in terms of bytes
// @return uint8_t* Pointer to buffer, null if allocation fails.
uint8_t* HsaRsrcFactory::AllocateLocalMemory(const AgentInfo* agent_info, size_t size) {
  hsa_status_t status = HSA_STATUS_ERROR;
  uint8_t* buffer = NULL;
  size = (size + MEM_PAGE_MASK) & ~MEM_PAGE_MASK;
  status = hsa_amd_memory_pool_allocate(agent_info->gpu_pool, size, 0, (void**)&buffer);
  // Only GPU can access the memory
  if (status == HSA_STATUS_SUCCESS) {
    hsa_agent_t agents_list[1] = {agent_info->dev_id};
    status = hsa_amd_agents_allow_access(1, agents_list, NULL, buffer);
  }
  uint8_t* ptr = (status == HSA_STATUS_SUCCESS) ? buffer : NULL;
  printf("AllocateLocalMemory %p\n", ptr);
  return ptr;
}

// Allocate memory to pass kernel parameters.
// Memory is alocated accessible for all CPU agents and for GPU given by AgentInfo parameter.
// @param agent_info Agent from whose memory region to allocate
// @param size Size of memory in terms of bytes
// @return uint8_t* Pointer to buffer, null if allocation fails.
uint8_t* HsaRsrcFactory::AllocateKernArgMemory(const AgentInfo* agent_info, size_t size) {
  hsa_status_t status = HSA_STATUS_ERROR;
  uint8_t* buffer = NULL;
  if (!cpu_agents_.empty()) {
    size = (size + MEM_PAGE_MASK) & ~MEM_PAGE_MASK;
    status = hsa_amd_memory_pool_allocate(cpu_list_[0]->kern_arg_pool, size, 0, (void**)&buffer);
    // Both the CPU and GPU can access the kernel arguments
    if (status == HSA_STATUS_SUCCESS) {
      auto agents_vec = cpu_agents_;
      agents_vec.push_back(agent_info->dev_id);
      status = hsa_amd_agents_allow_access(agents_vec.size(), &agents_vec[0], NULL, buffer);
    }
  }
  uint8_t* ptr = (status == HSA_STATUS_SUCCESS) ? buffer : NULL;
  printf("AllocateKernargMemory %p\n", ptr);
  return ptr;
}

// Allocate system memory accessible by both CPU and GPU
// @param agent_info Agent from whose memory region to allocate
// @param size Size of memory in terms of bytes
// @return uint8_t* Pointer to buffer, null if allocation fails.
uint8_t* HsaRsrcFactory::AllocateSysMemory(const AgentInfo* agent_info, size_t size) {
  hsa_status_t status = HSA_STATUS_ERROR;
  uint8_t* buffer = NULL;
  if (!cpu_agents_.empty()) {
    size = (size + MEM_PAGE_MASK) & ~MEM_PAGE_MASK;
    status = hsa_amd_memory_pool_allocate(cpu_list_[0]->cpu_pool, size, 0, (void**)&buffer);
    // Both the CPU and GPU can access the memory
    if (status == HSA_STATUS_SUCCESS) {
      auto agents_vec = cpu_agents_;
      agents_vec.push_back(agent_info->dev_id);
      status = hsa_amd_agents_allow_access(agents_vec.size(), &agents_vec[0], NULL, buffer);
    }
  }
  uint8_t* ptr = (status == HSA_STATUS_SUCCESS) ? buffer : NULL;
  printf("AllocateSysMemory %p\n", ptr);
  return ptr;
}

// Copy data from GPU to host memory
bool HsaRsrcFactory::CopyToHost(const hsa_agent_t& agent, void* dst, const void* src, size_t size) {
  hsa_status_t status = HSA_STATUS_ERROR;
  if (!cpu_agents_.empty()) {
    hsa_signal_t s = {};
    hsa_status_t status = hsa_signal_create(1, 0, NULL, &s);
    if (status == HSA_STATUS_SUCCESS) {
      status = hsa_amd_memory_async_copy(dst, cpu_agents_[0], src, agent, size, 0, NULL, s);
      if (status == HSA_STATUS_SUCCESS) {
        if (hsa_signal_wait_scacquire(s, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX, HSA_WAIT_STATE_BLOCKED) != 0) {
          status = HSA_STATUS_ERROR;
        }
      }
      status = hsa_signal_destroy(s);
    }
  }
  return (status == HSA_STATUS_SUCCESS);
}
bool HsaRsrcFactory::CopyToHost(const AgentInfo* agent_info, void* dst, const void* src, size_t size) {
  return CopyToHost(agent_info->dev_id, dst, src, size);
}

// Loads an Assembled Brig file and Finalizes it into Device Isa
// @param agent_info Gpu device for which to finalize
// @param brig_path File path of the Assembled Brig file
// @param kernel_name Name of the kernel to finalize
// @param code_desc Handle of finalized Code Descriptor that could
// be used to submit for execution
// @return bool true if successful, false otherwise
bool HsaRsrcFactory::LoadAndFinalize(const AgentInfo* agent_info, const char* brig_path,
                                      const char* kernel_name, hsa_executable_t* executable, hsa_executable_symbol_t* code_desc) {
  hsa_status_t status = HSA_STATUS_ERROR;

  // Build the code object filename
  std::string filename(brig_path);
  std::clog << "Code object filename: " << filename << std::endl;

  // Open the file containing code object
  hsa_file_t file_handle = open(filename.c_str(), O_RDONLY);
  if (file_handle == -1) {
    std::cerr << "Error: failed to load '" << filename << "'" << std::endl;
    assert(false);
    return false;
  }

  // Create code object reader
  hsa_code_object_reader_t code_obj_rdr = {0};
  status = hsa_code_object_reader_create_from_file(file_handle, &code_obj_rdr);
  if (status != HSA_STATUS_SUCCESS) {
    std::cerr << "Failed to create code object reader '" << filename << "'" << std::endl;
    return false;
  }

  // Create executable.
  status = hsa_executable_create_alt(HSA_PROFILE_FULL,
    HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, NULL, executable);
  CHECK_STATUS("Error in creating executable object", status);

  // Load code object.
  status = hsa_executable_load_agent_code_object(*executable, agent_info->dev_id,
    code_obj_rdr, NULL, NULL);
  CHECK_STATUS("Error in loading executable object", status);

  // Freeze executable.
  status = hsa_executable_freeze(*executable, "");
  CHECK_STATUS("Error in freezing executable object", status);

  // Get symbol handle.
  hsa_executable_symbol_t kernelSymbol;
  status = hsa_executable_get_symbol(*executable, NULL, kernel_name, agent_info->dev_id, 0,
                                     &kernelSymbol);
  CHECK_STATUS("Error in looking up kernel symbol", status);

  // Update output parameter
  *code_desc = kernelSymbol;
  return true;
}

// Print the various fields of Hsa Gpu Agents
bool HsaRsrcFactory::PrintGpuAgents(const std::string& header) {
  std::clog << header << " :" << std::endl;

  const AgentInfo* agent_info;
  int size = uint32_t(gpu_list_.size());
  for (int idx = 0; idx < size; idx++) {
    agent_info = gpu_list_[idx];

    std::clog << "> agent[" << idx << "] :" << std::endl;
    std::clog << ">> Name : " << agent_info->name << std::endl;
    std::clog << ">> APU : " << agent_info->is_apu << std::endl;
    std::clog << ">> HSAIL profile : " << agent_info->profile << std::endl;
    std::clog << ">> Max Wave Size : " << agent_info->max_wave_size << std::endl;
    std::clog << ">> Max Queue Size : " << agent_info->max_queue_size << std::endl;
//    std::clog << ">> Kernarg Region Id : " << agent_info->coarse_region.handle << std::endl;
    std::clog << ">> CU number : " << agent_info->cu_num << std::endl;
    std::clog << ">> Waves per CU : " << agent_info->waves_per_cu << std::endl;
    std::clog << ">> SIMDs per CU : " << agent_info->simds_per_cu << std::endl;
    std::clog << ">> SE number : " << agent_info->se_num << std::endl;
    std::clog << ">> Shader Arrays per SE : " << agent_info->shader_arrays_per_se << std::endl;
  }
  return true;
}

uint64_t HsaRsrcFactory::Submit(hsa_queue_t* queue, void* packet) {
  const uint32_t slot_size_b = 0x40;

  // adevance command queue
  const uint64_t write_idx = hsa_queue_load_write_index_relaxed(queue);
  hsa_queue_store_write_index_relaxed(queue, write_idx + 1);
  while ((write_idx - hsa_queue_load_read_index_relaxed(queue)) >= queue->size) {
    sched_yield();
  }

  uint32_t slot_idx = (uint32_t)(write_idx % queue->size);
  uint32_t* queue_slot = (uint32_t*)((uintptr_t)(queue->base_address) + (slot_idx * slot_size_b));
  uint32_t* slot_data = (uint32_t*)packet;

  // Copy buffered commands into the queue slot.
  // Overwrite the AQL invalid header (first dword) last.
  // This prevents the slot from being read until it's fully written.
  memcpy(&queue_slot[1], &slot_data[1], slot_size_b - sizeof(uint32_t));
  std::atomic<uint32_t>* header_atomic_ptr = reinterpret_cast<std::atomic<uint32_t>*>(&queue_slot[0]);
  header_atomic_ptr->store(slot_data[0], std::memory_order_release);

  // ringdoor bell
  hsa_signal_store_relaxed(queue->doorbell_signal, write_idx);

  return write_idx;
}

HsaRsrcFactory* HsaRsrcFactory::instance_ = NULL;
HsaRsrcFactory::mutex_t HsaRsrcFactory::mutex_;

}  // namespace util
}  // namespace roctracer