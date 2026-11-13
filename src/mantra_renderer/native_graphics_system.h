#pragma once
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::native {

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "mantra"; }

 protected:
  void CreateProvider(bool with_presentation) override;  // copy from vulkan/graphics_system.cpp
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;  // returns NativeCommandProcessor
};

}  // namespace rex::graphics::native
