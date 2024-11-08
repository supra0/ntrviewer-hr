#include "DeviceResources.h"
#include "Logger.h"
#include "fmt/format.h"

namespace Magpie::Core {

ID3D11SamplerState* DeviceResources::GetSampler(D3D11_FILTER filterMode, D3D11_TEXTURE_ADDRESS_MODE addressMode) noexcept {
	auto key = std::make_pair(filterMode, addressMode);
	auto it = _samMap.find(key);
	if (it != _samMap.end()) {
		return it->second.get();
	}

	com_ptr<ID3D11SamplerState> sam;

	D3D11_SAMPLER_DESC desc{
		.Filter = filterMode,
		.AddressU = addressMode,
		.AddressV = addressMode,
		.AddressW = addressMode,
		.ComparisonFunc = D3D11_COMPARISON_NEVER
	};
	HRESULT hr = _d3dDevice->CreateSamplerState(&desc, sam.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("创建 ID3D11SamplerState 出错", hr);
		return nullptr;
	}

	return _samMap.emplace(key, std::move(sam)).first->second.get();
}

}
