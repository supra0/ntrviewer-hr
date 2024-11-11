#include "pch.h"

#include "lib.h"

#include "BackendDescriptorStore.h"
#include "DeviceResources.h"
#include "EffectDrawer.h"
#include "EffectCompiler.h"
#include "EffectsProfiler.h"
#include "ScalingMode.h"
#include "JsonHelper.h"
#include "StrUtils.h"
#include "Win32Utils.h"
#include "Logger.h"
#include "CommonSharedConstants.h"
#include "Utils.h"
#include "com_ptr.h"

using namespace winrt::Magpie::App;
using namespace Magpie::Core;

#include <memory>
#include <string>
#include <rapidjson/prettywriter.h>
#include <rapidjson/document.h>

struct EffectInfo {
	std::string name;
	std::vector<std::string> passNames;
};

struct magpie_render_t {
	std::vector<EffectDrawer> effectDrawers;
	std::vector<EffectInfo> effectInfos;
	DeviceResources backendResources;
	BackendDescriptorStore backendDescriptorStore;
	uint32_t firstDynamicEffectIdx = std::numeric_limits<uint32_t>::max();
	com_ptr<ID3D11Buffer> dynamicCB;
	ID3D11Texture2D* outputTexture;
	uint32_t frameCount = 0;
	EffectsProfiler effectsProfiler;
};

struct magpie_t {
	std::vector<ScalingMode> scalingModes;
	std::vector<std::string> display_names;
};

static bool magpie_startup_done;
extern "C" void magpie_startup(void) {
	if (magpie_startup_done)
		return;

	Logger::Get().Initialize(
		spdlog::level::info,
		CommonSharedConstants::LOG_PATH,
		65535,
		1
	);

	magpie_startup_done = 1;
}

static bool LoadScalingMode(
	const rapidjson::GenericObject<true, rapidjson::Value>& scalingModeObj,
	ScalingMode& scalingMode
) {
	if (!JsonHelper::ReadString(scalingModeObj, "name", scalingMode.name, true)) {
		return false;
	}

	auto effectsNode = scalingModeObj.FindMember("effects");
	if (effectsNode == scalingModeObj.MemberEnd()) {
		return true;
	}

	if (!effectsNode->value.IsArray()) {
		return true;
	}

	auto effectsArray = effectsNode->value.GetArray();
	scalingMode.effects.reserve(effectsArray.Size());

	for (const auto& elem : effectsArray) {
		if (!elem.IsObject()) {
			continue;
		}

		auto elemObj = elem.GetObj();
		EffectOption& effect = scalingMode.effects.emplace_back();

		if (!JsonHelper::ReadString(elemObj, "name", effect.name, true)) {
			scalingMode.effects.pop_back();
			continue;
		}

		JsonHelper::ReadUInt(elemObj, "scalingType", (uint32_t&)effect.scalingType);

		auto scaleNode = elemObj.FindMember("scale");
		if (scaleNode != elemObj.MemberEnd()) {
			if (scaleNode->value.IsObject()) {
				auto scaleObj = scaleNode->value.GetObj();

				float x, y;
				if (JsonHelper::ReadFloat(scaleObj, "x", x, true)
					&& JsonHelper::ReadFloat(scaleObj, "y", y, true)
					&& x > 0 && y > 0)
				{
					effect.scale = { x,y };
				}
			}
		}

		auto parametersNode = elemObj.FindMember("parameters");
		if (parametersNode != elemObj.MemberEnd()) {
			if (parametersNode->value.IsObject()) {
				auto paramsObj = parametersNode->value.GetObj();

				effect.parameters.reserve(paramsObj.MemberCount());
				for (const auto& param : paramsObj) {
					if (!param.value.IsNumber()) {
						continue;
					}

					std::wstring name = StrUtils::UTF8ToUTF16(param.name.GetString());
					effect.parameters[name] = param.value.GetFloat();
				}
			}
		}
	}

	return true;
}

static std::vector<ScalingMode> ImportScalingModes(const rapidjson::GenericObject<true, rapidjson::Value>& root) noexcept {
	std::vector<ScalingMode> scalingModes;

	auto scalingModesNode = root.FindMember("scalingModes");
	if (scalingModesNode == root.MemberEnd()) {
		return scalingModes;
	}

	if (!scalingModesNode->value.IsArray()) {
		return scalingModes;
	}

	const auto& scalingModesArray = scalingModesNode->value.GetArray();
	const rapidjson::SizeType size = scalingModesArray.Size();
	if (size == 0) {
		return scalingModes;
	}

	scalingModes.reserve(size);

	for (const auto& elem : scalingModesArray) {
		if (!elem.IsObject()) {
			continue;
		}

		if (!LoadScalingMode(elem.GetObj(), scalingModes.emplace_back())) {
			scalingModes.pop_back();
		}
	}

	return scalingModes;
}

struct magpie_t *magpie_load(const char *filename) {
	std::string json;
	if (!Win32Utils::ReadTextFile(StrUtils::UTF8ToUTF16(std::string(filename)).c_str(), json)) {
		return 0;
	}

	rapidjson::Document doc;
	// 导入时放宽 json 格式限制
	doc.ParseInsitu<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(json.data());
	if (doc.HasParseError()) {
		Logger::Get().Error(fmt::format("解析缩放模式失败\n\t错误码: {}", (int)doc.GetParseError()));
		return 0;
	}

	if (!doc.IsObject()) {
		return 0;
	}

	auto magpie = std::make_unique<magpie_t>();

	magpie->scalingModes = ImportScalingModes(((const rapidjson::Document&)doc).GetObj());
	magpie->display_names.resize(magpie_mode_count(magpie.get()));

	return magpie.release();
}

void magpie_unload(struct magpie_t *magpie) {
	if (magpie)
		delete magpie;
}

size_t magpie_mode_count(struct magpie_t *magpie) {
	return magpie->scalingModes.size();
}

const char *magpie_mode_name(struct magpie_t *magpie, size_t index, const char *prefix) {
	if (index >= magpie_mode_count(magpie)) {
		return 0;
	}

	if (magpie->display_names[index].empty()) {
		magpie->display_names[index] = prefix + StrUtils::UTF16ToUTF8(magpie->scalingModes[index].name);
	}

	return magpie->display_names[index].c_str();
}

static std::optional<EffectDesc> CompileEffect(const EffectOption& effectOption) noexcept {
	EffectDesc result;

	result.name = StrUtils::UTF16ToUTF8(effectOption.name);

	if (effectOption.flags & EffectOptionFlags::InlineParams) {
		result.flags |= EffectFlags::InlineParams;
	}
	if (effectOption.flags & EffectOptionFlags::FP16) {
		result.flags |= EffectFlags::FP16;
	}

	uint32_t compileFlag = 0;

	bool success = true;
	int duration = Utils::Measure([&]() {
		success = !EffectCompiler::Compile(result, compileFlag, &effectOption.parameters);
	});

	if (success) {
		Logger::Get().Info(fmt::format("编译 {}.hlsl 用时 {} 毫秒",
			StrUtils::UTF16ToUTF8(effectOption.name), duration / 1000.0f));
		return result;
	} else {
		Logger::Get().Error(StrUtils::Concat("编译 ",
			StrUtils::UTF16ToUTF8(effectOption.name), ".hlsl 失败"));
		return std::nullopt;
	}
}

static ID3D11Texture2D* RenderBuildEffects(magpie_render_t *render, const std::vector<EffectOption>& effects, ID3D11Texture2D* tex, const SIZE& outSize) {
	const uint32_t effectCount = (uint32_t)effects.size();

	// 并行编译所有效果
	std::vector<EffectDesc> effectDescs(effects.size());
	std::atomic<bool> anyFailure;

	int duration = Utils::Measure([&]() {
		Win32Utils::RunParallel([&](uint32_t id) {
			std::optional<EffectDesc> desc = CompileEffect(effects[id]);
			if (desc) {
				effectDescs[id] = std::move(*desc);
			} else {
				anyFailure.store(true, std::memory_order_relaxed);
			}
		}, effectCount);
	});

	if (anyFailure.load(std::memory_order_relaxed)) {
		return nullptr;
	}

	if (effectCount > 1) {
		Logger::Get().Info(fmt::format("编译着色器总计用时 {} 毫秒", duration / 1000.0f));
	}

	render->effectDrawers.resize(effects.size());

	ID3D11Texture2D* inOutTexture = tex;
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!render->effectDrawers[i].Initialize(
			effectDescs[i],
			effects[i],
			render->backendResources,
			render->backendDescriptorStore,
			&inOutTexture,
			outSize
		)) {
			Logger::Get().Error(fmt::format("初始化效果#{} ({}) 失败", i, StrUtils::UTF16ToUTF8(effects[i].name)));
			return nullptr;
		}
	}

	// 初始化 render->effectInfos
	render->effectInfos.resize(effectDescs.size());
	for (size_t i = 0; i < effectDescs.size(); ++i) {
		EffectInfo& info = render->effectInfos[i];
		EffectDesc& desc = effectDescs[i];
		info.name = std::move(desc.name);

		info.passNames.reserve(desc.passes.size());
		for (EffectPassDesc& passDesc : desc.passes) {
			info.passNames.emplace_back(std::move(passDesc.desc));
		}
	}

	// 输出尺寸大于缩放窗口尺寸则需要降采样
	{
		D3D11_TEXTURE2D_DESC desc;
		inOutTexture->GetDesc(&desc);
		const SIZE scalingWndSize = outSize;
		if ((LONG)desc.Width > scalingWndSize.cx || (LONG)desc.Height > scalingWndSize.cy) {
			EffectOption bicubicOption{
				.name = L"Bicubic",
				.parameters{
					{L"paramB", 0.0f},
					{L"paramC", 0.5f}
				},
				.scalingType = ScalingType::Fit,
				// 参数不会改变，因此可以内联
				.flags = EffectOptionFlags::InlineParams
			};

			std::optional<EffectDesc> bicubicDesc = CompileEffect(bicubicOption);
			if (!bicubicDesc) {
				Logger::Get().Error("编译降采样效果失败");
				return nullptr;
			}

			EffectDrawer& bicubicDrawer = render->effectDrawers.emplace_back();
			if (!bicubicDrawer.Initialize(
				*bicubicDesc,
				bicubicOption,
				render->backendResources,
				render->backendDescriptorStore,
				&inOutTexture,
				outSize
			)) {
				Logger::Get().Error("初始化降采样效果失败");
				return nullptr;
			}

			// 为降采样算法生成 EffectInfo
			EffectInfo& bicubicEffectInfo = render->effectInfos.emplace_back();
			bicubicEffectInfo.name = std::move(bicubicDesc->name);
			bicubicEffectInfo.passNames.reserve(bicubicDesc->passes.size());
			for (EffectPassDesc& passDesc : bicubicDesc->passes) {
				bicubicEffectInfo.passNames.emplace_back(std::move(passDesc.desc));
			}
		}
	}

	// 初始化所有效果共用的动态常量缓冲区
	for (uint32_t i = 0; i < effectDescs.size(); ++i) {
		if (effectDescs[i].flags & EffectFlags::UseDynamic) {
			render->firstDynamicEffectIdx = i;
			break;
		}
	}

	if (render->firstDynamicEffectIdx != std::numeric_limits<uint32_t>::max()) {
		D3D11_BUFFER_DESC bd = {
			.ByteWidth = 16,	// 只用 4 个字节
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		HRESULT hr = render->backendResources.GetD3DDevice()->CreateBuffer(&bd, nullptr, render->dynamicCB.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateBuffer 失败", hr);
			return nullptr;
		}
	}

	return inOutTexture;
}

magpie_render_t *magpie_render_init(struct magpie_t *magpie, size_t index, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const SIZE* outSize) {
	if (index >= magpie_mode_count(magpie)) {
		return 0;
	}

	auto render = std::make_unique<magpie_render_t>();
	render->backendResources.Initialize(dev, ctx);

	auto d3dDevice = render->backendResources.GetD3DDevice();
	render->backendDescriptorStore.Initialize(d3dDevice);

	render->outputTexture = RenderBuildEffects(render.get(), magpie->scalingModes[index].effects, tex, *outSize);

	return render.release();
}

void magpie_render_close(struct magpie_render_t *render) {
	if (render)
		delete render;
}

static bool RenderUpdateDynamicConstants(struct magpie_render_t *render) noexcept {
	// cbuffer __CB2 : register(b1) { uint __frameCount; };

	auto d3dDC = render->backendResources.GetD3DDC();

	D3D11_MAPPED_SUBRESOURCE ms;
	HRESULT hr = d3dDC->Map(render->dynamicCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
	if (SUCCEEDED(hr)) {
		// 避免使用 *(uint32_t*)ms.pData，见
		// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
		const uint32_t frameCount = ++render->frameCount;
		std::memcpy(ms.pData, &frameCount, 4);
		d3dDC->Unmap(render->dynamicCB.get(), 0);
	} else {
		Logger::Get().ComError("Map 失败", hr);
		return false;
	}

	return true;
}

void magpie_render_run(struct magpie_render_t *render) {
	auto d3dDC = render->backendResources.GetD3DDC();
	d3dDC->ClearState();

	if (ID3D11Buffer* t = render->dynamicCB.get()) {
		RenderUpdateDynamicConstants(render);
		d3dDC->CSSetConstantBuffers(1, 1, &t);
	}

	for (const EffectDrawer& effectDrawer : render->effectDrawers) {
		effectDrawer.Draw(render->effectsProfiler);
	}
}

ID3D11Texture2D* magpie_render_output(struct magpie_render_t *render) {
	return render->outputTexture;
}
