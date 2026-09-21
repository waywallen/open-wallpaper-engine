module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import rstd;
import rstd.log;
import :shader_lex;

using namespace rstd::prelude;
using namespace rstd::literals;

// WE shader annotation collector. Walks the source line by line and pulls
// `// [COMBO]` / `uniform NAME; // {json}` annotations into ShaderInfo.
// Collection is unconditional — #if/#endif dead-branch stripping is glslang's
// job downstream, and gating here creates chicken-and-egg cycles (texture
// combo flag inside `#if MASK == 1` depends on its own annotation).

namespace owe
{

namespace
{

using shader_lex::Cursor;
using shader_lex::LineWalker;

bool TryParseAnnotationJson(ref<str> source, Json& result) {
    auto parsed = rstd::json::from_str(source);
    if (parsed.is_err()) return false;
    result = parsed.unwrap();
    return true;
}

bool CanStartNumberToken(ref<str> source, usize pos) {
    Cursor probe(source, pos);
    while (pos > usize()) {
        probe.SeekTo(--pos);
        char ch = probe.Peek();
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        return ch == '[' || ch == '{' || ch == ':' || ch == ',';
    }
    return true;
}

Option<String> NormalizeAnnotationNumbers(ref<str> source) {
    auto   out       = Vec<u8>::with_capacity(source.len());
    bool   in_string = false;
    bool   escaped   = false;
    bool   changed   = false;
    Cursor cursor(source);
    auto   append = [&] {
        out.push(u8(static_cast<rstd::uint8_t>(cursor.Peek())));
        cursor.Advance();
    };
    while (! cursor.Eof()) {
        const char ch = cursor.Peek();
        if (in_string) {
            append();
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            append();
            continue;
        }
        if ((ch == '-' || (ch >= '0' && ch <= '9')) && CanStartNumberToken(source, cursor.Pos())) {
            if (ch == '-') {
                if (cursor.Peek(usize(1)) < '0' || cursor.Peek(usize(1)) > '9') {
                    append();
                    continue;
                }
                append();
            }
            while (cursor.Peek() == '0' && cursor.Peek(usize(1)) >= '0' &&
                   cursor.Peek(usize(1)) <= '9') {
                changed = true;
                cursor.Advance();
            }
        }
        append();
    }
    if (! changed) return None();
    return Some(String::from_utf8(rstd::move(out)).unwrap());
}

bool ParseAnnotationJson(ref<str> source, Json& result) {
    if (TryParseAnnotationJson(source, result)) return true;
    auto normalized = NormalizeAnnotationNumbers(source);
    return normalized && TryParseAnnotationJson(normalized->as_str(), result);
}

void HandleComboLine(ShaderInfo* info, ref<str> line) {
    auto brace = line.find("{"_str);
    if (brace.is_none()) return;
    Json j;
    if (! ParseAnnotationJson(*line.get(*brace, line.len()), j)) return;
    if (j.get("combo"_str).is_none()) return;
    wpscene::Combo combo;
    combo.FromJson(j);
    if (combo.combo.is_empty()) return;
    (void)info->combos.insert(combo.combo.clone(), rstd::format("{}", combo.default_));
    info->combo_defs.push(rstd::move(combo));
}

void HandlePassLine(ShaderInfo* info, ref<str> line) {
    constexpr auto prefix = "// [PASS] shadow"_str;
    auto           offset = line.find(prefix);
    if (offset.is_none()) return;
    Cursor cursor(line, *offset + prefix.len());
    cursor.SkipHSpace();
    auto   end = line.len();
    Cursor tail(line);
    while (end > cursor.Pos()) {
        tail.SeekTo(end - usize(1));
        auto ch = tail.Peek();
        if (ch != ' ' && ch != '\t' && ch != '\r') break;
        --end;
    }
    if (end > cursor.Pos()) info->shadow_pass = rstd::into(*line.get(cursor.Pos(), end));
}

void HandleUniformLine(ShaderInfo* info, slice<ShaderTexInfo> texinfos, ref<str> line) {
    Cursor c(line);
    c.SkipHSpace();
    if (! c.MatchKeyword("uniform"_str)) return;
    c.SkipHSpace();
    auto tn = shader_lex::ReadTypeName(c);
    if (! tn) return;
    c.SkipHSpace();
    (void)c.ReadArraySuffix();
    c.SkipHSpace();
    if (! c.MatchChar(';')) return;

    // Find the trailing `// {json}` blob.
    while (! c.Eof() && c.Peek() != '/') c.Advance();
    if (! c.MatchPunct("//"_str)) return;
    while (! c.Eof() && c.Peek() != '{') c.Advance();
    if (c.Eof()) return;
    Json sv_json;
    if (! ParseAnnotationJson(*line.get(c.Pos(), line.len()), sv_json)) return;

    auto name = tn->name;

    String material_key;
    GetJsonValue(sv_json, "material"_str, material_key, false);
    if (! material_key.is_empty())
        (void)info->alias.insert(rstd::move(material_key), rstd::into(tn->name));

    const bool is_tex   = name.starts_with("g_Texture"_str);
    const auto texcount = texinfos.len();

    if (is_tex) {
        wpscene::UniformTex wput;
        wput.FromJson(sv_json);
        i32  index {};
        auto parsed = rstd::from_str<i32>(*name.get(usize(9), name.len()));
        if (parsed.is_ok()) {
            index = rstd::move(parsed).unwrap();
        } else {
            rstd_error("invalid shader texture index: {}", name);
        }
        if (! wput.default_.is_empty()) {
            info->defTexs.push({ .slot = index, .texture = wput.default_.clone() });
        }
        const bool has_texture   = index >= i32() && rstd::as_cast<usize>(index) < texcount;
        const auto texture_index = rstd::as_cast<usize>(index);
        if (! wput.combo.is_empty()) {
            const bool enabled = has_texture && texinfos[texture_index].enabled;
            (void)info->combos.insert(wput.combo.clone(), enabled ? "1"_Str : "0"_Str);
        }
        if (has_texture && texinfos[texture_index].enabled) {
            auto& compos = texinfos[texture_index].composEnabled;
            auto  num    = rstd::cmp::min(compos.len(), wput.components.len());
            for (usize i {}; i < num; ++i) {
                if (compos[i]) {
                    auto& combo = wput.components[i].combo;
                    (void)info->combos.insert(combo.clone(), "1"_Str);
                }
            }
        }
        info->texture_uniforms.push(rstd::move(wput));
    } else {
        wpscene::UniformVar var;
        var.FromJson(sv_json, rstd::into(tn->name));
        if (auto value = sv_json.get("default"_str); value.is_some()) {
            ShaderValue sv;
            if ((*value)->is_string()) {
                Vec<float> values;
                GetJsonValue(**value, values);
                sv = ShaderValue(values.as_slice());
            } else if ((*value)->is_number()) {
                sv.setSize(usize(1));
                GetJsonValue(**value, sv[usize()]);
            }
            (void)info->svs.insert(rstd::into(tn->name), sv);
        }
        if (auto combo = sv_json.get("combo"_str); combo.is_some()) {
            String cname;
            GetJsonValue(sv_json, "combo"_str, cname);
            if (! cname.is_empty()) (void)info->combos.insert(rstd::move(cname), "1"_Str);
        }
        info->scalar_uniforms.push(rstd::move(var));
    }
}

} // namespace

void ParseShader(ref<str> src, ShaderInfo* info, slice<ShaderTexInfo> texinfos) {
    LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        auto line = w.Line();
        if (line.is_empty()) continue;
        // Helpers / forward decls above `void main()` are the annotated
        // region; the function body never carries new annotations.
        if (line.contains("void main("_str)) break;

        if (line.contains("// [COMBO]"_str)) {
            HandleComboLine(info, line);
            continue;
        }
        if (line.contains("// [PASS] shadow"_str)) {
            HandlePassLine(info, line);
            continue;
        }
        // Cheap pre-check: only attempt the full keyword match if the trimmed
        // line could plausibly start with `uniform`.
        Cursor probe(line);
        probe.SkipHSpace();
        if (probe.Eof() || probe.Peek() != 'u') continue;
        HandleUniformLine(info, texinfos, line);
    }
}

} // namespace owe
