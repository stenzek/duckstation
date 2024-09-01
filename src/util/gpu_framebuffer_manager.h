// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/assert.h"

#include "gpu_device.h"
#include "gpu_texture.h"

#include <unordered_map>

class GPUFramebufferManagerBase
{
protected:
  struct Key
  {
    GPUTexture* rts[GPUDevice::MAX_RENDER_TARGETS];
    GPUTexture* ds;
    u32 num_rts;
    u32 flags;

    bool operator==(const Key& rhs) const;
    bool operator!=(const Key& rhs) const;

    bool ContainsRT(const GPUTexture* tex) const;
  };

  struct KeyHash
  {
    size_t operator()(const Key& key) const;
  };
};

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
class GPUFramebufferManager : public GPUFramebufferManagerBase
{
public:
  GPUFramebufferManager() = default;
  ~GPUFramebufferManager();

  FBOType Lookup(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags);

  void RemoveReferences(const GPUTexture* tex);
  void RemoveRTReferences(const GPUTexture* tex);
  void RemoveDSReferences(const GPUTexture* tex);

  void Clear();

private:
  using MapType = std::unordered_map<Key, FBOType, KeyHash>;

  MapType m_map;
};

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::~GPUFramebufferManager()
{
  Clear();
}

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
FBOType GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::Lookup(GPUTexture* const* rts, u32 num_rts,
                                                                         GPUTexture* ds, u32 flags)
{
  Key key;
  for (u32 i = 0; i < num_rts; i++)
    key.rts[i] = rts[i];
  for (u32 i = num_rts; i < GPUDevice::MAX_RENDER_TARGETS; i++)
    key.rts[i] = nullptr;
  key.ds = ds;
  key.num_rts = num_rts;
  key.flags = flags;

  auto it = m_map.find(key);
  if (it == m_map.end())
  {
    FBOType fbo = FactoryFunc(rts, num_rts, ds, flags);
    if (!fbo)
      return fbo;

    it = m_map.emplace(key, fbo).first;
  }

  return it->second;
}

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
void GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::RemoveRTReferences(const GPUTexture* tex)
{
  DebugAssert(tex->IsRenderTarget() || tex->IsRWTexture());
  for (auto it = m_map.begin(); it != m_map.end();)
  {
    if (!it->first.ContainsRT(tex))
    {
      ++it;
      continue;
    }

    DestroyFunc(it->second);
    it = m_map.erase(it);
  }
}

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
void GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::RemoveDSReferences(const GPUTexture* tex)
{
  DebugAssert(tex->IsDepthStencil());
  for (auto it = m_map.begin(); it != m_map.end();)
  {
    if (it->first.ds != tex)
    {
      ++it;
      continue;
    }

    DestroyFunc(it->second);
    it = m_map.erase(it);
  }
}

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
void GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::RemoveReferences(const GPUTexture* tex)
{
  if (tex->IsRenderTarget())
    RemoveRTReferences(tex);
  else if (tex->IsDepthStencil())
    RemoveDSReferences(tex);
}

template<typename FBOType, FBOType (*FactoryFunc)(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags),
         void (*DestroyFunc)(FBOType fbo)>
void GPUFramebufferManager<FBOType, FactoryFunc, DestroyFunc>::Clear()
{
  for (const auto& it : m_map)
    DestroyFunc(it.second);
  m_map.clear();
}
