#pragma once
#include "com_ptr.h"
#include <unordered_map>
#include <cstdint>

namespace Magpie::Core {

class BackendDescriptorStore {
public:
	BackendDescriptorStore() = default;
	BackendDescriptorStore(const BackendDescriptorStore&) = delete;
	BackendDescriptorStore(BackendDescriptorStore&&) = default;

	void Initialize(ID3D11Device* d3dDevice) noexcept {
		_d3dDevice = d3dDevice;
	}

	ID3D11ShaderResourceView* GetShaderResourceView(ID3D11Texture2D* texture) noexcept;

	ID3D11UnorderedAccessView* GetUnorderedAccessView(ID3D11Texture2D* texture) noexcept;

	ID3D11UnorderedAccessView* GetUnorderedAccessView(
		ID3D11Buffer* buffer,
		uint32_t numElements,
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN
	) noexcept;

private:
	ID3D11Device* _d3dDevice = nullptr;

	std::unordered_map<ID3D11Texture2D*, com_ptr<ID3D11ShaderResourceView>> _srvMap;
	std::unordered_map<void*, com_ptr<ID3D11UnorderedAccessView>> _uavMap;
};

}
