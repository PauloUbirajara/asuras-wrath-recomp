#include "translator.h"

// XenosRecomp (thirdparty/XenosRecomp, unmodified). pch.h first: the other
// headers rely on it.
#include "pch.h"
#include "dxc_compiler.h"
#include "shader_recompiler.h"

#include "shader_common_hlsl.h"  // generated: kShaderCommonHlsl

namespace mantra_shadergen {

TranslateResult TranslateContainer(const uint8_t* container) {
  TranslateResult result;
  ShaderRecompiler recompiler;
  recompiler.recompile(container, kShaderCommonHlsl);
  result.is_pixel = recompiler.isPixelShader;
  result.spec_constants_mask = recompiler.specConstantsMask;
  result.hlsl = std::move(recompiler.out);

  static DxcCompiler dxc;
  IDxcBlob* spirv = dxc.compile(result.hlsl, result.is_pixel, false, true);
  if (!spirv) {
    return result;
  }
  const auto* bytes = static_cast<const uint8_t*>(spirv->GetBufferPointer());
  result.spirv.assign(bytes, bytes + spirv->GetBufferSize());
  spirv->Release();
  result.ok = true;
  return result;
}

}  // namespace mantra_shadergen
