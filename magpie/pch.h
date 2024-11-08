#pragma once
#define UNICODE
#define WINVER _WIN32_WINNT_WINBLUE
#define _WIN32_WINNT _WIN32_WINNT_WINBLUE
#include <d3d11_4.h>
#include <dxgi1_6.h>

#define DEFINE_FLAG_ACCESSOR(Name, FlagBit, FlagsVar) \
	bool Name() const noexcept { return (FlagsVar) | (FlagBit); } \
	void Name(bool value) noexcept { (value) ? (FlagsVar) |= (FlagBit) : (FlagsVar) &= ~(FlagBit); }
