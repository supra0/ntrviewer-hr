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

	IDXGIFactory7* GetDXGIFactory() const noexcept { return _dxgiFactory.get(); }
	ID3D11Device5* GetD3DDevice() const noexcept { return _d3dDevice.get(); }
	ID3D11DeviceContext4* GetD3DDC() const noexcept { return _d3dDC.get(); }
	IDXGIAdapter4* GetGraphicsAdapter() const noexcept { return _graphicsAdapter.get(); }

	bool IsSupportTearing() const noexcept {
		return _isSupportTearing;
	}

	ID3D11SamplerState* GetSampler(D3D11_FILTER filterMode, D3D11_TEXTURE_ADDRESS_MODE addressMode) noexcept;

private:
	com_ptr<IDXGIFactory7> _dxgiFactory;
	com_ptr<IDXGIAdapter4> _graphicsAdapter;
	com_ptr<ID3D11Device5> _d3dDevice;
	com_ptr<ID3D11DeviceContext4> _d3dDC;

	std::map<
		std::pair<D3D11_FILTER, D3D11_TEXTURE_ADDRESS_MODE>,
		com_ptr<ID3D11SamplerState>
	> _samMap;

	bool _isSupportTearing = false;
};

}
