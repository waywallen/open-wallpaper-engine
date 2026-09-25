export module wescene.particle;

import eigen;
import rstd;

using rstd::collections::HashMap;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::particle
{

struct ParticleAttributeId {
    u64 value {};

    bool        Valid() const noexcept { return value != u64(); }
    friend bool operator==(ParticleAttributeId, ParticleAttributeId) = default;
};

struct ParticleSlot {
    usize index {};

    friend bool operator==(ParticleSlot, ParticleSlot) = default;
};

enum class ParticleAttributeResetPolicy : rstd::uint8_t
{
    DefaultValue,
    Custom,
};

struct ParticleAttributeDescriptor {
    ParticleAttributeId          id;
    String                       debug_name;
    String                       owner;
    rstd::any::TypeId            concrete_type;
    rstd::any::TypeId            value_type;
    usize                        value_size {};
    usize                        value_alignment {};
    ParticleAttributeResetPolicy reset_policy { ParticleAttributeResetPolicy::DefaultValue };

    auto Clone() const -> ParticleAttributeDescriptor {
        return {
            .id              = id,
            .debug_name      = debug_name.clone(),
            .owner           = owner.clone(),
            .concrete_type   = concrete_type,
            .value_type      = value_type,
            .value_size      = value_size,
            .value_alignment = value_alignment,
            .reset_policy    = reset_policy,
        };
    }
};

struct ParticleAttribute {
    using Trait                  = ParticleAttribute;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleAttribute;

        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
            return rstd::trait_call<0>(this);
        }
        auto ConcreteType() const noexcept -> rstd::any::TypeId {
            return rstd::trait_call<1>(this);
        }
        auto ValueType() const noexcept -> rstd::any::TypeId { return rstd::trait_call<2>(this); }
        auto Len() const noexcept -> usize { return rstd::trait_call<3>(this); }
        auto Capacity() const noexcept -> usize { return rstd::trait_call<4>(this); }
        void Reserve(usize total_slots) { rstd::trait_call<5>(this, total_slots); }
        void AppendDefaults(usize count) { rstd::trait_call<6>(this, count); }
        void ResetSlots(slice<ParticleSlot> slots) { rstd::trait_call<7>(this, slots); }
        void Clear() { rstd::trait_call<8>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Descriptor, &T::ConcreteType, &T::ValueType, &T::Len, &T::Capacity,
                             &T::Reserve, &T::AppendDefaults, &T::ResetSlots, &T::Clear>;
};

struct ParticleAttributeFactory {
    using Trait                  = ParticleAttributeFactory;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleAttributeFactory;

        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
            return rstd::trait_call<0>(this);
        }
        auto Create() const -> Box<dyn<ParticleAttribute>> { return rstd::trait_call<1>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Descriptor, &T::Create>;
};

template<typename Attribute>
struct ParticleAttributeKey {
    using AttributeType = Attribute;
    using Value         = typename Attribute::Value;

    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };

    bool Valid() const noexcept { return id.Valid() && schema_slot != usize::MAX; }
};

struct ParticleAttributeRequirement {
    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };
    rstd::any::TypeId   concrete_type;
};

template<typename Attribute>
auto RequireParticleAttribute(ParticleAttributeKey<Attribute> key) -> ParticleAttributeRequirement {
    return {
        .id            = key.id,
        .schema_slot   = key.schema_slot,
        .concrete_type = rstd::any::TypeId::of<Attribute>(),
    };
}

struct ParticleSchemaError {
    String message;
};

template<typename ValueType>
class ParticleValueAttributeStorage {
public:
    using Value = ValueType;

    ParticleValueAttributeStorage(ParticleAttributeDescriptor descriptor, Value default_value)
        : m_descriptor(rstd::move(descriptor)), m_default(rstd::move(default_value)) {}

    auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
        return ref<ParticleAttributeDescriptor>::from_raw_parts(rstd::addressof(m_descriptor));
    }
    auto ConcreteType() const noexcept -> rstd::any::TypeId { return m_descriptor.concrete_type; }
    auto ValueTypeId() const noexcept -> rstd::any::TypeId { return m_descriptor.value_type; }
    auto Len() const noexcept -> usize { return m_values.len(); }
    auto Capacity() const noexcept -> usize { return m_values.capacity(); }
    void Reserve(usize total_slots) {
        if (total_slots > m_values.capacity()) m_values.reserve(total_slots - m_values.len());
    }
    void AppendDefaults(usize count) {
        for (usize index {}; index < count; ++index) m_values.emplace_back(m_default);
    }
    void ResetSlots(slice<ParticleSlot> slots) {
        for (auto slot : slots) m_values[slot.index] = m_default;
    }
    void Clear() { m_values.clear(); }

    auto Values() const noexcept -> slice<Value> { return m_values.as_slice(); }
    auto ValuesMut() noexcept -> mut_ref<Value[]> { return m_values.deref_mut(); }
    auto CloneDescriptor() const -> ParticleAttributeDescriptor { return m_descriptor.Clone(); }
    const Value& DefaultValue() const noexcept { return m_default; }

private:
    ParticleAttributeDescriptor m_descriptor;
    Value                       m_default;
    Vec<Value>                  m_values;
};

#define OWE_PARTICLE_VALUE_ATTRIBUTE(Name, Type)                                                   \
    struct Name {                                                                                  \
        using Value = Type;                                                                        \
                                                                                                   \
        Name(ParticleAttributeDescriptor descriptor, Value default_value)                          \
            : storage(rstd::move(descriptor), rstd::move(default_value)) {}                        \
                                                                                                   \
        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {                              \
            return storage.Descriptor();                                                           \
        }                                                                                          \
        auto ConcreteType() const noexcept -> rstd::any::TypeId { return storage.ConcreteType(); } \
        auto ValueType() const noexcept -> rstd::any::TypeId { return storage.ValueTypeId(); }     \
        auto Len() const noexcept -> usize { return storage.Len(); }                               \
        auto Capacity() const noexcept -> usize { return storage.Capacity(); }                     \
        void Reserve(usize total_slots) { storage.Reserve(total_slots); }                          \
        void AppendDefaults(usize count) { storage.AppendDefaults(count); }                        \
        void ResetSlots(slice<ParticleSlot> slots) { storage.ResetSlots(slots); }                  \
        void Clear() { storage.Clear(); }                                                          \
        auto Values() const noexcept -> slice<Value> { return storage.Values(); }                  \
        auto ValuesMut() noexcept -> mut_ref<Value[]> { return storage.ValuesMut(); }              \
        auto CloneEmpty() const -> Name {                                                          \
            return Name(storage.CloneDescriptor(), storage.DefaultValue());                        \
        }                                                                                          \
                                                                                                   \
        ParticleValueAttributeStorage<Value> storage;                                              \
    }

struct ParticleSlotState {
    bool active { false };
    bool fresh { false };
    u64  spawn_sequence {};
};

OWE_PARTICLE_VALUE_ATTRIBUTE(SlotStateAttribute, ParticleSlotState);
OWE_PARTICLE_VALUE_ATTRIBUTE(PositionAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(VelocityAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AccelerationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(RotationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AngularVelocityAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AngularAccelerationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(ColorAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AlphaAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(SizeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(LifetimeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(RandomAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialColorAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialAlphaAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialSizeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialLifetimeAttribute, float);

#undef OWE_PARTICLE_VALUE_ATTRIBUTE

template<typename Attribute>
class ConcreteParticleAttributeFactory {
public:
    explicit ConcreteParticleAttributeFactory(Attribute prototype)
        : m_prototype(rstd::move(prototype)) {}

    auto Descriptor() const -> ref<ParticleAttributeDescriptor> { return m_prototype.Descriptor(); }
    auto Create() const -> Box<dyn<ParticleAttribute>> {
        return Box<dyn<ParticleAttribute>>::make(m_prototype.CloneEmpty());
    }

private:
    Attribute m_prototype;
};

class ParticleStorage;
class ParticleViewCompiler;
class ParticleViewBinding;

class ParticleSchema {
public:
    ParticleSchema(const ParticleSchema&)                = delete;
    ParticleSchema& operator=(const ParticleSchema&)     = delete;
    ParticleSchema(ParticleSchema&&) noexcept            = default;
    ParticleSchema& operator=(ParticleSchema&&) noexcept = default;

    auto Len() const noexcept -> usize { return m_factories.len(); }
    auto Descriptor(usize slot) const -> ref<ParticleAttributeDescriptor> {
        return m_factories[slot]->Descriptor();
    }
    auto Find(ParticleAttributeId id) const -> Option<usize> {
        auto slot = m_id_slots.get(id.value);
        if (slot.is_none()) return None();
        auto value = usize((**slot).to_primitive());
        return Some(rstd::move(value));
    }
    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }
    auto PositionKey() const noexcept -> ParticleAttributeKey<PositionAttribute> {
        return m_position_key;
    }

    auto Validate(ParticleAttributeRequirement requirement) const
        -> Result<bool, ParticleSchemaError> {
        if (! requirement.id.Valid() || requirement.schema_slot >= m_factories.len()) {
            return Err(ParticleSchemaError {
                .message = "required particle attribute is missing"_Str,
            });
        }
        auto descriptor = m_factories[requirement.schema_slot]->Descriptor();
        if (descriptor->id != requirement.id ||
            descriptor->concrete_type != requirement.concrete_type) {
            return Err(ParticleSchemaError {
                .message = "required particle attribute does not match schema"_Str,
            });
        }
        return Ok(true);
    }

    auto CreateStorage() const -> ParticleStorage;

private:
    friend class ParticleSchemaBuilder;
    friend class ParticleStorage;

    ParticleSchema(Vec<Box<dyn<ParticleAttributeFactory>>> factories, HashMap<u64, usize> id_slots,
                   ParticleAttributeKey<SlotStateAttribute> slot_state_key,
                   ParticleAttributeKey<PositionAttribute>  position_key)
        : m_factories(rstd::move(factories)),
          m_id_slots(rstd::move(id_slots)),
          m_slot_state_key(slot_state_key),
          m_position_key(position_key) {}

    Vec<Box<dyn<ParticleAttributeFactory>>>  m_factories;
    HashMap<u64, usize>                      m_id_slots;
    ParticleAttributeKey<SlotStateAttribute> m_slot_state_key;
    ParticleAttributeKey<PositionAttribute>  m_position_key;
};

class ParticleSchemaBuilder {
public:
    ParticleSchemaBuilder();

    template<typename Attribute, typename... Args>
    auto Register(ref<str> name, ref<str> owner, ParticleAttributeResetPolicy reset_policy,
                  Args&&... args) -> Result<ParticleAttributeKey<Attribute>, ParticleSchemaError> {
        for (const auto& factory : m_factories) {
            if (factory->Descriptor()->debug_name == name) {
                return Err(ParticleSchemaError {
                    .message = "duplicate particle attribute name"_Str,
                });
            }
        }

        ParticleAttributeId         id { .value = m_next_id++ };
        ParticleAttributeDescriptor descriptor {
            .id              = id,
            .debug_name      = String::make(name),
            .owner           = String::make(owner),
            .concrete_type   = rstd::any::TypeId::of<Attribute>(),
            .value_type      = rstd::any::TypeId::of<typename Attribute::Value>(),
            .value_size      = usize(sizeof(typename Attribute::Value)),
            .value_alignment = usize(alignof(typename Attribute::Value)),
            .reset_policy    = reset_policy,
        };
        Attribute prototype(rstd::move(descriptor), rstd::forward<Args>(args)...);
        auto      slot    = m_factories.len();
        auto      factory = ConcreteParticleAttributeFactory<Attribute>(rstd::move(prototype));
        m_factories.push(Box<dyn<ParticleAttributeFactory>>::make(rstd::move(factory)));
        (void)m_id_slots.insert(id.value, slot);
        return Ok(ParticleAttributeKey<Attribute> { .id = id, .schema_slot = slot });
    }

    template<typename Attribute, typename... Args>
    auto Register(ref<str> name, ref<str> owner, Args&&... args)
        -> Result<ParticleAttributeKey<Attribute>, ParticleSchemaError> {
        return Register<Attribute>(
            name, owner, ParticleAttributeResetPolicy::DefaultValue, rstd::forward<Args>(args)...);
    }

    auto Build() && -> ParticleSchema {
        return ParticleSchema(
            rstd::move(m_factories), rstd::move(m_id_slots), m_slot_state_key, m_position_key);
    }

    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }
    auto PositionKey() const noexcept -> ParticleAttributeKey<PositionAttribute> {
        return m_position_key;
    }

private:
    u64                                      m_next_id { 1 };
    Vec<Box<dyn<ParticleAttributeFactory>>>  m_factories;
    HashMap<u64, usize>                      m_id_slots;
    ParticleAttributeKey<SlotStateAttribute> m_slot_state_key;
    ParticleAttributeKey<PositionAttribute>  m_position_key;
};

class ParticleStorage {
public:
    ParticleStorage(const ParticleStorage&)                = delete;
    ParticleStorage& operator=(const ParticleStorage&)     = delete;
    ParticleStorage(ParticleStorage&&) noexcept            = default;
    ParticleStorage& operator=(ParticleStorage&&) noexcept = default;

    auto Len() const noexcept -> usize { return m_len; }
    auto StructureVersion() const noexcept -> u64 { return m_structure_version; }
    auto ColumnVersion() const noexcept -> u64 { return m_column_version; }
    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }

    void Reserve(usize total_slots) {
        bool needs_reserve { false };
        for (const auto& attribute : m_attributes) {
            if (attribute->Capacity() < total_slots) {
                needs_reserve = true;
                break;
            }
        }
        if (! needs_reserve) return;
        for (auto& attribute : m_attributes) attribute->Reserve(total_slots);
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
    }

    auto AppendSlots(usize count) -> Vec<ParticleSlot> {
        auto slots = Vec<ParticleSlot>::with_capacity(count);
        if (count == usize()) return slots;

        Reserve(m_len + count);
        for (auto& attribute : m_attributes) attribute->AppendDefaults(count);
        for (usize index {}; index < count; ++index) {
            slots.emplace_back(ParticleSlot { .index = m_len + index });
        }
        m_len += count;
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
        return slots;
    }

    auto AppendSlot() -> ParticleSlot {
        auto slots = AppendSlots(usize(1));
        return slots.first().unwrap().get();
    }

    void ResetSlots(slice<ParticleSlot> slots) {
        for (auto slot : slots) {
            if (slot.index >= m_len) rstd::panic { "particle slot out of bounds" };
        }
        for (auto& attribute : m_attributes) attribute->ResetSlots(slots);
        CheckInvariant();
    }

    void ResetSlot(ParticleSlot slot) {
        ResetSlots(slice<ParticleSlot>::from_raw_parts(rstd::addressof(slot), usize(1)));
    }

    void Clear() {
        for (auto& attribute : m_attributes) attribute->Clear();
        m_len                 = usize();
        m_next_spawn_sequence = u64();
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
    }

    auto AcquireSlots(usize count, usize max_slots) -> Vec<ParticleSlot> {
        auto acquired = Vec<ParticleSlot>::with_capacity(count);
        if (count == usize()) return acquired;

        auto states = Values(m_slot_state_key);
        for (usize index {}; index < states.len(); ++index) {
            if (states[index].active) continue;
            acquired.emplace_back(ParticleSlot { .index = index });
            if (acquired.len() == count) break;
        }

        auto reused_count = acquired.len();
        if (reused_count != usize()) ResetSlots(acquired.as_slice());

        auto available    = max_slots > m_len ? max_slots - m_len : usize();
        auto append_count = rstd::cmp::min(count - acquired.len(), available);
        if (append_count != usize()) {
            auto appended = AppendSlots(append_count);
            for (auto slot : appended) acquired.emplace_back(slot);
        }

        auto current = ValuesMut(m_slot_state_key);
        for (auto slot : acquired) {
            current[slot.index] = {
                .active         = true,
                .fresh          = true,
                .spawn_sequence = m_next_spawn_sequence++,
            };
        }
        return acquired;
    }

    auto AcquireSlot(usize max_slots) -> Option<ParticleSlot> {
        auto slots = AcquireSlots(usize(1), max_slots);
        return slots.first().map([](ref<ParticleSlot> slot) {
            return slot.get();
        });
    }

    void ReleaseSlot(ParticleSlot slot) {
        auto states = ValuesMut(m_slot_state_key);
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        states[slot.index] = {};
    }

    void MarkOld(ParticleSlot slot) {
        auto states = ValuesMut(m_slot_state_key);
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        states[slot.index].fresh = false;
    }

    template<typename Attribute>
    auto AttributeRef(ParticleAttributeKey<Attribute> key) const -> ref<Attribute> {
        ValidateKey(key);
        auto erased = m_attributes[key.schema_slot].as_ref();
        return ref<Attribute>::from_raw_parts(static_cast<const Attribute*>(erased.as_raw_ptr()));
    }

    template<typename Attribute>
    auto AttributeMut(ParticleAttributeKey<Attribute> key) -> mut_ref<Attribute> {
        ValidateKey(key);
        auto erased = m_attributes[key.schema_slot].as_mut_ptr();
        return mut_ref<Attribute>::from_raw_parts(static_cast<Attribute*>(erased.as_raw_ptr()));
    }

    template<typename Attribute>
    auto Values(ParticleAttributeKey<Attribute> key) const -> slice<typename Attribute::Value> {
        return AttributeRef(key)->Values();
    }

    template<typename Attribute>
    auto ValuesMut(ParticleAttributeKey<Attribute> key) -> mut_ref<typename Attribute::Value[]> {
        return AttributeMut(key)->ValuesMut();
    }

private:
    friend class ParticleSchema;
    friend class ParticleViewCompiler;
    friend class ParticleViewBinding;

    ParticleStorage(Vec<Box<dyn<ParticleAttribute>>>         attributes,
                    ParticleAttributeKey<SlotStateAttribute> slot_state_key,
                    ParticleAttributeKey<PositionAttribute>  position_key)
        : m_attributes(rstd::move(attributes)),
          m_slot_state_key(slot_state_key),
          m_position_key(position_key) {
        CheckInvariant();
    }

    template<typename Attribute>
    void ValidateKey(ParticleAttributeKey<Attribute> key) const {
        if (! key.Valid() || key.schema_slot >= m_attributes.len()) {
            rstd::panic { "invalid particle attribute key" };
        }
        auto descriptor = m_attributes[key.schema_slot]->Descriptor();
        if (descriptor->id != key.id ||
            descriptor->concrete_type != rstd::any::TypeId::of<Attribute>()) {
            rstd::panic { "particle attribute key does not match storage" };
        }
    }

    void CheckInvariant() const {
        for (const auto& attribute : m_attributes) {
            if (attribute->Len() != m_len) rstd::panic { "particle attribute length mismatch" };
        }
    }

    void BumpStructureVersion() noexcept {
        ++m_structure_version;
        if (m_structure_version == u64()) m_structure_version = u64(1);
    }

    void BumpColumnVersion() noexcept {
        ++m_column_version;
        if (m_column_version == u64()) m_column_version = u64(1);
    }

    usize                                    m_len {};
    u64                                      m_next_spawn_sequence {};
    u64                                      m_structure_version { 1 };
    u64                                      m_column_version { 1 };
    Vec<Box<dyn<ParticleAttribute>>>         m_attributes;
    ParticleAttributeKey<SlotStateAttribute> m_slot_state_key;
    ParticleAttributeKey<PositionAttribute>  m_position_key;
};

template<typename Attribute>
class ParticleReadIndex {
public:
    ParticleReadIndex() = default;
    bool Valid() const noexcept { return m_value != usize::MAX; }

private:
    explicit ParticleReadIndex(usize value): m_value(value) {}

    friend class ParticleViewCompiler;
    friend class ParticleReadView;
    friend class ParticleWriteView;

    usize m_value { usize::MAX };
};

template<typename Attribute>
class ParticleWriteIndex {
public:
    ParticleWriteIndex() = default;
    bool Valid() const noexcept { return m_value != usize::MAX; }

private:
    explicit ParticleWriteIndex(usize value): m_value(value) {}

    friend class ParticleViewCompiler;
    friend class ParticleWriteView;

    usize m_value { usize::MAX };
};

template<typename Attribute>
class ParticleReadObjectIndex {
public:
    ParticleReadObjectIndex() = default;
    bool Valid() const noexcept { return m_value != usize::MAX; }

private:
    explicit ParticleReadObjectIndex(usize value): m_value(value) {}

    friend class ParticleViewCompiler;
    friend class ParticleReadView;
    friend class ParticleWriteView;

    usize m_value { usize::MAX };
};

template<typename Attribute>
class ParticleWriteObjectIndex {
public:
    ParticleWriteObjectIndex() = default;
    bool Valid() const noexcept { return m_value != usize::MAX; }

private:
    explicit ParticleWriteObjectIndex(usize value): m_value(value) {}

    friend class ParticleViewCompiler;
    friend class ParticleWriteView;

    usize m_value { usize::MAX };
};

struct ParticleBoundColumn {
    void* object { nullptr };
    const void* (*read)(const void*) { nullptr };
    void* (*write)(void*) { nullptr };
};

struct ParticleViewColumnLayout {
    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };
    rstd::any::TypeId   concrete_type;
    ParticleBoundColumn (*bind)(ParticleStorage&, usize) { nullptr };
    bool readable { false };
    bool writable { false };
};

struct ParticleViewObjectLayout {
    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };
    rstd::any::TypeId   concrete_type;
    void* (*bind)(ParticleStorage&, usize) { nullptr };
    bool readable { false };
    bool writable { false };
};

class ParticleViewLayout {
public:
    ParticleViewLayout(const ParticleViewLayout&)                = delete;
    ParticleViewLayout& operator=(const ParticleViewLayout&)     = delete;
    ParticleViewLayout(ParticleViewLayout&&) noexcept            = default;
    ParticleViewLayout& operator=(ParticleViewLayout&&) noexcept = default;

    auto ColumnCount() const noexcept -> usize { return m_columns.len(); }
    auto ObjectCount() const noexcept -> usize { return m_objects.len(); }

private:
    friend class ParticleViewCompiler;
    friend class ParticleViewBinding;

    ParticleViewLayout(ParticleViewColumnLayout state, ParticleViewColumnLayout position)
        : m_state(rstd::move(state)), m_position(rstd::move(position)) {}

    ParticleViewColumnLayout      m_state;
    ParticleViewColumnLayout      m_position;
    Vec<ParticleViewColumnLayout> m_columns;
    Vec<ParticleViewObjectLayout> m_objects;
};

class ParticleViewCompiler {
public:
    explicit ParticleViewCompiler(const ParticleSchema& schema)
        : m_schema(rstd::addressof(schema)),
          m_layout(MakeColumnLayout(schema.SlotStateKey()),
                   MakeColumnLayout(schema.PositionKey())) {}

    void ReadBase(ParticleAttributeKey<SlotStateAttribute> key) {
        ValidateBase(key, m_schema->SlotStateKey(), m_layout.m_state, false);
    }
    void WriteBase(ParticleAttributeKey<SlotStateAttribute> key) {
        ValidateBase(key, m_schema->SlotStateKey(), m_layout.m_state, true);
    }
    void ReadBase(ParticleAttributeKey<PositionAttribute> key) {
        ValidateBase(key, m_schema->PositionKey(), m_layout.m_position, false);
    }
    void WriteBase(ParticleAttributeKey<PositionAttribute> key) {
        ValidateBase(key, m_schema->PositionKey(), m_layout.m_position, true);
    }

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) -> ParticleReadIndex<Attribute> {
        auto index = CompileColumn(key, false);
        return ParticleReadIndex<Attribute>(index);
    }

    template<typename Attribute>
    auto Write(ParticleAttributeKey<Attribute> key) -> ParticleWriteIndex<Attribute> {
        auto index = CompileColumn(key, true);
        return ParticleWriteIndex<Attribute>(index);
    }

    template<typename Attribute>
    auto ReadObject(ParticleAttributeKey<Attribute> key) -> ParticleReadObjectIndex<Attribute> {
        auto index = CompileObject(key, false);
        return ParticleReadObjectIndex<Attribute>(index);
    }

    template<typename Attribute>
    auto WriteObject(ParticleAttributeKey<Attribute> key) -> ParticleWriteObjectIndex<Attribute> {
        auto index = CompileObject(key, true);
        return ParticleWriteObjectIndex<Attribute>(index);
    }

    auto Finish() && -> Result<ParticleViewLayout, ParticleSchemaError> {
        if (m_error.is_some()) return Err(rstd::move(m_error).unwrap());
        return Ok(rstd::move(m_layout));
    }

private:
    template<typename Attribute>
    static auto ReadValues(const void* object) -> const void* {
        return static_cast<const Attribute*>(object)->Values().as_raw_ptr();
    }

    template<typename Attribute>
    static auto WriteValues(void* object) -> void* {
        return static_cast<Attribute*>(object)->ValuesMut().as_raw_ptr();
    }

    template<typename Attribute>
    static auto BindColumn(ParticleStorage& storage, usize schema_slot) -> ParticleBoundColumn {
        auto erased = storage.m_attributes[schema_slot].as_mut_ptr();
        auto object = static_cast<Attribute*>(erased.as_raw_ptr());
        return ParticleBoundColumn {
            .object = object,
            .read   = &ReadValues<Attribute>,
            .write  = &WriteValues<Attribute>,
        };
    }

    template<typename Attribute>
    static auto BindObject(ParticleStorage& storage, usize schema_slot) -> void* {
        auto erased = storage.m_attributes[schema_slot].as_mut_ptr();
        return static_cast<Attribute*>(erased.as_raw_ptr());
    }

    template<typename Attribute>
    static auto MakeColumnLayout(ParticleAttributeKey<Attribute> key) -> ParticleViewColumnLayout {
        return {
            .id            = key.id,
            .schema_slot   = key.schema_slot,
            .concrete_type = rstd::any::TypeId::of<Attribute>(),
            .bind          = &BindColumn<Attribute>,
        };
    }

    template<typename Attribute>
    void Validate(ParticleAttributeKey<Attribute> key) {
        if (m_error.is_some()) return;
        auto result = m_schema->Validate(RequireParticleAttribute(key));
        if (result.is_err()) m_error = Some(rstd::move(result).unwrap_err());
    }

    template<typename Attribute>
    void ValidateBase(ParticleAttributeKey<Attribute> key, ParticleAttributeKey<Attribute> expected,
                      ParticleViewColumnLayout& layout, bool write) {
        Validate(key);
        if (m_error.is_some()) return;
        if (key.id != expected.id || key.schema_slot != expected.schema_slot) {
            SetError("particle base attribute does not match schema"_str);
            return;
        }
        layout.readable = true;
        layout.writable = layout.writable || write;
    }

    bool IsBaseSlot(usize schema_slot) const noexcept {
        return schema_slot == m_layout.m_state.schema_slot ||
               schema_slot == m_layout.m_position.schema_slot;
    }

    template<typename Attribute>
    auto CompileColumn(ParticleAttributeKey<Attribute> key, bool write) -> usize {
        Validate(key);
        if (m_error.is_some()) return usize::MAX;
        if (IsBaseSlot(key.schema_slot)) {
            SetError("particle base attribute must use base view access"_str);
            return usize::MAX;
        }
        for (usize index {}; index < m_layout.m_columns.len(); ++index) {
            auto& column = m_layout.m_columns[index];
            if (column.schema_slot != key.schema_slot) continue;
            column.readable = true;
            column.writable = column.writable || write;
            return index;
        }
        auto layout     = MakeColumnLayout(key);
        layout.readable = true;
        layout.writable = write;
        auto index      = m_layout.m_columns.len();
        m_layout.m_columns.emplace_back(rstd::move(layout));
        return index;
    }

    template<typename Attribute>
    auto CompileObject(ParticleAttributeKey<Attribute> key, bool write) -> usize {
        Validate(key);
        if (m_error.is_some()) return usize::MAX;
        for (usize index {}; index < m_layout.m_objects.len(); ++index) {
            auto& object = m_layout.m_objects[index];
            if (object.schema_slot != key.schema_slot) continue;
            object.readable = true;
            object.writable = object.writable || write;
            return index;
        }
        auto index = m_layout.m_objects.len();
        m_layout.m_objects.emplace_back(ParticleViewObjectLayout {
            .id            = key.id,
            .schema_slot   = key.schema_slot,
            .concrete_type = rstd::any::TypeId::of<Attribute>(),
            .bind          = &BindObject<Attribute>,
            .readable      = true,
            .writable      = write,
        });
        return index;
    }

    void SetError(ref<str> message) {
        if (m_error.is_none()) {
            m_error = Some(ParticleSchemaError { .message = String::make(message) });
        }
    }

    const ParticleSchema*       m_schema;
    ParticleViewLayout          m_layout;
    Option<ParticleSchemaError> m_error;
};

class ParticleReadView {
public:
    auto Len() const noexcept -> usize { return m_len; }
    auto Generation() const noexcept -> u64 { return m_generation; }
    auto States() const noexcept -> slice<ParticleSlotState> {
        return slice<ParticleSlotState>::from_raw_parts(m_states, m_len);
    }
    auto Positions() const noexcept -> slice<Eigen::Vector3f> {
        return slice<Eigen::Vector3f>::from_raw_parts(m_positions, m_len);
    }

    template<typename Attribute>
    auto Read(ParticleReadIndex<Attribute> index) const -> slice<typename Attribute::Value> {
        if (! index.Valid() || index.m_value >= m_columns.len()) {
            rstd::panic { "invalid particle read index" };
        }
        return slice<typename Attribute::Value>::from_raw_parts(
            static_cast<const typename Attribute::Value*>(m_columns[index.m_value]), m_len);
    }

    template<typename Attribute>
    auto ReadObject(ParticleReadObjectIndex<Attribute> index) const -> ref<Attribute> {
        if (! index.Valid() || index.m_value >= m_objects.len()) {
            rstd::panic { "invalid particle object read index" };
        }
        return ref<Attribute>::from_raw_parts(
            static_cast<const Attribute*>(m_objects[index.m_value]));
    }

private:
    friend class ParticleViewBinding;

    ParticleReadView(usize len, u64 generation, const ParticleSlotState* states,
                     const Eigen::Vector3f* positions, slice<const void*> columns,
                     slice<const void*> objects)
        : m_len(len),
          m_generation(generation),
          m_states(states),
          m_positions(positions),
          m_columns(columns),
          m_objects(objects) {}

    usize                    m_len {};
    u64                      m_generation {};
    const ParticleSlotState* m_states { nullptr };
    const Eigen::Vector3f*   m_positions { nullptr };
    slice<const void*>       m_columns;
    slice<const void*>       m_objects;
};

class ParticleWriteView {
public:
    auto Len() const noexcept -> usize { return m_len; }
    auto Generation() const noexcept -> u64 { return m_generation; }
    auto States() const noexcept -> slice<ParticleSlotState> {
        return slice<ParticleSlotState>::from_raw_parts(m_states, m_len);
    }
    auto StatesMut() const noexcept -> mut_ref<ParticleSlotState[]> {
        return mut_ref<ParticleSlotState[]>::from_raw_parts(m_states, m_len);
    }
    auto Positions() const noexcept -> slice<Eigen::Vector3f> {
        return slice<Eigen::Vector3f>::from_raw_parts(m_positions, m_len);
    }
    auto PositionsMut() const noexcept -> mut_ref<Eigen::Vector3f[]> {
        return mut_ref<Eigen::Vector3f[]>::from_raw_parts(m_positions, m_len);
    }

    template<typename Attribute>
    auto Read(ParticleReadIndex<Attribute> index) const -> slice<typename Attribute::Value> {
        if (! index.Valid() || index.m_value >= m_columns.len()) {
            rstd::panic { "invalid particle read index" };
        }
        return slice<typename Attribute::Value>::from_raw_parts(
            static_cast<const typename Attribute::Value*>(m_columns[index.m_value]), m_len);
    }

    template<typename Attribute>
    auto Write(ParticleWriteIndex<Attribute> index) const -> mut_ref<typename Attribute::Value[]> {
        if (! index.Valid() || index.m_value >= m_columns.len()) {
            rstd::panic { "invalid particle write index" };
        }
        return mut_ref<typename Attribute::Value[]>::from_raw_parts(
            static_cast<typename Attribute::Value*>(m_columns[index.m_value]), m_len);
    }

    template<typename Attribute>
    auto ReadObject(ParticleReadObjectIndex<Attribute> index) const -> ref<Attribute> {
        if (! index.Valid() || index.m_value >= m_objects.len()) {
            rstd::panic { "invalid particle object read index" };
        }
        return ref<Attribute>::from_raw_parts(
            static_cast<const Attribute*>(m_objects[index.m_value]));
    }

    template<typename Attribute>
    auto WriteObject(ParticleWriteObjectIndex<Attribute> index) const -> mut_ref<Attribute> {
        if (! index.Valid() || index.m_value >= m_objects.len()) {
            rstd::panic { "invalid particle object write index" };
        }
        return mut_ref<Attribute>::from_raw_parts(
            static_cast<Attribute*>(m_objects[index.m_value]));
    }

private:
    friend class ParticleViewBinding;

    ParticleWriteView(usize len, u64 generation, ParticleSlotState* states,
                      Eigen::Vector3f* positions, slice<void*> columns, slice<void*> objects)
        : m_len(len),
          m_generation(generation),
          m_states(states),
          m_positions(positions),
          m_columns(columns),
          m_objects(objects) {}

    usize              m_len {};
    u64                m_generation {};
    ParticleSlotState* m_states { nullptr };
    Eigen::Vector3f*   m_positions { nullptr };
    slice<void*>       m_columns;
    slice<void*>       m_objects;
};

class ParticleViewBinding {
public:
    ParticleViewBinding(const ParticleViewLayout& layout, ParticleStorage& storage)
        : m_storage(rstd::addressof(storage)),
          m_state(layout.m_state.bind(storage, layout.m_state.schema_slot)),
          m_position(layout.m_position.bind(storage, layout.m_position.schema_slot)),
          m_columns(Vec<ParticleBoundColumn>::with_capacity(layout.m_columns.len())),
          m_read_columns(Vec<const void*>::with_capacity(layout.m_columns.len())),
          m_write_columns(Vec<void*>::with_capacity(layout.m_columns.len())),
          m_read_objects(Vec<const void*>::with_capacity(layout.m_objects.len())),
          m_write_objects(Vec<void*>::with_capacity(layout.m_objects.len())) {
        for (const auto& column : layout.m_columns) {
            m_columns.emplace_back(column.bind(storage, column.schema_slot));
        }
        for (const auto& object : layout.m_objects) {
            auto value = object.bind(storage, object.schema_slot);
            m_read_objects.emplace_back(value);
            m_write_objects.emplace_back(value);
        }
        Refresh();
    }

    auto Read() -> ParticleReadView {
        Refresh();
        return ParticleReadView(
            m_storage->Len(),
            m_storage->ColumnVersion(),
            static_cast<const ParticleSlotState*>(m_state.read(m_state.object)),
            static_cast<const Eigen::Vector3f*>(m_position.read(m_position.object)),
            m_read_columns.as_slice(),
            m_read_objects.as_slice());
    }

    auto Write() -> ParticleWriteView {
        Refresh();
        return ParticleWriteView(m_storage->Len(),
                                 m_storage->ColumnVersion(),
                                 static_cast<ParticleSlotState*>(m_state.write(m_state.object)),
                                 static_cast<Eigen::Vector3f*>(m_position.write(m_position.object)),
                                 m_write_columns.as_slice(),
                                 m_write_objects.as_slice());
    }

private:
    void Refresh() {
        auto version = m_storage->ColumnVersion();
        if (m_version == version && m_len == m_storage->Len()) return;

        m_read_columns.clear();
        m_write_columns.clear();
        for (auto& column : m_columns) {
            m_read_columns.emplace_back(column.read(column.object));
            m_write_columns.emplace_back(column.write(column.object));
        }
        m_version = version;
        m_len     = m_storage->Len();
    }

    ParticleStorage*         m_storage;
    ParticleBoundColumn      m_state;
    ParticleBoundColumn      m_position;
    Vec<ParticleBoundColumn> m_columns;
    Vec<const void*>         m_read_columns;
    Vec<void*>               m_write_columns;
    Vec<const void*>         m_read_objects;
    Vec<void*>               m_write_objects;
    u64                      m_version {};
    usize                    m_len { usize::MAX };
};

class ParticleSlotReader {
public:
    ParticleSlotReader(const ParticleStorage& storage, ParticleSlot slot)
        : m_storage(rstd::addressof(storage)), m_slot(slot), m_version(storage.StructureVersion()) {
        if (slot.index >= storage.Len()) rstd::panic { "particle slot out of bounds" };
    }

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> const typename Attribute::Value& {
        CheckVersion();
        return m_storage->Values(key)[m_slot.index];
    }

    auto Slot() const noexcept -> ParticleSlot { return m_slot; }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle slot reader used after structural mutation" };
        }
    }

    const ParticleStorage* m_storage;
    ParticleSlot           m_slot;
    u64                    m_version;
};

class ParticleSlotWriter {
public:
    ParticleSlotWriter(ParticleStorage& storage, ParticleSlot slot)
        : m_storage(rstd::addressof(storage)), m_slot(slot), m_version(storage.StructureVersion()) {
        if (slot.index >= storage.Len()) rstd::panic { "particle slot out of bounds" };
    }

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> const typename Attribute::Value& {
        CheckVersion();
        return m_storage->Values(key)[m_slot.index];
    }

    template<typename Attribute>
    auto Write(ParticleAttributeKey<Attribute> key) -> typename Attribute::Value& {
        CheckVersion();
        return m_storage->ValuesMut(key)[m_slot.index];
    }

    auto Slot() const noexcept -> ParticleSlot { return m_slot; }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle slot writer used after structural mutation" };
        }
    }

    ParticleStorage* m_storage;
    ParticleSlot     m_slot;
    u64              m_version;
};

inline ParticleSchemaBuilder::ParticleSchemaBuilder() {
    auto state =
        Register<SlotStateAttribute>("slot_state"_str, "framework"_str, ParticleSlotState {});
    if (state.is_err()) rstd::panic { "failed to register particle slot state" };
    m_slot_state_key = state.unwrap();

    auto position =
        Register<PositionAttribute>("position"_str, "framework"_str, Eigen::Vector3f::Zero());
    if (position.is_err()) rstd::panic { "failed to register particle position" };
    m_position_key = position.unwrap();
}

inline auto ParticleSchema::CreateStorage() const -> ParticleStorage {
    auto attributes = Vec<Box<dyn<ParticleAttribute>>>::with_capacity(m_factories.len());
    for (const auto& factory : m_factories) attributes.push(factory->Create());
    return ParticleStorage(rstd::move(attributes), m_slot_state_key, m_position_key);
}

} // namespace owe::particle

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::particle::ParticleSchemaError>
    : ImplBase<owe::particle::ParticleSchemaError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("{}", this->self().message));
    }
};

template<>
struct Impl<fmt::Debug, owe::particle::ParticleSchemaError>
    : ImplBase<owe::particle::ParticleSchemaError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(
            fmt::Arguments::make("ParticleSchemaError({})", this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::particle::ParticleSchemaError>
    : DefaultInImpl<error::Error, owe::particle::ParticleSchemaError> {};

} // namespace rstd

static_assert(rstd::Impled<owe::particle::ParticleSchemaError, rstd::error::Error>);
