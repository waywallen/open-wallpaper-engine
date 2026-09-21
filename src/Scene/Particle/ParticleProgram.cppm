export module wescene.particle.program;

import rstd;
import wescene.particle;

using namespace rstd::prelude;

export namespace owe::particle
{

class ParticleEmitterContext;
struct ParticleSpawnContext;
struct ParticleLifecycleContext;
struct ParticleEventContext;
struct ParticleUpdateContext;
struct ParticleExtractContext;

struct ParticleEmitterProgram {
    using Trait                  = ParticleEmitterProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleEmitterProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Emit(ParticleEmitterContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Emit>;
};

struct ParticleSpawnProgram {
    using Trait                  = ParticleSpawnProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleSpawnProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Initialize(ParticleSpawnContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Initialize>;
};

struct ParticleLifecycleProgram {
    using Trait                  = ParticleLifecycleProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleLifecycleProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Update(ParticleLifecycleContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Update>;
};

struct ParticleEventProgram {
    using Trait                  = ParticleEventProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleEventProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Process(ParticleEventContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Process>;
};

struct ParticleUpdateProgram {
    using Trait                  = ParticleUpdateProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleUpdateProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Update(ParticleUpdateContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Update>;
};

struct ParticleExtractProgram {
    using Trait                  = ParticleExtractProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleExtractProgram;

        void Compile(ParticleViewCompiler& compiler) { rstd::trait_call<0>(this, compiler); }
        void Extract(ParticleExtractContext& context) { rstd::trait_call<1>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Compile, &T::Extract>;
};

struct ParticleSlotTransition {
    ParticleSlot slot;
    bool         spawned { false };
    bool         died { false };
};

struct ParticleSlotEvents {
    Vec<ParticleSlot>           spawned;
    Vec<ParticleSlot>           died;
    Vec<ParticleSlotTransition> transitions;

    void RecordSpawn(ParticleSlot slot) {
        EnsureTransition(slot).spawned = true;
        spawned.emplace_back(slot);
    }

    void RecordDeath(ParticleSlot slot) {
        EnsureTransition(slot).died = true;
        died.emplace_back(slot);
    }

    void Clear() {
        spawned.clear();
        died.clear();
        transitions.clear();
    }

private:
    auto EnsureTransition(ParticleSlot slot) -> ParticleSlotTransition& {
        while (transitions.len() <= slot.index) {
            transitions.emplace_back(ParticleSlotTransition {
                .slot = ParticleSlot { .index = transitions.len() },
            });
        }
        return transitions[slot.index];
    }
};

enum class ParticleEventPhase
{
    BeforeEmit,
    AfterEmit,
};

struct ParticleSpawnRequest {
    ParticleSlot slot;
    f64          emitter_duration {};
};

struct ParticleSpawnContext {
    ParticleWriteView           view;
    slice<ParticleSpawnRequest> particles;
    ref<dyn<rstd::any::Any>>    frame;
};

struct ParticleLifecycleContext {
    ParticleWriteView        view;
    slice<ParticleSlot>      slots;
    ParticleSlotEvents&      events;
    ref<dyn<rstd::any::Any>> frame;
    f64                      delta {};
    f64                      elapsed {};

    void Kill(ParticleSlot slot) {
        auto states = view.StatesMut();
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        if (! states[slot.index].active) return;
        states[slot.index].active = false;
        states[slot.index].fresh  = false;
        events.RecordDeath(slot);
    }
};

struct ParticleEventContext {
    ParticleReadView         view;
    ref<ParticleSlotEvents>  events;
    ref<dyn<rstd::any::Any>> frame;
    ParticleEventPhase       phase;
    f64                      delta {};
    f64                      elapsed {};
};

struct ParticleUpdateContext {
    ParticleWriteView        view;
    slice<ParticleSlot>      slots;
    ref<dyn<rstd::any::Any>> frame;
    f64                      delta {};
    f64                      elapsed {};
};

class ParticleProgram {
public:
    void AddEmitter(Box<dyn<ParticleEmitterProgram>> program) {
        m_emitters.push(rstd::move(program));
    }
    void AddSpawn(Box<dyn<ParticleSpawnProgram>> program) { m_spawn.push(rstd::move(program)); }
    void AddLifecycle(Box<dyn<ParticleLifecycleProgram>> program) {
        m_lifecycle.push(rstd::move(program));
    }
    void AddEvent(Box<dyn<ParticleEventProgram>> program) { m_events.push(rstd::move(program)); }
    void AddUpdate(Box<dyn<ParticleUpdateProgram>> program) { m_updates.push(rstd::move(program)); }
    void AddPostUpdate(Box<dyn<ParticleUpdateProgram>> program) {
        m_post_updates.push(rstd::move(program));
    }
    void AddExtractor(Box<dyn<ParticleExtractProgram>> program) {
        m_extractors.push(rstd::move(program));
    }

private:
    friend class ParticleEmitterContext;
    friend class ParticleSystem;
    friend struct ParticleDefinition;

    Vec<Box<dyn<ParticleEmitterProgram>>>   m_emitters;
    Vec<Box<dyn<ParticleSpawnProgram>>>     m_spawn;
    Vec<Box<dyn<ParticleLifecycleProgram>>> m_lifecycle;
    Vec<Box<dyn<ParticleEventProgram>>>     m_events;
    Vec<Box<dyn<ParticleUpdateProgram>>>    m_updates;
    Vec<Box<dyn<ParticleUpdateProgram>>>    m_post_updates;
    Vec<Box<dyn<ParticleExtractProgram>>>   m_extractors;
};

struct ParticleDefinition {
    ParticleSchema     schema;
    ParticleProgram    program;
    ParticleViewLayout view_layout;
    usize              max_slots {};

    static auto Prepare(ParticleSchema schema, ParticleProgram program, usize max_slots)
        -> Result<ParticleDefinition, ParticleSchemaError> {
        ParticleViewCompiler compiler(schema);
        compiler.WriteBase(schema.SlotStateKey());
        compiler.WriteBase(schema.PositionKey());
        for (auto& emitter : program.m_emitters) emitter->Compile(compiler);
        for (auto& spawn : program.m_spawn) spawn->Compile(compiler);
        for (auto& lifecycle : program.m_lifecycle) lifecycle->Compile(compiler);
        for (auto& event : program.m_events) event->Compile(compiler);
        for (auto& update : program.m_updates) update->Compile(compiler);
        for (auto& update : program.m_post_updates) update->Compile(compiler);
        for (auto& extractor : program.m_extractors) extractor->Compile(compiler);
        auto layout = rstd::move(compiler).Finish();
        if (layout.is_err()) return Err(rstd::move(layout).unwrap_err());
        return Ok(ParticleDefinition {
            .schema      = rstd::move(schema),
            .program     = rstd::move(program),
            .view_layout = rstd::move(layout).unwrap(),
            .max_slots   = max_slots,
        });
    }
};

class ParticleInstance {
public:
    ParticleInstance(const ParticleSchema& schema, const ParticleViewLayout& layout)
        : m_storage(schema.CreateStorage()), m_binding(layout, m_storage) {}

    ParticleInstance(const ParticleInstance&)            = delete;
    ParticleInstance& operator=(const ParticleInstance&) = delete;
    ParticleInstance(ParticleInstance&&)                 = delete;
    ParticleInstance& operator=(ParticleInstance&&)      = delete;

    ParticleStorage&       Storage() noexcept { return m_storage; }
    const ParticleStorage& Storage() const noexcept { return m_storage; }
    ParticleViewBinding&   Binding() noexcept { return m_binding; }
    ParticleSlotEvents&    Events() noexcept { return m_events; }
    auto ActiveSlots() const noexcept -> slice<ParticleSlot> { return m_active_slots.as_slice(); }
    bool Active() const noexcept { return m_active; }
    void SetActive(bool active) noexcept { m_active = active; }

    void RefreshActiveSlots() {
        auto states = m_binding.Read().States();
        m_active_slots.clear();
        if (m_active_slots.capacity() < states.len()) {
            m_active_slots.reserve(states.len() - m_active_slots.len());
        }
        for (usize index {}; index < states.len(); ++index) {
            if (states[index].active) m_active_slots.emplace_back(ParticleSlot { .index = index });
        }
    }

    void Reset() {
        m_storage.Clear();
        m_events.Clear();
        m_active_slots.clear();
        m_active = true;
    }

private:
    ParticleStorage     m_storage;
    ParticleViewBinding m_binding;
    ParticleSlotEvents  m_events;
    Vec<ParticleSlot>   m_active_slots;
    bool                m_active { true };
};

struct ParticleExtractInstance {
    ParticleReadView    view;
    slice<ParticleSlot> slots;
    usize               instance_index {};
};

struct ParticleExtractContext {
    slice<ParticleExtractInstance> instances;
    ref<dyn<rstd::any::Any>>       frame;
};

class ParticleEmitterContext {
public:
    bool Empty() const noexcept { return m_storage->Len() == usize(); }
    auto Frame() const noexcept -> ref<dyn<rstd::any::Any>> { return m_frame; }
    f64  Delta() const noexcept { return m_delta; }
    f64  Elapsed() const noexcept { return m_elapsed; }

    auto Acquire(usize count, f64 emitter_duration) -> slice<ParticleSpawnRequest> {
        m_requests.clear();
        auto slots = m_storage->AcquireSlots(count, m_max_slots);
        for (auto slot : slots) {
            m_events->RecordSpawn(slot);
            m_requests.emplace_back(ParticleSpawnRequest {
                .slot             = slot,
                .emitter_duration = emitter_duration,
            });
        }
        return m_requests.as_slice();
    }

    auto View() -> ParticleWriteView { return m_binding->Write(); }

    void Initialize(slice<ParticleSpawnRequest> particles) {
        if (particles.is_empty()) return;
        ParticleSpawnContext context {
            .view      = m_binding->Write(),
            .particles = particles,
            .frame     = m_frame,
        };
        for (auto& program : *m_spawn) program->Initialize(context);
    }

private:
    friend class ParticleSystem;

    ParticleEmitterContext(ParticleStorage& storage, ParticleViewBinding& binding,
                           ParticleSlotEvents& events, Vec<Box<dyn<ParticleSpawnProgram>>>& spawn,
                           ref<dyn<rstd::any::Any>> frame, usize max_slots, f64 delta, f64 elapsed)
        : m_storage(rstd::addressof(storage)),
          m_binding(rstd::addressof(binding)),
          m_events(rstd::addressof(events)),
          m_spawn(rstd::addressof(spawn)),
          m_frame(frame),
          m_max_slots(max_slots),
          m_delta(delta),
          m_elapsed(elapsed) {}

    ParticleStorage*                     m_storage;
    ParticleViewBinding*                 m_binding;
    ParticleSlotEvents*                  m_events;
    Vec<Box<dyn<ParticleSpawnProgram>>>* m_spawn;
    ref<dyn<rstd::any::Any>>             m_frame;
    usize                                m_max_slots;
    f64                                  m_delta;
    f64                                  m_elapsed;
    Vec<ParticleSpawnRequest>            m_requests;
};

class ParticleSystem {
public:
    explicit ParticleSystem(ParticleDefinition definition): m_definition(rstd::move(definition)) {}

    auto CreateInstance() -> ParticleInstance& {
        m_instances.push(
            Box<ParticleInstance>::make(m_definition.schema, m_definition.view_layout));
        return *m_instances[m_instances.len() - usize(1)];
    }

    auto Instances() const noexcept -> slice<Box<ParticleInstance>> {
        return m_instances.as_slice();
    }

    auto InstancesMut() noexcept -> mut_ref<Box<ParticleInstance>[]> {
        return m_instances.deref_mut();
    }

    auto InstanceCount() const noexcept -> usize { return m_instances.len(); }

    ParticleInstance&       Instance(usize index) { return *m_instances[index]; }
    const ParticleInstance& Instance(usize index) const { return *m_instances[index]; }

    ParticleDefinition&       Definition() noexcept { return m_definition; }
    const ParticleDefinition& Definition() const noexcept { return m_definition; }

    void Advance(ParticleInstance& instance, ref<dyn<rstd::any::Any>> frame, f64 delta,
                 f64 elapsed) {
        if (! instance.Active()) return;

        auto& storage = instance.Storage();
        auto& binding = instance.Binding();
        auto& events  = instance.Events();
        events.Clear();
        instance.RefreshActiveSlots();

        ParticleLifecycleContext lifecycle_context {
            .view    = binding.Write(),
            .slots   = instance.ActiveSlots(),
            .events  = events,
            .frame   = frame,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& lifecycle : m_definition.program.m_lifecycle) {
            lifecycle->Update(lifecycle_context);
        }
        bool had_lifecycle_events = ! events.spawned.is_empty() || ! events.died.is_empty();
        ProcessEvents(
            binding, events, frame, ParticleEventPhase::BeforeEmit, delta, elapsed, false);
        events.Clear();

        ParticleEmitterContext emitter_context(storage,
                                               binding,
                                               events,
                                               m_definition.program.m_spawn,
                                               frame,
                                               m_definition.max_slots,
                                               delta,
                                               elapsed);
        for (auto& emitter : m_definition.program.m_emitters) emitter->Emit(emitter_context);
        ProcessEvents(binding,
                      events,
                      frame,
                      ParticleEventPhase::AfterEmit,
                      delta,
                      elapsed,
                      had_lifecycle_events);

        instance.RefreshActiveSlots();
        auto states = binding.Write().StatesMut();
        for (auto slot : instance.ActiveSlots()) states[slot.index].fresh = false;

        ParticleUpdateContext update_context {
            .view    = binding.Write(),
            .slots   = instance.ActiveSlots(),
            .frame   = frame,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& update : m_definition.program.m_updates) update->Update(update_context);
        for (auto& update : m_definition.program.m_post_updates) update->Update(update_context);
    }

    void Extract(ref<dyn<rstd::any::Any>> frame) {
        auto instances = Vec<ParticleExtractInstance>::with_capacity(m_instances.len());
        for (usize index {}; index < m_instances.len(); ++index) {
            auto& instance = *m_instances[index];
            instance.RefreshActiveSlots();
            instances.emplace_back(ParticleExtractInstance {
                .view           = instance.Binding().Read(),
                .slots          = instance.ActiveSlots(),
                .instance_index = index,
            });
        }
        ParticleExtractContext context {
            .instances = instances.as_slice(),
            .frame     = frame,
        };
        for (auto& extractor : m_definition.program.m_extractors) extractor->Extract(context);
    }

private:
    void ProcessEvents(ParticleViewBinding& binding, ParticleSlotEvents& events,
                       ref<dyn<rstd::any::Any>> frame, ParticleEventPhase phase, f64 delta,
                       f64 elapsed, bool force) {
        if (! force && events.spawned.is_empty() && events.died.is_empty()) return;
        ParticleEventContext context {
            .view    = binding.Read(),
            .events  = ref<ParticleSlotEvents>::from_raw_parts(rstd::addressof(events)),
            .frame   = frame,
            .phase   = phase,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& event : m_definition.program.m_events) event->Process(context);
    }

    ParticleDefinition         m_definition;
    Vec<Box<ParticleInstance>> m_instances;
};

} // namespace owe::particle
