// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "metal_stream_buffer.h"
#include "metal_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"

Log_SetChannel(MetalDevice);

MetalStreamBuffer::MetalStreamBuffer() = default;

MetalStreamBuffer::~MetalStreamBuffer()
{
  if (IsValid())
    Destroy();
}

bool MetalStreamBuffer::Create(id<MTLDevice> device, u32 size)
{
  @autoreleasepool
  {
    const MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;

    id<MTLBuffer> new_buffer = [device newBufferWithLength:size options:options];
    if (new_buffer == nil)
    {
      ERROR_LOG("Failed to create buffer.");
      return false;
    }

    if (IsValid())
      Destroy();

    // Replace with the new buffer
    m_size = size;
    m_current_offset = 0;
    m_current_gpu_position = 0;
    m_tracked_fences.clear();
    m_buffer = [new_buffer retain];
    m_host_pointer = static_cast<u8*>([new_buffer contents]);
    return true;
  }
}

void MetalStreamBuffer::Destroy()
{
  m_size = 0;
  m_current_offset = 0;
  m_current_gpu_position = 0;
  m_tracked_fences.clear();
  [m_buffer release];
  m_buffer = nil;
  m_host_pointer = nullptr;
}

bool MetalStreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
  const u32 required_bytes = num_bytes + alignment;

  // Check for sane allocations
  if (required_bytes > m_size) [[unlikely]]
  {
    ERROR_LOG("Attempting to allocate {} bytes from a {} byte stream buffer", num_bytes, m_size);
    Panic("Stream buffer overflow");
    return false;
  }

  UpdateGPUPosition();

  // Is the GPU behind or up to date with our current offset?
  if (m_current_offset >= m_current_gpu_position)
  {
    const u32 remaining_bytes = m_size - m_current_offset;
    if (required_bytes <= remaining_bytes)
    {
      // Place at the current position, after the GPU position.
      m_current_offset = Common::AlignUp(m_current_offset, alignment);
      m_current_space = m_size - m_current_offset;
      return true;
    }

    // Check for space at the start of the buffer
    // We use < here because we don't want to have the case of m_current_offset ==
    // m_current_gpu_position. That would mean the code above would assume the
    // GPU has caught up to us, which it hasn't.
    if (required_bytes < m_current_gpu_position)
    {
      // Reset offset to zero, since we're allocating behind the gpu now
      m_current_offset = 0;
      m_current_space = m_current_gpu_position - 1;
      return true;
    }
  }

  // Is the GPU ahead of our current offset?
  if (m_current_offset < m_current_gpu_position)
  {
    // We have from m_current_offset..m_current_gpu_position space to use.
    const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
    if (required_bytes < remaining_bytes)
    {
      // Place at the current position, since this is still behind the GPU.
      m_current_offset = Common::AlignUp(m_current_offset, alignment);
      m_current_space = m_current_gpu_position - m_current_offset - 1;
      return true;
    }
  }

  // Can we find a fence to wait on that will give us enough memory?
  if (WaitForClearSpace(required_bytes))
  {
    const u32 align_diff = Common::AlignUp(m_current_offset, alignment) - m_current_offset;
    m_current_offset += align_diff;
    m_current_space -= align_diff;
    return true;
  }

  // We tried everything we could, and still couldn't get anything. This means that too much space
  // in the buffer is being used by the command buffer currently being recorded. Therefore, the
  // only option is to execute it, and wait until it's done.
  return false;
}

void MetalStreamBuffer::CommitMemory(u32 final_num_bytes)
{
  DebugAssert((m_current_offset + final_num_bytes) <= m_size);
  DebugAssert(final_num_bytes <= m_current_space);

  m_current_offset += final_num_bytes;
  m_current_space -= final_num_bytes;
  UpdateCurrentFencePosition();
}

void MetalStreamBuffer::UpdateCurrentFencePosition()
{
  // Has the offset changed since the last fence?
  const u64 counter = MetalDevice::GetInstance().GetCurrentFenceCounter();
  if (!m_tracked_fences.empty() && m_tracked_fences.back().first == counter)
  {
    // Still haven't executed a command buffer, so just update the offset.
    m_tracked_fences.back().second = m_current_offset;
    return;
  }

  // New buffer, so update the GPU position while we're at it.
  m_tracked_fences.emplace_back(counter, m_current_offset);
}

void MetalStreamBuffer::UpdateGPUPosition()
{
  auto start = m_tracked_fences.begin();
  auto end = start;

  const u64 completed_counter = MetalDevice::GetInstance().GetCompletedFenceCounter();
  while (end != m_tracked_fences.end() && completed_counter >= end->first)
  {
    m_current_gpu_position = end->second;
    ++end;
  }

  if (start != end)
  {
    m_tracked_fences.erase(start, end);
    if (m_current_offset == m_current_gpu_position)
    {
      // GPU is all caught up now.
      m_current_offset = 0;
      m_current_gpu_position = 0;
      m_current_space = m_size;
    }
  }
}

bool MetalStreamBuffer::WaitForClearSpace(u32 num_bytes)
{
  u32 new_offset = 0;
  u32 new_space = 0;
  u32 new_gpu_position = 0;

  auto iter = m_tracked_fences.begin();
  for (; iter != m_tracked_fences.end(); ++iter)
  {
    // Would this fence bring us in line with the GPU?
    // This is the "last resort" case, where a command buffer execution has been forced
    // after no additional data has been written to it, so we can assume that after the
    // fence has been signaled the entire buffer is now consumed.
    u32 gpu_position = iter->second;
    if (m_current_offset == gpu_position)
    {
      new_offset = 0;
      new_space = m_size;
      new_gpu_position = 0;
      break;
    }

    // Assuming that we wait for this fence, are we allocating in front of the GPU?
    if (m_current_offset > gpu_position)
    {
      // This would suggest the GPU has now followed us and wrapped around, so we have from
      // m_current_position..m_size free, as well as and 0..gpu_position.
      const u32 remaining_space_after_offset = m_size - m_current_offset;
      if (remaining_space_after_offset >= num_bytes)
      {
        // Switch to allocating in front of the GPU, using the remainder of the buffer.
        new_offset = m_current_offset;
        new_space = m_size - m_current_offset;
        new_gpu_position = gpu_position;
        break;
      }

      // We can wrap around to the start, behind the GPU, if there is enough space.
      // We use > here because otherwise we'd end up lining up with the GPU, and then the
      // allocator would assume that the GPU has consumed what we just wrote.
      if (gpu_position > num_bytes)
      {
        new_offset = 0;
        new_space = gpu_position - 1;
        new_gpu_position = gpu_position;
        break;
      }
    }
    else
    {
      // We're currently allocating behind the GPU. This would give us between the current
      // offset and the GPU position worth of space to work with. Again, > because we can't
      // align the GPU position with the buffer offset.
      u32 available_space_inbetween = gpu_position - m_current_offset;
      if (available_space_inbetween > num_bytes)
      {
        // Leave the offset as-is, but update the GPU position.
        new_offset = m_current_offset;
        new_space = available_space_inbetween - 1;
        new_gpu_position = gpu_position;
        break;
      }
    }
  }

  // Did any fences satisfy this condition?
  // Has the command buffer been executed yet? If not, the caller should execute it.
  MetalDevice& dev = MetalDevice::GetInstance();
  if (iter == m_tracked_fences.end() || iter->first == dev.GetCurrentFenceCounter())
    return false;

  // Wait until this fence is signaled. This will fire the callback, updating the GPU position.
  dev.WaitForFenceCounter(iter->first);
  m_tracked_fences.erase(m_tracked_fences.begin(), m_current_offset == iter->second ? m_tracked_fences.end() : ++iter);
  m_current_offset = new_offset;
  m_current_space = new_space;
  m_current_gpu_position = new_gpu_position;
  return true;
}
