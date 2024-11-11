#include "placebo.h"
#include <string>
#include <variant>
#include <unordered_map>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>

#define PLACEBO_LOG_DIR "placebo-logs"
#include <filesystem>
#include <cstdio>

pl_log placebo_log_create(void) {
    std::filesystem::create_directory(PLACEBO_LOG_DIR);
    pl_log_params params = {
        .log_cb = pl_log_simple,
        .log_priv = fopen(PLACEBO_LOG_DIR "/placebo.log", "w"),
        .log_level = PL_LOG_INFO,
    };
    return pl_log_create(PL_API_VER, &params);
}

void placebo_log_destroy(pl_log *log) {
    if (*log) {
        if ((*log)->params.log_priv) {
            fclose((FILE *)(*log)->params.log_priv);
        }
        pl_log_destroy(log);
    }
}

struct placebo_param_value_t {
    int i;
    unsigned u;
    float f;
};

struct placebo_effect_t {
    std::string filename;
    std::unordered_map<std::string, placebo_param_value_t> params;
};

struct placebo_mode_t {
    std::string name;
    std::vector<placebo_effect_t> effects;
};

struct placebo_t {
    std::vector<placebo_mode_t> modes;
    std::vector<std::string> display_names;
};

static bool load_mode(
    const rapidjson::GenericObject<true, rapidjson::Value>& mode_obj,
    placebo_mode_t& mode
) {
    auto name_node = mode_obj.FindMember("name");
    if (name_node == mode_obj.MemberEnd() || !name_node->value.IsString()) {
        return false;
    }
    mode.name = name_node->value.GetString();

    auto effects_node = mode_obj.FindMember("effects");
    if (effects_node == mode_obj.MemberEnd() || !effects_node->value.IsArray()) {
        return true;
    }

    auto effects_array = effects_node->value.GetArray();
    mode.effects.reserve(effects_array.Size());

    for (const auto& elem : effects_array) {
        if (!elem.IsObject()) {
            continue;
        }

        auto elem_obj = elem.GetObj();
        placebo_effect_t& effect = mode.effects.emplace_back();

        auto name_node = elem_obj.FindMember("name");
        if (name_node == elem_obj.MemberEnd() || !name_node->value.IsString()) {
            mode.effects.pop_back();
            continue;
        }
        effect.filename = name_node->value.GetString();

        auto params_node = elem_obj.FindMember("params");
        if (params_node != elem_obj.MemberEnd() && params_node->value.IsObject()) {
            auto params_obj = params_node->value.GetObj();

            effect.params.reserve(params_obj.MemberCount());
            for (const auto& param : params_obj) {
                std::string name = param.name.GetString();
                placebo_param_value_t& value = effect.params[name];
                if (param.value.IsNumber()) {
                    value.f = param.value.GetFloat();
                }
                if (param.value.IsInt()) {
                    value.i = param.value.GetInt();
                }
                if (param.value.IsUint()) {
                    value.u = param.value.GetUint();
                }
            }
        }
    }

    return true;
}

static std::vector<placebo_mode_t> parse_modes(const rapidjson::GenericObject<true, rapidjson::Value>& root) {
    std::vector<placebo_mode_t> modes;

    auto modes_node = root.FindMember("modes");
    if (modes_node == root.MemberEnd() || !modes_node->value.IsArray()) {
        return modes;
    }

    const auto& modes_array = modes_node->value.GetArray();
    const rapidjson::SizeType size = modes_array.Size();
    if (size == 0) {
        return modes;
    }

    modes.reserve(size);

    for (const auto& elem : modes_array) {
        if (!elem.IsObject()) {
            continue;
        }

        if (!load_mode(elem.GetObj(), modes.emplace_back())) {
            modes.pop_back();
        }
    }


    return modes;
}

struct placebo_t *placebo_load(const char *filename) {
    std::ifstream infile{filename};
    if (!infile)
        return 0;
    std::istreambuf_iterator<char> itfilebegin{infile}, itfileend;
    std::string json{itfilebegin, itfileend};

    rapidjson::Document doc;
    doc.ParseInsitu<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(json.data());
    if (doc.HasParseError()) {
        std::cerr << "placebo config file \"" << filename << "\" parse error\n";
        return 0;
    }

    if (!doc.IsObject()) {
        return 0;
    }

    auto placebo = std::make_unique<placebo_t>();
    placebo->modes = parse_modes(((const rapidjson::Document&)doc).GetObj());
    placebo->display_names.resize(placebo_mode_count(placebo.get()));

    return placebo.release();
}

void placebo_unload(struct placebo_t *placebo) {
    if (placebo)
        delete placebo;
}

size_t placebo_mode_count(struct placebo_t *placebo) {
    return placebo->modes.size();
}

const char *placebo_mode_name(struct placebo_t *placebo, size_t index, const char *prefix) {
    if (index >= placebo_mode_count(placebo)) {
        return 0;
    }

    if (placebo->display_names[index].empty()) {
        placebo->display_names[index] = prefix + placebo->modes[index].name;
    }

    return placebo->display_names[index].c_str();
}
