
/**
This file is included by d3d11_context.cpp
and should be not used as a compilation unit

since it is for internal use only
(and I don't want to deal with several thousands line of code)
*/
#include "d3d11_private.h"
#include "d3d11_context_state.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "dxmt_command_queue.hpp"
#include "mtld11_resource.hpp"
#include "util_flags.hpp"

namespace dxmt {

class ContextInternal {

public:
  ContextInternal(IMTLD3D11Device *pDevice, D3D11ContextState &state,
                  CommandQueue &cmd_queue)
      : device(pDevice), state_(state), cmd_queue(cmd_queue) {
    default_rasterizer_state = CreateDefaultRasterizerState(pDevice);
    default_depth_stencil_state = CreateDefaultDepthStencilState(pDevice);
    default_blend_state = CreateDefaultBlendState(pDevice);
  }

#pragma region ShaderCommon

  template <ShaderType Type, typename IShader>
  void SetShader(IShader *pShader, ID3D11ClassInstance *const *ppClassInstances,
                 UINT NumClassInstances) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];

    if (pShader) {
      if (auto expected = com_cast<IMTLD3D11Shader>(pShader)) {
        ShaderStage.Shader = std::move(expected);
      } else {
        D3D11_ASSERT(0 && "wtf");
      }
    } else {
      ShaderStage.Shader = nullptr;
    }

    if (NumClassInstances)
      ERR("Class instances not supported");

    if (Type == ShaderType::Compute) {
      InvalidateComputePipeline();
    } else {
      InvalidateRenderPipeline();
    }
  }

  template <ShaderType Type, typename IShader>
  void GetShader(IShader **ppShader, ID3D11ClassInstance **ppClassInstances,
                 UINT *pNumClassInstances) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];

    if (ppShader) {
      if (ShaderStage.Shader) {
        ShaderStage.Shader->QueryInterface(IID_PPV_ARGS(ppShader));
      } else {
        *ppShader = NULL;
      }
    }
    if (ppClassInstances) {
      *ppClassInstances = NULL;
    }
    if (pNumClassInstances) {
      *pNumClassInstances = 0;
    }
  }

  template <ShaderType Type>
  void SetConstantBuffer(UINT StartSlot, UINT NumBuffers,
                         ID3D11Buffer *const *ppConstantBuffers,
                         const UINT *pFirstConstant,
                         const UINT *pNumConstants) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];

    for (unsigned slot = StartSlot; slot < StartSlot + NumBuffers; slot++) {
      auto pConstantBuffer = ppConstantBuffers[slot - StartSlot];
      if (pConstantBuffer) {
        bool replaced = false;
        auto &entry =
            ShaderStage.ConstantBuffers.bind(slot, {pConstantBuffer}, replaced);
        if (!replaced) {
          if (pFirstConstant &&
              pFirstConstant[slot - StartSlot] != entry.FirstConstant) {
            ShaderStage.ConstantBuffers.set_dirty(slot);
            entry.FirstConstant = pFirstConstant[slot - StartSlot];
          }
          if (pNumConstants &&
              pNumConstants[slot - StartSlot] != entry.NumConstants) {
            ShaderStage.ConstantBuffers.set_dirty(slot);
            entry.NumConstants = pNumConstants[slot - StartSlot];
          }
          continue;
        }
        entry.FirstConstant =
            pFirstConstant ? pFirstConstant[slot - StartSlot] : 0;
        if (pNumConstants) {
          entry.NumConstants = pNumConstants[slot - StartSlot];
        } else {
          D3D11_BUFFER_DESC desc;
          pConstantBuffer->GetDesc(&desc);
          entry.NumConstants = desc.ByteWidth >> 4;
        }
        if (auto dynamic = com_cast<IMTLDynamicBindable>(pConstantBuffer)) {
          auto old = std::move(entry.Buffer); // FIXME: it looks so weird!
          // should migrate vertex buffer storage to BindingSet
          // once we have implemented BindingSet iteration
          dynamic->GetBindable(
              &entry.Buffer, [slot, &set = ShaderStage.ConstantBuffers](auto) {
                set.set_dirty(slot);
              });
        } else if (auto expected = com_cast<IMTLBindable>(pConstantBuffer)) {
          entry.Buffer = std::move(expected);
        } else {
          D3D11_ASSERT(0 && "wtf");
        }
      } else {
        // BIND NULL
        ShaderStage.ConstantBuffers.unbind(slot);
      }
    }
  }

  template <ShaderType Type>
  void GetConstantBuffer(UINT StartSlot, UINT NumBuffers,
                         ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant,
                         UINT *pNumConstants) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];

    for (auto i = 0u; i < NumBuffers; i++) {
      if (ShaderStage.ConstantBuffers.test_bound(StartSlot + i)) {
        auto &cb = ShaderStage.ConstantBuffers.at(StartSlot + i);
        if (ppConstantBuffers) {
          cb.Buffer->GetLogicalResourceOrView(
              IID_PPV_ARGS(&ppConstantBuffers[i]));
        }
        if (pFirstConstant) {
          pFirstConstant[i] = cb.FirstConstant;
        }
        if (pNumConstants) {
          pNumConstants[i] = cb.NumConstants;
        }
      } else {
        if (ppConstantBuffers) {
          ppConstantBuffers[i] = nullptr;
        }
      }
    }
  }

  template <ShaderType Type>
  void
  SetShaderResource(UINT StartSlot, UINT NumViews,
                    ID3D11ShaderResourceView *const *ppShaderResourceViews) {

    auto &ShaderStage = state_.ShaderStages[(UINT)Type];
    for (unsigned slot = StartSlot; slot < StartSlot + NumViews; slot++) {
      auto pView = ppShaderResourceViews[slot - StartSlot];
      if (pView) {
        bool replaced = false;
        auto &entry = ShaderStage.SRVs.bind(slot, {pView}, replaced);
        if (!replaced)
          continue;
        if (auto dynamic = com_cast<IMTLDynamicBindable>(pView)) {
          entry.SRV = nullptr;
          dynamic->GetBindable(&entry.SRV, [slot, &set = ShaderStage.SRVs](
                                               auto) { set.set_dirty(slot); });
        } else if (auto expected = com_cast<IMTLBindable>(pView)) {
          entry.SRV = std::move(expected);
        } else {
          D3D11_ASSERT(0 && "wtf");
        }
      } else {
        // BIND NULL
        ShaderStage.SRVs.unbind(slot);
      }
    }
  }

  template <ShaderType Type>
  void GetShaderResource(UINT StartSlot, UINT NumViews,
                         ID3D11ShaderResourceView **ppShaderResourceViews) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];

    if (!ppShaderResourceViews)
      return;
    for (auto i = 0u; i < NumViews; i++) {
      if (ShaderStage.SRVs.test_bound(StartSlot + i)) {
        ShaderStage.SRVs[i].SRV->GetLogicalResourceOrView(
            IID_PPV_ARGS(&ppShaderResourceViews[i]));
      } else {
        ppShaderResourceViews[i] = nullptr;
      }
    }
  }

  template <ShaderType Type>
  void SetSamplers(UINT StartSlot, UINT NumSamplers,
                   ID3D11SamplerState *const *ppSamplers) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];
    for (unsigned Slot = StartSlot; Slot < StartSlot + NumSamplers; Slot++) {
      auto pSampler = ppSamplers[Slot - StartSlot];
      if (pSampler) {
        bool replaced = false;
        auto &entry = ShaderStage.Samplers.bind(Slot, {pSampler}, replaced);
        if (!replaced)
          continue;
        if (auto expected = com_cast<IMTLD3D11SamplerState>(pSampler)) {
          entry.Sampler = std::move(expected);
        } else {
          D3D11_ASSERT(0 && "wtf");
        }
      } else {
        // BIND NULL
        ShaderStage.Samplers.unbind(Slot);
      }
    }
  }

  template <ShaderType Type>
  void GetSamplers(UINT StartSlot, UINT NumSamplers,
                   ID3D11SamplerState **ppSamplers) {
    auto &ShaderStage = state_.ShaderStages[(UINT)Type];
    if (ppSamplers) {
      for (unsigned Slot = StartSlot; Slot < StartSlot + NumSamplers; Slot++) {
        if (ShaderStage.Samplers.test_bound(Slot)) {
          ppSamplers[Slot - StartSlot] =
              ShaderStage.Samplers[Slot].Sampler.ref();
        } else {
          ppSamplers[Slot - StartSlot] = nullptr;
        }
      }
    }
  }

  template <ShaderType Type>
  void SetUnorderedAccessView(
      UINT StartSlot, UINT NumUAVs,
      ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
      const UINT *pUAVInitialCounts) {
    auto &binding_set = Type == ShaderType::Compute
                            ? state_.ComputeStageUAV.UAVs
                            : state_.OutputMerger.UAVs;

    // std::erase_if(state_.ComputeStageUAV.UAVs, [&](const auto &item) -> bool
    // {
    //   auto &[slot, bound_uav] = item;
    //   if (slot < StartSlot || slot >= (StartSlot + NumUAVs))
    //     return false;
    //   for (auto i = 0u; i < NumUAVs; i++) {
    //     if (auto uav = static_cast<IMTLD3D11UnorderedAccessView *>(
    //             ppUnorderedAccessViews[i])) {
    //       // FIXME! GetViewRange is not defined on IMTLBindable
    //       // if (bound_uav.View->GetViewRange().CheckOverlap(
    //       //         uav->GetViewRange())) {
    //       //   return true;
    //       // }
    //     }
    //   }
    //   return false;
    // });

    for (unsigned slot = StartSlot; slot < StartSlot + NumUAVs; slot++) {
      auto pUAV = ppUnorderedAccessViews[slot - StartSlot];
      auto InitialCount =
          pUAVInitialCounts ? pUAVInitialCounts[slot - StartSlot] : ~0u;
      if (pUAV) {
        bool replaced = false;
        auto &entry = binding_set.bind(slot, {pUAV}, replaced);
        if (!replaced) {
          // FIXME: update initial count
          continue;
        }
        // FIXME: handle keep current counter...
        entry.InitialCountValue = InitialCount;
        if (auto expected = com_cast<IMTLBindable>(pUAV)) {
          entry.View = std::move(expected);
        } else {
          D3D11_ASSERT(0 && "wtf");
        }
        // FIXME: resolve srv hazard: unbind any cs srv that share the resource
        // std::erase_if(state_.ShaderStages[5].SRVs,
        //               [&](const auto &item) -> bool {
        //                 // auto &[slot, bound_srv] = item;
        //                 // if srv conflict with uav, return true
        //                 return false;
        //               });
      } else {
        binding_set.unbind(slot);
      }
    }
  }

#pragma endregion

#pragma region InputAssembler
  void SetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                        ID3D11Buffer *const *ppVertexBuffers,
                        const UINT *pStrides, const UINT *pOffsets) {
    // FIXME: this implementation is completely BS
    auto &BoundVBs = state_.InputAssembler.VertexBuffers;
    for (unsigned Slot = StartSlot; Slot < StartSlot + NumBuffers; Slot++) {
      auto &VB = BoundVBs[Slot];
      VB.Buffer = nullptr;
      if (auto dynamic = com_cast<IMTLDynamicBindable>(
              ppVertexBuffers[Slot - StartSlot])) {
        dynamic->GetBindable(&VB.Buffer, [this, Slot](MTL::Buffer *buffer) {
          vertex_buffer_dirty |= (1 << Slot);
          auto &BoundVBs = state_.InputAssembler.VertexBuffers;
          auto &VB = BoundVBs[Slot];
          VB.BufferRaw = buffer;
        });
      } else if (auto expected = com_cast<IMTLBindable>(
                     ppVertexBuffers[Slot - StartSlot])) {
        VB.Buffer = std::move(expected);
      } else if (ppVertexBuffers[Slot - StartSlot]) {
        ERR("IASetVertexBuffers: wrong vertex buffer binding");
      } else {
        BoundVBs.erase(Slot);
      }
      if (ppVertexBuffers[Slot - StartSlot]) {
        VB.BufferRaw =
            VB.Buffer->UseBindable(this->cmd_queue.CurrentSeqId()).buffer();
      }
      VB.Stride = pStrides[Slot - StartSlot];
      VB.Offset = pOffsets[Slot - StartSlot];
      vertex_buffer_dirty |= (1 << Slot);
    }
  }

  void GetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                        ID3D11Buffer **ppVertexBuffers, UINT *pStrides,
                        UINT *pOffsets) {
    for (unsigned i = 0; i < NumBuffers; i++) {
      if (!state_.InputAssembler.VertexBuffers.contains(i + StartSlot)) {
        if (ppVertexBuffers != NULL)
          ppVertexBuffers[i] = nullptr;
        if (pStrides != NULL)
          pStrides[i] = 0;
        if (pOffsets != NULL)
          pOffsets[i] = 0;
        continue;
      }
      auto &VertexBuffer = state_.InputAssembler.VertexBuffers[i + StartSlot];
      if (ppVertexBuffers != NULL) {
        VertexBuffer.Buffer->GetLogicalResourceOrView(
            IID_PPV_ARGS(&ppVertexBuffers[i]));
      }
      if (pStrides != NULL)
        pStrides[i] = VertexBuffer.Stride;
      if (pOffsets != NULL)
        pOffsets[i] = VertexBuffer.Offset;
    }
  }
#pragma endregion

#pragma region CopyResource

  void CopyBuffer(ID3D11Buffer *pDstResource, ID3D11Buffer *pSrcResource) {
    D3D11_BUFFER_DESC dst_desc;
    D3D11_BUFFER_DESC src_desc;
    pDstResource->GetDesc(&dst_desc);
    pSrcResource->GetDesc(&src_desc);
    MTL_STAGING_RESOURCE dst_bind;
    UINT bytes_per_row, bytes_per_image;
    MTL_STAGING_RESOURCE src_bind;
    auto currentChunkId = cmd_queue.CurrentSeqId();
    if (auto staging_dst = com_cast<IMTLD3D11Staging>(pDstResource)) {
      staging_dst->UseCopyDestination(0, currentChunkId, &dst_bind,
                                      &bytes_per_row, &bytes_per_image);
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        // wtf
        D3D11_ASSERT(0 && "TODO: copy between staging?");
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // might be a dynamic, default or immutable buffer
        EmitBlitCommand<true>([dst = Obj(dst_bind.Buffer),
                               src = src->UseBindable(currentChunkId)](
                                  MTL::BlitCommandEncoder *encoder, auto &) {
          auto src_buf = src.buffer();
          encoder->copyFromBuffer(src_buf, 0, dst, 0, src_buf->length());
        });
      }

    } else if (dst_desc.Usage == D3D11_USAGE_DEFAULT) {
      auto dst = com_cast<IMTLBindable>(pDstResource);
      D3D11_ASSERT(dst);
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        // copy from staging to default
        staging_src->UseCopySource(0, currentChunkId, &src_bind, &bytes_per_row,
                                   &bytes_per_image);
        EmitBlitCommand<true>([dst = dst->UseBindable(currentChunkId),
                               src = Obj(src_bind.Buffer)](
                                  MTL::BlitCommandEncoder *encoder, auto &) {
          auto dst_buf = dst.buffer();
          encoder->copyFromBuffer(src, 0, dst_buf, 0, src->length());
        });
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // on-device copy
        D3D11_ASSERT(src_bind.Type == MTL_BIND_BUFFER_UNBOUNDED);
        EmitBlitCommand<true>([dst = Obj(dst_bind.Buffer),
                               src = src->UseBindable(currentChunkId)](
                                  MTL::BlitCommandEncoder *encoder, auto &) {
          auto src_buf = src.buffer();
          encoder->copyFromBuffer(src_buf, 0, dst, 0, src_buf->length());
        });
      }
    }
  }

  void CopyTexture1D(ID3D11Texture1D *pDstResource, uint32_t DstSubresource,
                     uint32_t DstX, uint32_t DstY, uint32_t DstZ,
                     ID3D11Texture1D *pSrcResource, uint32_t SrcSubresource,
                     const D3D11_BOX *pSrcBox) {
    D3D11_TEXTURE1D_DESC dst_desc;
    D3D11_TEXTURE1D_DESC src_desc;
    pDstResource->GetDesc(&dst_desc);
    pSrcResource->GetDesc(&src_desc);
    D3D11_BOX SrcBox;
    if (pSrcBox) {
      SrcBox = *pSrcBox;
    } else {
      SrcBox.left = 0;
      SrcBox.right = src_desc.Width;
    }
    auto currentChunkId = cmd_queue.CurrentSeqId();
    if (auto staging_dst = com_cast<IMTLD3D11Staging>(pDstResource)) {
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        D3D11_ASSERT(0 && "tod: copy between staging");
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // copy from device to staging
        MTL_STAGING_RESOURCE dst_bind;
        uint32_t bytes_per_row, bytes_per_image;
        if (!staging_dst->UseCopyDestination(DstSubresource, currentChunkId,
                                             &dst_bind, &bytes_per_row,
                                             &bytes_per_image))
          return;
        EmitBlitCommand<true>([src_ = src->UseBindable(currentChunkId),
                               dst = Obj(dst_bind.Buffer), bytes_per_row,
                               SrcSubresource, DstX, SrcBox](
                                  MTL::BlitCommandEncoder *encoder, auto ctx) {
          auto src = src_.texture(&ctx);
          auto src_mips = src->mipmapLevelCount();
          auto src_level = SrcSubresource % src_mips;
          auto src_slice = SrcSubresource / src_mips;
          D3D11_ASSERT(DstX == 0);
          encoder->copyFromTexture(
              src, src_slice, src_level, MTL::Origin::Make(SrcBox.left, 0, 0),
              MTL::Size::Make(SrcBox.right - SrcBox.left, 1, 1), dst.ptr(),
              /* offset */ 0, bytes_per_row, 0);
          // offset should be DstX*BytesPerTexel
        });
      } else {
        D3D11_ASSERT(0 && "todo");
      }
    } else if (dst_desc.Usage == D3D11_USAGE_DEFAULT) {
      auto dst = com_cast<IMTLBindable>(pDstResource);
      D3D11_ASSERT(dst);
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        // copy from staging to default
        MTL_STAGING_RESOURCE src_bind;
        uint32_t bytes_per_row, bytes_per_image;
        if (!staging_src->UseCopySource(SrcSubresource, currentChunkId,
                                        &src_bind, &bytes_per_row,
                                        &bytes_per_image))
          return;
        EmitBlitCommand<true>(
            [dst_ = dst->UseBindable(currentChunkId),
             src = Obj(src_bind.Buffer), bytes_per_row, DstSubresource, DstX,
             DstY, DstZ, SrcBox](MTL::BlitCommandEncoder *encoder, auto ctx) {
              auto dst = dst_.texture(&ctx);
              auto dst_mips = dst->mipmapLevelCount();
              auto dst_level = DstSubresource % dst_mips;
              auto dst_slice = DstSubresource / dst_mips;
              // FIXME: offste should be calculated from SrcBox
              encoder->copyFromBuffer(
                  src, 0, bytes_per_row, 0,
                  MTL::Size::Make(SrcBox.right - SrcBox.left, 1, 1), dst,
                  dst_slice, dst_level, MTL::Origin::Make(DstX, DstY, DstZ));
            });
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // on-device copy
        EmitBlitCommand<true>([dst_ = dst->UseBindable(currentChunkId),
                               src_ = src->UseBindable(currentChunkId),
                               DstSubresource, SrcSubresource, DstX, DstY, DstZ,
                               SrcBox](MTL::BlitCommandEncoder *encoder,
                                       auto ctx) {
          auto src = src_.texture(&ctx);
          auto dst = dst_.texture(&ctx);
          auto src_mips = src->mipmapLevelCount();
          auto src_level = SrcSubresource % src_mips;
          auto src_slice = SrcSubresource / src_mips;
          auto dst_mips = dst->mipmapLevelCount();
          auto dst_level = DstSubresource % dst_mips;
          auto dst_slice = DstSubresource / dst_mips;
          encoder->copyFromTexture(
              src, src_slice, src_level, MTL::Origin::Make(SrcBox.left, 0, 0),
              MTL::Size::Make(SrcBox.right - SrcBox.left, 1, 1), dst, dst_slice,
              dst_level, MTL::Origin::Make(DstX, DstY, DstZ));
        });
      } else {
        D3D11_ASSERT(0 && "todo");
      }
    } else {
      D3D11_ASSERT(0 && "todo");
    }
  }

  void CopyTexture2D(ID3D11Texture2D *pDstResource, uint32_t DstSubresource,
                     uint32_t DstX, uint32_t DstY, uint32_t DstZ,
                     ID3D11Texture2D *pSrcResource, uint32_t SrcSubresource,
                     const D3D11_BOX *pSrcBox) {
    D3D11_TEXTURE2D_DESC dst_desc;
    D3D11_TEXTURE2D_DESC src_desc;
    pDstResource->GetDesc(&dst_desc);
    pSrcResource->GetDesc(&src_desc);
    D3D11_BOX SrcBox;
    if (pSrcBox) {
      SrcBox = *pSrcBox;
    } else {
      SrcBox.left = 0;
      SrcBox.top = 0;
      SrcBox.right = src_desc.Width;
      SrcBox.bottom = src_desc.Height;
    }
    auto currentChunkId = cmd_queue.CurrentSeqId();
    if (auto staging_dst = com_cast<IMTLD3D11Staging>(pDstResource)) {
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        D3D11_ASSERT(0 && "tod: copy between staging");
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // copy from device to staging
        MTL_STAGING_RESOURCE dst_bind;
        uint32_t bytes_per_row, bytes_per_image;
        if (!staging_dst->UseCopyDestination(DstSubresource, currentChunkId,
                                             &dst_bind, &bytes_per_row,
                                             &bytes_per_image))
          return;
        EmitBlitCommand<true>(
            [src_ = src->UseBindable(currentChunkId),
             dst = Obj(dst_bind.Buffer), bytes_per_row, SrcSubresource, DstX,
             DstY, SrcBox](MTL::BlitCommandEncoder *encoder, auto ctx) {
              auto src = src_.texture(&ctx);
              auto src_mips = src->mipmapLevelCount();
              auto src_level = SrcSubresource % src_mips;
              auto src_slice = SrcSubresource / src_mips;
              D3D11_ASSERT(DstX == 0);
              D3D11_ASSERT(DstY == 0);
              encoder->copyFromTexture(
                  src, src_slice, src_level,
                  MTL::Origin::Make(SrcBox.left, SrcBox.top, 0),
                  MTL::Size::Make(SrcBox.right - SrcBox.left,
                                  SrcBox.bottom - SrcBox.top, 1),
                  dst.ptr(), /* offset */ 0, bytes_per_row, 0);
              // offset should be DstY*bytes_per_row + DstX*BytesPerTexel
            });
      } else {
        D3D11_ASSERT(0 && "todo");
      }
    } else if (dst_desc.Usage == D3D11_USAGE_DEFAULT) {
      auto dst = com_cast<IMTLBindable>(pDstResource);
      D3D11_ASSERT(dst);
      if (auto staging_src = com_cast<IMTLD3D11Staging>(pSrcResource)) {
        // copy from staging to default
        MTL_STAGING_RESOURCE src_bind;
        uint32_t bytes_per_row, bytes_per_image;
        if (!staging_src->UseCopySource(SrcSubresource, currentChunkId,
                                        &src_bind, &bytes_per_row,
                                        &bytes_per_image))
          return;
        EmitBlitCommand<true>([dst_ = dst->UseBindable(currentChunkId),
                               src = Obj(src_bind.Buffer), bytes_per_row,
                               DstSubresource, DstX, DstY, DstZ, SrcBox](
                                  MTL::BlitCommandEncoder *encoder, auto ctx) {
          auto dst = dst_.texture(&ctx);
          auto dst_mips = dst->mipmapLevelCount();
          auto dst_level = DstSubresource % dst_mips;
          auto dst_slice = DstSubresource / dst_mips;
          // FIXME: offste should be calculated from SrcBox
          encoder->copyFromBuffer(
              src, 0, bytes_per_row, 0,
              MTL::Size::Make(SrcBox.right - SrcBox.left,
                              SrcBox.bottom - SrcBox.top, 1),
              dst, dst_slice, dst_level, MTL::Origin::Make(DstX, DstY, DstZ));
        });
      } else if (auto src = com_cast<IMTLBindable>(pSrcResource)) {
        // on-device copy
        EmitBlitCommand<true>([dst_ = dst->UseBindable(currentChunkId),
                               src_ = src->UseBindable(currentChunkId),
                               DstSubresource, SrcSubresource, DstX, DstY, DstZ,
                               SrcBox](MTL::BlitCommandEncoder *encoder,
                                       auto ctx) {
          auto src = src_.texture(&ctx);
          auto dst = dst_.texture(&ctx);
          auto src_mips = src->mipmapLevelCount();
          auto src_level = SrcSubresource % src_mips;
          auto src_slice = SrcSubresource / src_mips;
          auto dst_mips = dst->mipmapLevelCount();
          auto dst_level = DstSubresource % dst_mips;
          auto dst_slice = DstSubresource / dst_mips;
          encoder->copyFromTexture(
              src, src_slice, src_level,
              MTL::Origin::Make(SrcBox.left, SrcBox.top, 0),
              MTL::Size::Make(SrcBox.right - SrcBox.left,
                              SrcBox.bottom - SrcBox.top, 1),
              dst, dst_slice, dst_level, MTL::Origin::Make(DstX, DstY, DstZ));
        });
      } else {
        D3D11_ASSERT(0 && "todo");
      }
    } else {
      D3D11_ASSERT(0 && "todo");
    }
  }

#pragma endregion

#pragma region CommandEncoder Maintain State

  bool ReadyToFlush() {
    if (encoder_count == 0) {
      return false;
    }
    encoder_count = 0;
    return true;
  };

  uint32_t encoder_count = 0;

  /**
  Render pass can be invalidated by reasons:
  - render target changes (including depth stencil)
  All pass can be invalidated by reasons:
  - a encoder type switch
  - flush/present
  */
  void InvalidateCurrentPass() {
    CommandChunk *chk = cmd_queue.CurrentChunk();
    switch (cmdbuf_state) {
    case CommandBufferState::Idle:
      break;
    case CommandBufferState::RenderEncoderActive:
    case CommandBufferState::RenderPipelineReady:
      chk->emit([](CommandChunk::context &ctx) {
        ctx.render_encoder->endEncoding();
        ctx.render_encoder = nullptr;
        ctx.dsv_valid = false;
      });
      encoder_count++;
      break;
    case CommandBufferState::ComputeEncoderActive:
    case CommandBufferState::ComputePipelineReady:
      chk->emit([](CommandChunk::context &ctx) {
        ctx.compute_encoder->endEncoding();
        ctx.compute_encoder = nullptr;
      });
      encoder_count++;
      break;
    case CommandBufferState::BlitEncoderActive:
      chk->emit([](CommandChunk::context &ctx) {
        ctx.blit_encoder->endEncoding();
        ctx.blit_encoder = nullptr;
      });
      encoder_count++;
      break;
    }

    cmdbuf_state = CommandBufferState::Idle;
  }

  /**
  Render pipeline can be invalidate by reasons:
  - shader program changes
  - input layout changes (if using metal vertex descriptor)
  - blend state changes
  - tessellation state changes (not implemented yet)
  - (when layered rendering) input primitive topology changes
  -
  */
  void InvalidateRenderPipeline() {
    if (cmdbuf_state != CommandBufferState::RenderPipelineReady)
      return;
    cmdbuf_state = CommandBufferState::RenderEncoderActive;
  }

  /**
  Compute pipeline can be invalidate by reasons:
  - shader program changes
  TOOD: maybe we don't need this, since compute shader can be pre-compiled
  */
  void InvalidateComputePipeline() {
    if (cmdbuf_state != CommandBufferState::ComputePipelineReady)
      return;
    cmdbuf_state = CommandBufferState::ComputeEncoderActive;
  }

  void EncodeClearPass(ENCODER_CLEARPASS_INFO *clear_pass) {
    InvalidateCurrentPass();
    CommandChunk *chk = cmd_queue.CurrentChunk();
    chk->emit([clear_pass,
               _ = clear_pass->use_clearpass()](CommandChunk::context &ctx) {
      if (clear_pass->skipped) {
        return;
      }
      auto pool = transfer(NS::AutoreleasePool::alloc()->init());
      auto enc_descriptor = MTL::RenderPassDescriptor::renderPassDescriptor();
      for (unsigned i = 0; i < clear_pass->num_color_attachments; i++) {
        auto attachmentz = enc_descriptor->colorAttachments()->object(i);
        attachmentz->setClearColor(clear_pass->clear_colors[i]);
        attachmentz->setTexture(
            clear_pass->clear_color_attachments[i].texture(&ctx));
        attachmentz->setLoadAction(MTL::LoadActionClear);
        attachmentz->setStoreAction(MTL::StoreActionStore);
      }
      if (clear_pass->clear_depth_stencil & D3D11_CLEAR_DEPTH) {
        auto attachmentz = enc_descriptor->depthAttachment();
        attachmentz->setClearDepth(clear_pass->clear_depth);
        attachmentz->setTexture(
            clear_pass->clear_depth_stencil_attachment.texture(&ctx));
        attachmentz->setLoadAction(MTL::LoadActionClear);
        attachmentz->setStoreAction(MTL::StoreActionStore);
      }
      if (clear_pass->clear_depth_stencil & D3D11_CLEAR_STENCIL) {
        auto attachmentz = enc_descriptor->stencilAttachment();
        attachmentz->setClearStencil(clear_pass->clear_stencil);
        attachmentz->setTexture(
            clear_pass->clear_depth_stencil_attachment.texture(&ctx));
        attachmentz->setLoadAction(MTL::LoadActionClear);
        attachmentz->setStoreAction(MTL::StoreActionStore);
      }

      auto enc = ctx.cmdbuf->renderCommandEncoder(enc_descriptor);
      enc->setLabel(NS::String::string("ClearPass", NS::ASCIIStringEncoding));
      enc->endEncoding();
    });
  };

  void ClearRenderTargetView(IMTLD3D11RenderTargetView *pRenderTargetView,
                             const FLOAT ColorRGBA[4]) {
    CommandChunk *chk = cmd_queue.CurrentChunk();

    auto target = pRenderTargetView->GetBinding(cmd_queue.CurrentSeqId());
    auto clear_color = MTL::ClearColor::Make(ColorRGBA[0], ColorRGBA[1],
                                             ColorRGBA[2], ColorRGBA[3]);

    auto previous_encoder = chk->get_previous_encoder();
    // use `while` instead of `if`, for short circuiting
    while (previous_encoder->kind == EncoderKind::ClearPass) {
      auto previous_clearpass = (ENCODER_CLEARPASS_INFO *)previous_encoder;
      unsigned i = 0;
      for (; i < previous_clearpass->num_color_attachments; i++) {
        if (previous_clearpass->clear_color_attachments[i] == target) {
          previous_clearpass->clear_colors[i] = clear_color;
          return;
        }
      }
      if (i == 8) {
        break; // we have to create a new clearpass
      }
      previous_clearpass->num_color_attachments = i + 1;
      previous_clearpass->clear_color_attachments[i] = std::move(target);
      previous_clearpass->clear_colors[i] = clear_color;
      return;
    }

    auto clear_pass = chk->mark_clear_pass();
    clear_pass->num_color_attachments = 1;
    clear_pass->clear_color_attachments[0] = std::move(target);
    clear_pass->clear_colors[0] = clear_color;
    EncodeClearPass(clear_pass);
  }

  void ClearDepthStencilView(IMTLD3D11DepthStencilView *pDepthStencilView,
                             UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    if (ClearFlags == 0)
      return;
    CommandChunk *chk = cmd_queue.CurrentChunk();
    auto target = pDepthStencilView->GetBinding(cmd_queue.CurrentSeqId());

    auto previous_encoder = chk->get_previous_encoder();
    // use `while` instead of `if`, for short circuiting
    while (previous_encoder->kind == EncoderKind::ClearPass) {
      auto previous_clearpass = (ENCODER_CLEARPASS_INFO *)previous_encoder;
      // if there is already a depth stencil attachment
      if (previous_clearpass->clear_depth_stencil_attachment) {
        // if it's the same target
        if (previous_clearpass->clear_depth_stencil_attachment == target) {
          // override previous value
          previous_clearpass->clear_depth_stencil |= ClearFlags;
          if (ClearFlags & D3D11_CLEAR_DEPTH) {
            previous_clearpass->clear_depth = Depth;
          }
          if (ClearFlags & D3D11_CLEAR_STENCIL) {
            previous_clearpass->clear_stencil = Stencil;
          }
          return;
        }
        // otherwise we must create a new clearpass
        break;
      }
      // no depth stencil attachment, just set it
      previous_clearpass->clear_depth_stencil_attachment = std::move(target);
      previous_clearpass->clear_depth_stencil = ClearFlags;
      if (ClearFlags & D3D11_CLEAR_DEPTH) {
        previous_clearpass->clear_depth = Depth;
      }
      if (ClearFlags & D3D11_CLEAR_STENCIL) {
        previous_clearpass->clear_stencil = Stencil;
      }
      return;
    }

    auto clear_pass = chk->mark_clear_pass();
    clear_pass->clear_depth_stencil_attachment = std::move(target);
    clear_pass->clear_depth_stencil = ClearFlags;
    if (ClearFlags & D3D11_CLEAR_DEPTH) {
      clear_pass->clear_depth = Depth;
    }
    if (ClearFlags & D3D11_CLEAR_STENCIL) {
      clear_pass->clear_stencil = Stencil;
    }
    EncodeClearPass(clear_pass);
  }

  /**
  Switch to render encoder and set all states (expect for pipeline state)
  */
  void SwitchToRenderEncoder() {
    if (cmdbuf_state == CommandBufferState::RenderPipelineReady)
      return;
    if (cmdbuf_state == CommandBufferState::RenderEncoderActive)
      return;
    InvalidateCurrentPass();

    // set dirty state
    state_.ShaderStages[0].ConstantBuffers.set_dirty();
    state_.ShaderStages[0].Samplers.set_dirty();
    state_.ShaderStages[0].SRVs.set_dirty();
    state_.ShaderStages[1].ConstantBuffers.set_dirty();
    state_.ShaderStages[1].Samplers.set_dirty();
    state_.ShaderStages[1].SRVs.set_dirty();
    vertex_buffer_dirty = 0xFFFFFFFF;
    dirty_state.set(DirtyState::BlendFactorAndStencilRef,
                    DirtyState::RasterizerState, DirtyState::DepthStencilState,
                    DirtyState::ViewportAndScissors, DirtyState::IndexBuffer);

    // should assume: render target is properly set
    {
      /* Setup RenderCommandEncoder */
      CommandChunk *chk = cmd_queue.CurrentChunk();
      auto currentChunkId = cmd_queue.CurrentSeqId();

      struct RENDER_TARGET_STATE {
        BindingRef Texture;
        UINT RenderTargetIndex;
        UINT MipSlice;
        UINT ArrayIndex;
        UINT DepthPlane;
        MTL::PixelFormat PixelFormat = MTL::PixelFormatInvalid;
        MTL::LoadAction LoadAction{MTL::LoadActionLoad};
        MTL::ClearColor ClearColor{};
      };
      auto rtvs =
          chk->reserve_vector<RENDER_TARGET_STATE>(state_.OutputMerger.NumRTVs);
      for (unsigned i = 0; i < state_.OutputMerger.NumRTVs; i++) {
        auto &rtv = state_.OutputMerger.RTVs[i];
        if (rtv) {
          auto props = rtv->GetRenderTargetProps();
          rtvs.push_back({rtv->GetBinding(currentChunkId), i, props->Level,
                          props->Slice, props->DepthPlane,
                          rtv->GetPixelFormat()});
          D3D11_ASSERT(rtv->GetPixelFormat() != MTL::PixelFormatInvalid);
        } else {
          rtvs.push_back(
              {BindingRef(std::nullopt), i, 0, 0, 0, MTL::PixelFormatInvalid});
        }
      }
      struct DEPTH_STENCIL_STATE {
        BindingRef Texture{};
        UINT MipSlice{};
        UINT ArrayIndex{};
        MTL::PixelFormat PixelFormat = MTL::PixelFormatInvalid;
        MTL::LoadAction DepthLoadAction{MTL::LoadActionLoad};
        MTL::LoadAction StencilLoadAction{MTL::LoadActionLoad};
        float ClearDepth{};
        uint8_t ClearStencil{};
      };
      // auto &dsv = state_.OutputMerger.DSV;
      DEPTH_STENCIL_STATE dsv_info;
      if (state_.OutputMerger.DSV) {
        dsv_info.Texture = state_.OutputMerger.DSV->GetBinding(currentChunkId);
        dsv_info.PixelFormat = state_.OutputMerger.DSV->GetPixelFormat();
      }

      auto previous_encoder = chk->get_previous_encoder();
      if (previous_encoder->kind == EncoderKind::ClearPass) {
        auto previous_clearpass = (ENCODER_CLEARPASS_INFO *)previous_encoder;
        uint32_t skip_clear_color_mask = 0;
        bool skip_clear_ds = 0;
        for (unsigned i = 0; i < previous_clearpass->num_color_attachments;
             i++) {
          for (auto &rtv : rtvs) {
            if (previous_clearpass->clear_color_attachments[i] == rtv.Texture) {
              // you think this is unnecessary? not the case in C++!
              previous_clearpass->clear_color_attachments[i] = {};
              skip_clear_color_mask |= (1 << i);
              rtv.LoadAction = MTL::LoadActionClear;
              rtv.ClearColor = previous_clearpass->clear_colors[i];
            }
          }
        }
        if (previous_clearpass->clear_depth_stencil_attachment &&
            dsv_info.Texture) {
          if (previous_clearpass->clear_depth_stencil_attachment ==
              dsv_info.Texture) {
            previous_clearpass->clear_depth_stencil_attachment = {};
            skip_clear_ds = 1;
            dsv_info.DepthLoadAction =
                previous_clearpass->clear_depth_stencil & D3D11_CLEAR_DEPTH
                    ? MTL::LoadActionClear
                    : MTL::LoadActionLoad;
            dsv_info.StencilLoadAction =
                previous_clearpass->clear_depth_stencil & D3D11_CLEAR_STENCIL
                    ? MTL::LoadActionClear
                    : MTL::LoadActionLoad;
            dsv_info.ClearDepth = previous_clearpass->clear_depth;
            dsv_info.ClearStencil = previous_clearpass->clear_stencil;
          }
        }
        if ((uint32_t)std::popcount(skip_clear_color_mask) ==
                previous_clearpass->num_color_attachments &&
            skip_clear_ds) {
          // the previous clearpass is fully skipped
          previous_clearpass->skipped = true;
        } else if (skip_clear_color_mask || skip_clear_ds) {
          // the previous clearpass is partially skipped, so we construct new
          // clearpass for remaining
          previous_clearpass->skipped = true;
          auto remain_clearpass = chk->mark_clear_pass();
          for (unsigned i = 0; i < previous_clearpass->num_color_attachments;
               i++) {
            if (skip_clear_color_mask | (1 << i))
              continue;
            auto j = remain_clearpass->num_color_attachments;
            remain_clearpass->num_color_attachments++;
            remain_clearpass->clear_color_attachments[j] =
                std::move(previous_clearpass->clear_color_attachments[i]);
            remain_clearpass->clear_colors[j] =
                std::move(previous_clearpass->clear_colors[i]);
          }
          if (!skip_clear_ds) {
            remain_clearpass->clear_depth_stencil_attachment =
                std::move(previous_clearpass->clear_depth_stencil_attachment);
            remain_clearpass->clear_depth_stencil =
                previous_clearpass->clear_depth_stencil;
            remain_clearpass->clear_depth = previous_clearpass->clear_depth;
            remain_clearpass->clear_stencil = previous_clearpass->clear_stencil;
          }
          EncodeClearPass(remain_clearpass);
        } else {
          // previous_clearpass kept untouched
        }
      };

      chk->mark_pass(EncoderKind::Render);

      chk->emit([rtvs = std::move(rtvs),
                 dsv = std::move(dsv_info)](CommandChunk::context &ctx) {
        auto pool = transfer(NS::AutoreleasePool::alloc()->init());
        auto renderPassDescriptor =
            MTL::RenderPassDescriptor::renderPassDescriptor();
        for (auto &rtv : rtvs) {
          if (rtv.PixelFormat == MTL::PixelFormatInvalid)
            continue;
          auto colorAttachment =
              renderPassDescriptor->colorAttachments()->object(
                  rtv.RenderTargetIndex);
          colorAttachment->setTexture(rtv.Texture.texture(&ctx));
          colorAttachment->setLevel(rtv.MipSlice);
          colorAttachment->setSlice(rtv.ArrayIndex);
          colorAttachment->setDepthPlane(rtv.DepthPlane);
          colorAttachment->setLoadAction(rtv.LoadAction);
          colorAttachment->setClearColor(rtv.ClearColor);
          colorAttachment->setStoreAction(MTL::StoreActionStore);
        };
        bool dsv_valid = false;
        if (dsv.Texture) {
          dsv_valid = true;
          // TODO: ...should know more about store behavior (e.g. DiscardView)
          auto depthAttachment = renderPassDescriptor->depthAttachment();
          depthAttachment->setTexture(dsv.Texture.texture(&ctx));
          depthAttachment->setLevel(dsv.MipSlice);
          depthAttachment->setSlice(dsv.ArrayIndex);
          depthAttachment->setLoadAction(dsv.DepthLoadAction);
          depthAttachment->setClearDepth(dsv.ClearDepth);
          depthAttachment->setStoreAction(MTL::StoreActionStore);

          // TODO: should know if depth buffer has stencil bits
          if (dsv.PixelFormat == MTL::PixelFormatDepth32Float_Stencil8 ||
              dsv.PixelFormat == MTL::PixelFormatDepth24Unorm_Stencil8 ||
              dsv.PixelFormat == MTL::PixelFormatStencil8) {
            auto stencilAttachment = renderPassDescriptor->stencilAttachment();
            stencilAttachment->setTexture(dsv.Texture.texture(&ctx));
            stencilAttachment->setLevel(dsv.MipSlice);
            stencilAttachment->setSlice(dsv.ArrayIndex);
            stencilAttachment->setLoadAction(dsv.StencilLoadAction);
            stencilAttachment->setClearStencil(dsv.ClearStencil);
            stencilAttachment->setStoreAction(MTL::StoreActionStore);
          }
        }
        ctx.render_encoder =
            ctx.cmdbuf->renderCommandEncoder(renderPassDescriptor);
        auto [h, _] = ctx.chk->inspect_gpu_heap();
        ctx.dsv_valid = dsv_valid;
        D3D11_ASSERT(ctx.render_encoder);
        ctx.render_encoder->setVertexBuffer(h, 0, 30);
        ctx.render_encoder->setFragmentBuffer(h, 0, 30);
      });
    }

    cmdbuf_state = CommandBufferState::RenderEncoderActive;
  }

  /**
  Switch to blit encoder
  */
  void SwitchToBlitEncoder() {
    if (cmdbuf_state == CommandBufferState::BlitEncoderActive)
      return;
    InvalidateCurrentPass();

    {
      /* Setup ComputeCommandEncoder */
      CommandChunk *chk = cmd_queue.CurrentChunk();

      chk->mark_pass(EncoderKind::Blit);

      chk->emit([](CommandChunk::context &ctx) {
        ctx.blit_encoder = ctx.cmdbuf->blitCommandEncoder();
      });
    }

    cmdbuf_state = CommandBufferState::BlitEncoderActive;
  }

  /**
  Switch to compute encoder and set all states (expect for pipeline state)
  */
  void SwitchToComputeEncoder() {
    if (cmdbuf_state == CommandBufferState::ComputePipelineReady)
      return;
    if (cmdbuf_state == CommandBufferState::ComputeEncoderActive)
      return;
    InvalidateCurrentPass();

    // set dirty state
    state_.ShaderStages[5].ConstantBuffers.set_dirty();
    state_.ShaderStages[5].Samplers.set_dirty();
    state_.ShaderStages[5].SRVs.set_dirty();

    {
      /* Setup ComputeCommandEncoder */
      CommandChunk *chk = cmd_queue.CurrentChunk();

      chk->mark_pass(EncoderKind::Compute);

      chk->emit([](CommandChunk::context &ctx) {
        ctx.compute_encoder = ctx.cmdbuf->computeCommandEncoder();
        auto [heap, offset] = ctx.chk->inspect_gpu_heap();
        ctx.compute_encoder->setBuffer(heap, 0, 30);
      });
    }

    cmdbuf_state = CommandBufferState::ComputeEncoderActive;
  }

  /**
  Assume we have all things needed to build PSO
  If the current encoder is not a render encoder, switch to it.
  */
  bool FinalizeCurrentRenderPipeline() {
    if (cmdbuf_state == CommandBufferState::RenderPipelineReady)
      return true;
    D3D11_ASSERT(state_.InputAssembler.InputLayout && "");

    if (state_.ShaderStages[(UINT)ShaderType::Hull].Shader) {
      // ERR("tessellation is not supported yet, skip drawcall");
      return false;
    }
    if (state_.ShaderStages[(UINT)ShaderType::Domain].Shader) {
      // ERR("tessellation is not supported yet, skip drawcall");
      return false;
    }
    if (state_.ShaderStages[(UINT)ShaderType::Geometry].Shader) {
      // ERR("geometry shader is not supported yet, skip drawcall");
      return false;
    }
    if (!state_.ShaderStages[(UINT)ShaderType::Pixel].Shader) {
      // ERR("stream-out is not supported yet, skip drawcall");
      return false;
    }
    if (!state_.OutputMerger.NumRTVs) {
      return false;
    }

    SwitchToRenderEncoder();

    CommandChunk *chk = cmd_queue.CurrentChunk();

    Com<IMTLCompiledGraphicsPipeline> pipeline;
    Com<IMTLCompiledShader> vs, ps;
    state_.ShaderStages[(UINT)ShaderType::Vertex]
        .Shader //
        ->GetCompiledShader(NULL, &vs);
    state_.ShaderStages[(UINT)ShaderType::Pixel]
        .Shader //
        ->GetCompiledShader(NULL, &ps);
    MTL_GRAPHICS_PIPELINE_DESC pipelineDesc;
    pipelineDesc.VertexShader = vs.ptr();
    pipelineDesc.PixelShader = ps.ptr();
    pipelineDesc.InputLayout = state_.InputAssembler.InputLayout.ptr();
    pipelineDesc.NumColorAttachments = state_.OutputMerger.NumRTVs;
    for (unsigned i = 0; i < pipelineDesc.NumColorAttachments; i++) {
      auto &rtv = state_.OutputMerger.RTVs[i];
      if (rtv) {
        pipelineDesc.ColorAttachmentFormats[i] =
            state_.OutputMerger.RTVs[i]->GetPixelFormat();
      } else {
        pipelineDesc.ColorAttachmentFormats[i] = MTL::PixelFormatInvalid;
      }
    }
    pipelineDesc.BlendState = state_.OutputMerger.BlendState
                                  ? state_.OutputMerger.BlendState.ptr()
                                  : default_blend_state.ptr();
    pipelineDesc.DepthStencilFormat =
        state_.OutputMerger.DSV ? state_.OutputMerger.DSV->GetPixelFormat()
                                : MTL::PixelFormatInvalid;

    device->CreateGraphicsPipeline(&pipelineDesc, &pipeline);

    chk->emit([pso = std::move(pipeline)](CommandChunk::context &ctx) {
      MTL_COMPILED_GRAPHICS_PIPELINE GraphicsPipeline;
      pso->GetPipeline(&GraphicsPipeline); // may block
      ctx.render_encoder->setRenderPipelineState(
          GraphicsPipeline.PipelineState);
    });

    cmdbuf_state = CommandBufferState::RenderPipelineReady;
    return true;
  }

  bool PreDraw() {
    if (!FinalizeCurrentRenderPipeline()) {
      return false;
    }
    UpdateVertexBuffer();
    if (dirty_state.any(DirtyState::DepthStencilState)) {
      EmitSetDepthStencilState<true>();
    }
    if (dirty_state.any(DirtyState::RasterizerState)) {
      EmitSetRasterizerState<true>();
    }
    if (dirty_state.any(DirtyState::BlendFactorAndStencilRef)) {
      EmitBlendFactorAndStencilRef<true>();
    }
    if (dirty_state.any(DirtyState::ViewportAndScissors)) {
      EmitSetViewportAndScissors<true>();
    }
    if (dirty_state.any(DirtyState::IndexBuffer)) {
      CommandChunk *chk = cmd_queue.CurrentChunk();
      // we should be able to retrieve it from chunk
      auto seq_id = cmd_queue.CurrentSeqId();
      if (state_.InputAssembler.IndexBuffer) {
        chk->emit([buffer = state_.InputAssembler.IndexBuffer->UseBindable(
                       seq_id)](CommandChunk::context &ctx) {
          ctx.current_index_buffer_ref = buffer.buffer();
        });
      } else {
        chk->emit([](CommandChunk::context &ctx) {
          ctx.current_index_buffer_ref = nullptr;
        });
      }
    }
    dirty_state.clrAll();
    return UploadShaderStageResourceBinding<ShaderType::Vertex>() &&
           UploadShaderStageResourceBinding<ShaderType::Pixel>();
  }

  /**
  Assume we have all things needed to build PSO
  If the current encoder is not a compute encoder, switch to it.
  */
  bool FinalizeCurrentComputePipeline() {
    if (cmdbuf_state == CommandBufferState::ComputePipelineReady)
      return true;

    SwitchToComputeEncoder();

    Com<IMTLCompiledShader> shader;
    state_.ShaderStages[(UINT)ShaderType::Compute]
        .Shader //
        ->GetCompiledShader(NULL, &shader);
    if (!shader) {
      ERR("Shader not found?");
      return false;
    }
    MTL_COMPILED_SHADER shader_data;
    shader->GetShader(&shader_data);
    Com<IMTLCompiledComputePipeline> pipeline;
    device->CreateComputePipeline(shader.ptr(), &pipeline);

    CommandChunk *chk = cmd_queue.CurrentChunk();
    chk->emit(
        [pso = std::move(pipeline),
         tg_size = MTL::Size::Make(shader_data.Reflection->ThreadgroupSize[0],
                                   shader_data.Reflection->ThreadgroupSize[1],
                                   shader_data.Reflection->ThreadgroupSize[2])](
            CommandChunk::context &ctx) {
          MTL_COMPILED_COMPUTE_PIPELINE ComputePipeline;
          pso->GetPipeline(&ComputePipeline); // may block
          ctx.compute_encoder->setComputePipelineState(
              ComputePipeline.PipelineState);
          ctx.cs_threadgroup_size = tg_size;
        });

    cmdbuf_state = CommandBufferState::ComputePipelineReady;
    return true;
  }

  bool PreDispatch() {
    if (!FinalizeCurrentComputePipeline()) {
      return false;
    }
    UploadShaderStageResourceBinding<ShaderType::Compute>();
    return true;
  }

  template <ShaderType stage, bool Force = false>
  bool UploadShaderStageResourceBinding() {
    auto &ShaderStage = state_.ShaderStages[(UINT)stage];

    // TODO: should be more optimized (only check dirty on used slot)
    if (!Force && !ShaderStage.ConstantBuffers.any_dirty() &&
        !ShaderStage.Samplers.any_dirty() && !ShaderStage.SRVs.any_dirty())
      return true;

    auto currentChunkId = cmd_queue.CurrentSeqId();

    MTL_SHADER_REFLECTION *reflection;
    ShaderStage.Shader->GetReflection(&reflection);
    auto BindingCount = reflection->NumArguments;

    if (BindingCount) {
      CommandChunk *chk = cmd_queue.CurrentChunk();

      /* FIXME: we are over-allocating */
      auto [heap, offset] = chk->allocate_gpu_heap(BindingCount << 4, 16);
      uint64_t *write_to_it = chk->gpu_argument_heap_contents + (offset >> 3);

      auto useResource = [&](BindingRef &&res, MTL::ResourceUsage usage) {
        chk->emit([res = std::move(res), usage](CommandChunk::context &ctx) {
          switch (stage) {
          case ShaderType::Vertex:
            ctx.render_encoder->useResource(res.resource(&ctx), usage,
                                            // FIXME: should be smarter
                                            MTL::RenderStageVertex |
                                                MTL::RenderStageFragment);
            break;
          case ShaderType::Pixel:
            ctx.render_encoder->useResource(res.resource(&ctx), usage,
                                            MTL::RenderStageVertex |
                                                MTL::RenderStageFragment);
            break;
          case ShaderType::Compute:
            ctx.compute_encoder->useResource(res.resource(&ctx), usage);
            break;
          case ShaderType::Geometry:
          case ShaderType::Hull:
          case ShaderType::Domain:
            D3D11_ASSERT(0 && "Not implemented");
            break;
          }
        });
      };

      for (unsigned i = 0; i < BindingCount; i++) {
        auto &arg = reflection->Arguments[i];
        auto slot = arg.SM50BindingSlot;
        switch (arg.Type) {
        case SM50BindingType::ConstantBuffer: {
          if (!ShaderStage.ConstantBuffers.test_bound(slot)) {
            ERR("expect constant buffer at slot ", slot, " but none is bound.");
            return false;
          }
          auto &cbuf = ShaderStage.ConstantBuffers[slot];
          write_to_it[arg.StructurePtrOffset] =
              cbuf.Buffer->GetArgumentData().buffer() +
              (cbuf.FirstConstant << 4);
          if (ShaderStage.ConstantBuffers.test_dirty(slot)) {
            useResource(cbuf.Buffer->UseBindable(currentChunkId),
                        MTL::ResourceUsageRead);
            ShaderStage.ConstantBuffers.clear_dirty(slot);
          }
          break;
        }
        case SM50BindingType::Sampler: {
          if (!ShaderStage.Samplers.test_bound(slot)) {
            ERR("expect sample at slot ", slot, " but none is bound.");
            return false;
          }
          auto &sampler = ShaderStage.Samplers[slot];
          write_to_it[arg.StructurePtrOffset] =
              sampler.Sampler->GetArgumentHandle();
          ShaderStage.Samplers.clear_dirty(slot);
          break;
        }
        case SM50BindingType::SRV: {
          if (!ShaderStage.SRVs.test_bound(slot)) {
            // TODO: debug only
            // ERR("expect shader resource at slot ", slot, " but none is
            // bound.");
            write_to_it[arg.StructurePtrOffset] = 0;
            // ? we are doing something dangerous
            // need to verify that 0 have defined behavior (e.g. no gpu crash)
            if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH) {
              write_to_it[arg.StructurePtrOffset + 1] = 0;
            }
            break;
          }
          auto &srv = ShaderStage.SRVs[slot];
          auto arg_data = srv.SRV->GetArgumentData();
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
            write_to_it[arg.StructurePtrOffset] = arg_data.buffer();
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH) {
            write_to_it[arg.StructurePtrOffset + 1] = arg_data.width();
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
            if (arg_data.requiresContext()) {
              D3D11_ASSERT(0 && "todo");
            } else {
              write_to_it[arg.StructurePtrOffset] = arg_data.texture();
            }
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
          }
          if (ShaderStage.SRVs.test_dirty(slot)) {
            MTL::ResourceUsage usage = (arg.Flags >> 10) & 0b11;
            useResource(srv.SRV->UseBindable(currentChunkId), usage);
            ShaderStage.SRVs.clear_dirty(slot);
          }
          break;
        }
        case SM50BindingType::UAV: {
          if constexpr (stage == ShaderType::Vertex) {
            ERR("uav in vertex shader! need to workaround");
            return false;
          }
          // FIXME: currently only pixel shader use uav from OM
          // REFACTOR NEEDED
          auto &binding_set = stage == ShaderType::Compute
                                  ? state_.ComputeStageUAV.UAVs
                                  : state_.OutputMerger.UAVs;
          // TODO: consider separately handle uav
          if (!binding_set.test_bound(arg.SM50BindingSlot)) {
            // ERR("expect uav at slot ", arg.SM50BindingSlot,
            //     " but none is bound.");
            write_to_it[arg.StructurePtrOffset] = 0;
            if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH) {
              write_to_it[arg.StructurePtrOffset + 1] = 0;
            }
            if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
              write_to_it[arg.StructurePtrOffset + 2] = 0;
            }
            break;
          }
          auto &uav = binding_set[arg.SM50BindingSlot];
          auto arg_data = uav.View->GetArgumentData();
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
            write_to_it[arg.StructurePtrOffset] = arg_data.buffer();
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH) {
            write_to_it[arg.StructurePtrOffset + 1] = arg_data.width();
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
            if (arg_data.requiresContext()) {
              D3D11_ASSERT(0 && "todo");
            } else {
              write_to_it[arg.StructurePtrOffset] = arg_data.texture();
            }
          }
          if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
            ERR("todo: implement uav counter binding");
            return false;
          }
          if (binding_set.test_dirty(slot)) {
            MTL::ResourceUsage usage = (arg.Flags >> 10) & 0b11;
            useResource(uav.View->UseBindable(currentChunkId), usage);
            binding_set.clear_dirty(slot);
          }
          break;
        }
        }
      }

      /* kArgumentBufferBinding = 30 */
      chk->emit([offset](CommandChunk::context &ctx) {
        if constexpr (stage == ShaderType::Vertex) {
          ctx.render_encoder->setVertexBufferOffset(offset, 30);
        } else if constexpr (stage == ShaderType::Pixel) {
          ctx.render_encoder->setFragmentBufferOffset(offset, 30);
        } else if constexpr (stage == ShaderType::Compute) {
          ctx.compute_encoder->setBufferOffset(offset, 30);
        } else {
          D3D11_ASSERT(0 && "Not implemented");
        }
      });
    }
    return true;
  }

  template <bool Force = false, typename Fn> void EmitRenderCommand(Fn &&fn) {
    // FIXME: not quite efficent...?
    EmitRenderCommandChk<Force>(
        [fn = std::forward<Fn>(fn)](CommandChunk::context &ctx) {
          std::invoke(fn, ctx.render_encoder.ptr());
        });
  };

  template <bool Force = false, typename Fn>
  void EmitRenderCommandChk(Fn &&fn) {
    if (Force) {
      SwitchToRenderEncoder();
    }
    if (cmdbuf_state == CommandBufferState::RenderEncoderActive ||
        cmdbuf_state == CommandBufferState::RenderPipelineReady) {
      CommandChunk *chk = cmd_queue.CurrentChunk();
      chk->emit([fn = std::forward<Fn>(fn)](CommandChunk::context &ctx) {
        std::invoke(fn, ctx);
      });
    }
  };

  template <bool Force = false, typename Fn> void EmitComputeCommand(Fn &&fn) {
    if (Force) {
      SwitchToComputeEncoder();
    }
    if (cmdbuf_state == CommandBufferState::ComputeEncoderActive ||
        cmdbuf_state == CommandBufferState::ComputePipelineReady) {
      CommandChunk *chk = cmd_queue.CurrentChunk();
      chk->emit([fn = std::forward<Fn>(fn)](CommandChunk::context &ctx) {
        std::invoke(fn, ctx.compute_encoder.ptr(), ctx.cs_threadgroup_size);
      });
    }
  };

  template <bool Force = false, typename Fn> void EmitBlitCommand(Fn &&fn) {
    if (Force) {
      SwitchToBlitEncoder();
    }
    if (cmdbuf_state == CommandBufferState::BlitEncoderActive) {
      CommandChunk *chk = cmd_queue.CurrentChunk();
      chk->emit([fn = std::forward<Fn>(fn)](CommandChunk::context &ctx) {
        fn(ctx.blit_encoder.ptr(), ctx);
      });
    }
  };

  template <bool Force = false> void EmitSetDepthStencilState() {
    auto state = state_.OutputMerger.DepthStencilState
                     ? state_.OutputMerger.DepthStencilState
                     : default_depth_stencil_state;
    EmitRenderCommandChk<Force>([state = std::move(state),
                                 stencil_ref = state_.OutputMerger.StencilRef](
                                    CommandChunk::context &ctx) {
      auto encoder = ctx.render_encoder;
      encoder->setDepthStencilState(state->GetDepthStencilState(ctx.dsv_valid));
      encoder->setStencilReferenceValue(stencil_ref);
    });
  }

  template <bool Force = false> void EmitSetRasterizerState() {
    auto state = state_.Rasterizer.RasterizerState
                     ? state_.Rasterizer.RasterizerState
                     : default_rasterizer_state;
    EmitRenderCommand<Force>(
        [state = std::move(state)](MTL::RenderCommandEncoder *encoder) {
          state->SetupRasterizerState(encoder);
        });
  }

  template <bool Force = false> void EmitSetViewportAndScissors() {
    if (state_.Rasterizer.NumViewports != state_.Rasterizer.NumScissorRects) {
      return;
    };
    CommandChunk *chk = cmd_queue.CurrentChunk();
    auto viewports =
        chk->reserve_vector<MTL::Viewport>(state_.Rasterizer.NumViewports);
    for (unsigned i = 0; i < state_.Rasterizer.NumViewports; i++) {
      auto &d3dViewport = state_.Rasterizer.viewports[i];
      viewports.push_back({d3dViewport.TopLeftX, d3dViewport.TopLeftY,
                           d3dViewport.Width, d3dViewport.Height,
                           d3dViewport.MinDepth, d3dViewport.MaxDepth});
    }
    auto scissors = chk->reserve_vector<MTL::ScissorRect>(
        state_.Rasterizer.NumScissorRects);
    if (state_.Rasterizer.RasterizerState->IsScissorEnabled()) {
      for (unsigned i = 0; i < state_.Rasterizer.NumScissorRects; i++) {
        auto &d3dRect = state_.Rasterizer.scissor_rects[i];
        scissors.push_back({(UINT)d3dRect.left, (UINT)d3dRect.top,
                            (UINT)d3dRect.right - d3dRect.left,
                            (UINT)d3dRect.bottom - d3dRect.top});
      }
    } else {
      for (unsigned i = 0; i < state_.Rasterizer.NumScissorRects; i++) {
        auto &d3dViewport = state_.Rasterizer.viewports[i];
        scissors.push_back({(UINT)d3dViewport.TopLeftX,
                            (UINT)d3dViewport.TopLeftY, (UINT)d3dViewport.Width,
                            (UINT)d3dViewport.Height});
      }
    }
    EmitRenderCommand<Force>(
        [viewports = std::move(viewports),
         scissors = std::move(scissors)](MTL::RenderCommandEncoder *encoder) {
          encoder->setViewports(viewports.data(), viewports.size());
          encoder->setScissorRects(scissors.data(), scissors.size());
        });
  }

  /**
  it's just about vertex buffer
  - index buffer and input topology are provided in draw commands
  */
  template <bool Force = false> void UpdateVertexBuffer() {
    auto dirty_vbs =
        std::min((size_t)std::popcount((vertex_buffer_dirty & 0xFFFFFFFF)),
                 state_.InputAssembler.VertexBuffers.size());
    if (!dirty_vbs) {
      return;
    }
    for (auto &[index, state] : state_.InputAssembler.VertexBuffers) {
      if ((vertex_buffer_dirty & (1 << index)) == 0) {
        continue;
      }
      // a ref is necessary (in case buffer destroyed before encoding)
      EmitRenderCommand<Force>([buffer = state.BufferRaw, ref = state.Buffer,
                                offset = state.Offset, stride = state.Stride,
                                index](MTL::RenderCommandEncoder *encoder) {
        encoder->setVertexBuffer(buffer, offset, stride, index);
      });
    };
    vertex_buffer_dirty = 0;
  }

  template <bool Force = false> void EmitBlendFactorAndStencilRef() {
    EmitRenderCommand<Force>([r = state_.OutputMerger.BlendFactor[0],
                              g = state_.OutputMerger.BlendFactor[1],
                              b = state_.OutputMerger.BlendFactor[2],
                              a = state_.OutputMerger.BlendFactor[3],
                              stencil_ref = state_.OutputMerger.StencilRef](
                                 MTL::RenderCommandEncoder *encoder) {
      encoder->setBlendColor(r, g, b, a);
      encoder->setStencilReferenceValue(stencil_ref);
    });
  }

#pragma endregion

  /**
  Valid transition:
  * -> Idle
  Idle -> RenderEncoderActive
  Idle -> ComputeEncoderActive
  Idle -> BlitEncoderActive
  RenderEncoderActive <-> RenderPipelineReady
  ComputeEncoderActive <-> CoputePipelineReady
  */
  enum class CommandBufferState {
    Idle,
    RenderEncoderActive,
    RenderPipelineReady,
    ComputeEncoderActive,
    ComputePipelineReady,
    BlitEncoderActive
  };

#pragma region Default State

private:
  /**
  Don't bind them to state or provide to API consumer
  (use COM just in case it's referenced by encoding thread)
  ...tricky
  */
  Com<IMTLD3D11RasterizerState> default_rasterizer_state;
  Com<IMTLD3D11DepthStencilState> default_depth_stencil_state;
  Com<IMTLD3D11BlendState> default_blend_state;

#pragma endregion

private:
  IMTLD3D11Device *device;
  D3D11ContextState &state_;
  CommandBufferState cmdbuf_state;
  uint64_t vertex_buffer_dirty = 0xFFFFFFFF;
  CommandQueue &cmd_queue;

public:
  enum class DirtyState {
    DepthStencilState = 1,
    RasterizerState = 2,
    BlendFactorAndStencilRef = 4,
    ViewportAndScissors = 8,
    IndexBuffer = 16,
  };

  Flags<DirtyState> dirty_state;
};

} // namespace dxmt