#pragma once
#include "pch.h"
#include "com_ptr.h"
#include <map>

namespace Magpie::Core {

class DeviceResources {
public:
	DeviceResources() = default;
	DeviceResources(const DeviceResources&) = delete;
	DeviceResources(DeviceResources&&) = default;

	ID3D11Device* GetD3DDevice() const noexcept { return _d3dDevice.get(); }
	ID3D11DeviceContext* GetD3DDC() const noexcept { return _d3dDC.get(); }

	void Initialize(ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept { _d3dDevice = dev; _d3dDC = ctx; }

	ID3D11SamplerState* GetSampler(D3D11_FILTER filterMode, D3D11_TEXTURE_ADDRESS_MODE addressMode) noexcept;

private:
	com_ptr<ID3D11Device> _d3dDevice;
	com_ptr<ID3D11DeviceContext> _d3dDC;

	std::map<
		std::pair<D3D11_FILTER, D3D11_TEXTURE_ADDRESS_MODE>,
		com_ptr<ID3D11SamplerState>
	> _samMap;
};

}
