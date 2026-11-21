module;
#include "quickjs.h"

export module wescene.script:models;

import rstd;
import wescene.scene;
import wescene.model_data;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

export namespace owe::script::models
{

struct Value {
    JSContext* context;
    JSValue    value;
    ~Value() { JS_FreeValue(context, value); }
};

struct Buffers {
    JSContext*               context;
    Vec<JSValue>             retained;
    Vec<Vec<rstd::uint32_t>> widened;
    ~Buffers() {
        for (auto value : retained) JS_FreeValue(context, value);
    }

    auto Read(JSValueConst object, int type, size_t unit) -> Option<slice<rstd::uint8_t>> {
        if (JS_GetTypedArrayType(object) != type) {
            JS_ThrowTypeError(context, "model buffer has an incompatible typed array type");
            return None();
        }
        size_t offset {}, length {}, element_size {}, size {};
        auto   buffer = JS_GetTypedArrayBuffer(context, object, &offset, &length, &element_size);
        if (JS_IsException(buffer)) return None();
        retained.push(rstd::move(buffer));
        auto* bytes = JS_GetArrayBuffer(context, &size, buffer);
        if (element_size != unit || offset > size || length > size - offset || length % unit != 0 ||
            (! bytes && length != 0)) {
            JS_ThrowTypeError(context, "invalid model typed array buffer");
            return None();
        }
        return Some(
            slice<rstd::uint8_t>::from_raw_parts(bytes ? bytes + offset : nullptr, usize(length)));
    }

    auto Vertices(JSValueConst object) -> Option<slice<float>> {
        auto bytes = Read(object, JS_TYPED_ARRAY_FLOAT32, sizeof(float));
        if (! bytes) return None();
        return Some(slice<float>::from_raw_parts(
            reinterpret_cast<const float*>(bytes->as_ptr().as_raw_ptr()),
            bytes->len() / usize(sizeof(float))));
    }

    auto Indices(JSValueConst object) -> Option<slice<rstd::uint32_t>> {
        if (JS_GetTypedArrayType(object) == JS_TYPED_ARRAY_UINT16) {
            auto bytes = Read(object, JS_TYPED_ARRAY_UINT16, sizeof(uint16_t));
            if (! bytes) return None();
            auto source = slice<uint16_t>::from_raw_parts(
                reinterpret_cast<const uint16_t*>(bytes->as_ptr().as_raw_ptr()),
                bytes->len() / usize(sizeof(uint16_t)));
            auto indices = Vec<rstd::uint32_t>::with_capacity(source.len());
            for (auto index : source) indices.push(index);
            widened.push(rstd::move(indices));
            return Some(widened.last().unwrap()->as_slice());
        }
        auto bytes = Read(object, JS_TYPED_ARRAY_UINT32, sizeof(uint32_t));
        if (! bytes) return None();
        return Some(slice<rstd::uint32_t>::from_raw_parts(
            reinterpret_cast<const rstd::uint32_t*>(bytes->as_ptr().as_raw_ptr()),
            bytes->len() / usize(sizeof(rstd::uint32_t))));
    }
};

struct Handle {
    Option<Arc<SceneModelData>> model;
};

JSClassID class_id {};

void Finalize(JSRuntime*, JSValue value) {
    delete static_cast<Handle*>(JS_GetOpaque(value, class_id));
}

auto Get(JSValueConst value) -> Option<Arc<SceneModelData>> {
    auto* handle = static_cast<Handle*>(JS_GetOpaque(value, class_id));
    return handle && handle->model ? Some((*handle->model).clone()) : None();
}

JSValue Apply(JSContext* context, JSValueConst object, int argc, JSValueConst* argv) {
    auto model = Get(object);
    if (! model) return JS_ThrowReferenceError(context, "model data has been destroyed");
    if (argc == 0 || ! JS_IsObject(argv[0]))
        return JS_ThrowTypeError(context, "applyData requires a configuration");
    Value      shapes { context, JS_GetPropertyStr(context, argv[0], "shapes") };
    const bool multiple = ! JS_IsUndefined(shapes.value);
    uint32_t   count    = 1;
    if (multiple) {
        Value length { context, JS_GetPropertyStr(context, shapes.value, "length") };
        if (! JS_IsArray(shapes.value) || JS_ToUint32(context, &count, length.value) < 0)
            return JS_ThrowTypeError(context, "model shapes must be an array");
    }
    Buffers               buffers { context };
    Vec<SceneModelUpdate> updates;
    for (uint32_t i = 0; i < count; ++i) {
        Value shape { context,
                      multiple ? JS_GetPropertyUint32(context, shapes.value, i)
                               : JS_DupValue(context, argv[0]) };
        if (! JS_IsObject(shape.value))
            return JS_ThrowTypeError(context, "model shape update must be an object");
        SceneModelUpdate update;
        Value vertices { context, JS_GetPropertyStr(context, shape.value, "vertexBuffer") };
        Value indices { context, JS_GetPropertyStr(context, shape.value, "indexBuffer") };
        if (! JS_IsUndefined(vertices.value)) {
            update.vertices = buffers.Vertices(vertices.value);
            if (! update.vertices) return JS_EXCEPTION;
        }
        if (! JS_IsUndefined(indices.value)) {
            update.indices = buffers.Indices(indices.value);
            if (! update.indices) return JS_EXCEPTION;
        }
        updates.push(rstd::move(update));
    }
    auto result = (*model)->ApplyData(updates.as_slice());
    if (result.is_err()) {
        const auto& message = result.unwrap_err();
        return JS_ThrowTypeError(context,
                                 "%.*s",
                                 static_cast<int>(message.len().to_primitive()),
                                 reinterpret_cast<const char*>(message.as_str().data()));
    }
    return JS_UNDEFINED;
}

void Init(JSContext* context, JSRuntime* runtime) {
    if (class_id == 0) JS_NewClassID(runtime, &class_id);
    JSClassDef definition {};
    definition.class_name = "IModelData";
    definition.finalizer  = Finalize;
    JS_NewClass(runtime, class_id, &definition);
    Value global { context, JS_GetGlobalObject(context) };
    Value constructor { context, JS_GetPropertyStr(context, global.value, "IModelData") };
    auto  prototype = JS_GetPropertyStr(context, constructor.value, "prototype");
    JS_SetPropertyStr(
        context, prototype, "applyData", JS_NewCFunction(context, Apply, "applyData", 1));
    JS_SetClassProto(context, class_id, prototype);
}

JSValue Create(JSContext* context, int argc, JSValueConst* argv) {
    if (argc == 0 || ! JS_IsObject(argv[0]))
        return JS_ThrowTypeError(context, "createModelData requires a configuration");
    Value    shapes { context, JS_GetPropertyStr(context, argv[0], "shapes") };
    Value    length { context, JS_GetPropertyStr(context, shapes.value, "length") };
    uint32_t count {};
    if (! JS_IsArray(shapes.value) || JS_ToUint32(context, &count, length.value) < 0)
        return JS_ThrowTypeError(context, "model shapes must be an array");
    Buffers              buffers { context };
    Vec<SceneModelShape> configurations;
    for (uint32_t i = 0; i < count; ++i) {
        Value shape { context, JS_GetPropertyUint32(context, shapes.value, i) };
        Value vertices { context, JS_GetPropertyStr(context, shape.value, "vertexBuffer") };
        Value indices { context, JS_GetPropertyStr(context, shape.value, "indexBuffer") };
        Value format { context, JS_GetPropertyStr(context, shape.value, "vertexFormat") };
        Value material { context, JS_GetPropertyStr(context, shape.value, "material") };
        Value vertex_dynamic { context,
                               JS_GetPropertyStr(context, shape.value, "isVertexBufferDynamic") };
        Value index_dynamic { context,
                              JS_GetPropertyStr(context, shape.value, "isIndexBufferDynamic") };
        auto  vertex_data = buffers.Vertices(vertices.value);
        if (! vertex_data) return JS_EXCEPTION;
        SceneModelShape config { .vertices = *vertex_data };
        if (! JS_IsUndefined(indices.value)) {
            config.indices = buffers.Indices(indices.value);
            if (! config.indices) return JS_EXCEPTION;
        }
        if (! JS_IsArray(format.value))
            return JS_ThrowTypeError(context, "vertexFormat must be an array");
        Value    format_length { context, JS_GetPropertyStr(context, format.value, "length") };
        uint32_t attributes {};
        if (JS_ToUint32(context, &attributes, format_length.value) < 0) return JS_EXCEPTION;
        for (uint32_t j = 0; j < attributes; ++j) {
            Value       attribute { context, JS_GetPropertyUint32(context, format.value, j) };
            const char* text = JS_ToCString(context, attribute.value);
            if (! text) return JS_EXCEPTION;
            auto                   name = rstd::ffi::CStr::from_ptr(text).to_str().unwrap();
            Option<VertexAttrSpec> spec;
            if (name == "position"_str) spec = Some(VertexAttrSpec(VAttr::Position));
            if (name == "normal"_str) spec = Some(VertexAttrSpec(VAttr::Normal));
            if (name == "uv"_str) spec = Some(VertexAttrSpec(VAttr::TexCoord));
            if (name == "tangentSigned"_str) spec = Some(VertexAttrSpec(VAttr::Tangent4));
            if (name == "color"_str) spec = Some(VertexAttrSpec(VAttr::Color));
            JS_FreeCString(context, text);
            if (! spec) return JS_ThrowTypeError(context, "unknown model vertex attribute");
            config.attributes.push({ String::make(spec->name), spec->type, false });
        }
        if (! JS_IsString(material.value))
            return JS_ThrowTypeError(context, "model shape requires a registered material");
        const char* path = JS_ToCString(context, material.value);
        if (! path) return JS_EXCEPTION;
        config.material = String::make(rstd::ffi::CStr::from_ptr(path).to_str().unwrap());
        JS_FreeCString(context, path);
        config.dynamic_vertices = JS_ToBool(context, vertex_dynamic.value) > 0;
        config.dynamic_indices  = JS_ToBool(context, index_dynamic.value) > 0;
        configurations.push(rstd::move(config));
    }
    auto model = SceneModelData::Create(rstd::move(configurations));
    if (model.is_err()) {
        const auto& message = model.unwrap_err();
        return JS_ThrowTypeError(context,
                                 "%.*s",
                                 static_cast<int>(message.len().to_primitive()),
                                 reinterpret_cast<const char*>(message.as_str().data()));
    }
    auto result = JS_NewObjectClass(context, class_id);
    if (JS_IsException(result)) return result;
    JS_SetOpaque(result, new Handle { Some(rstd::move(model).unwrap()) });
    JS_SetPropertyStr(
        context, result, "__modelDataToken", JS_NewString(context, "__scenescript_model"));
    return result;
}

JSValue Destroy(JSContext* context, int argc, JSValueConst* argv) {
    auto* handle = argc > 0 ? static_cast<Handle*>(JS_GetOpaque(argv[0], class_id)) : nullptr;
    if (! handle) return JS_ThrowTypeError(context, "destroyModelData requires model data");
    handle->model = None();
    return JS_UNDEFINED;
}

} // namespace owe::script::models
