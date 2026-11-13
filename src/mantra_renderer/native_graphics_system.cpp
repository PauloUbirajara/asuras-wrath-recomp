#include "native_graphics_system.h"

#include <rex/ui/vulkan/provider.h>

#include "native_command_processor.h"

namespace rex::graphics::native {

void NativeGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::native
