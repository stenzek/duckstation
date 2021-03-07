set(SRCS
	include/vulkan_loader.h
	include/vulkan/vulkan_android.h
	include/vulkan/vulkan_win32.h
	include/vulkan/vk_layer.h
	include/vulkan/vulkan.h
	include/vulkan/vulkan_metal.h
	include/vulkan/vulkan_core.h
	include/vulkan/vulkan_fuchsia.h
	include/vulkan/vulkan_xlib_xrandr.h
	include/vulkan/vk_sdk_platform.h
	include/vulkan/vulkan_xlib.h
	include/vulkan/vulkan_ios.h
	include/vulkan/vk_platform.h
	include/vulkan/vulkan_ggp.h
	include/vulkan/vulkan_wayland.h
	include/vulkan/vulkan_xcb.h
	include/vulkan/vulkan_vi.h
	include/vulkan/vulkan_beta.h
	include/vulkan/vk_icd.h
	include/vulkan/vulkan_macos.h
	src/vulkan_loader.cpp
)

add_library(vulkan-loader ${SRCS})
target_include_directories(vulkan-loader PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vulkan-loader PRIVATE ${CMAKE_DL_LIBS})

if(USE_X11)
  target_compile_definitions(vulkan-loader PUBLIC "VULKAN_USE_X11=1")
  target_include_directories(vulkan-loader PUBLIC "${X11_INCLUDE_DIR}")
endif()

if(USE_WAYLAND)
  target_compile_definitions(vulkan-loader PUBLIC "VULKAN_USE_WAYLAND=1")
endif()
