#pragma once
#include "com_ptr.h"

namespace Magpie::Core {

class TextureLoader {
public:
	static com_ptr<ID3D11Texture2D> Load(const wchar_t* fileName, ID3D11Device* d3dDevice) noexcept;
};

}
